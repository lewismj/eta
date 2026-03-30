#include "eta/semantics/semantic_analyzer.h"

#include <algorithm>
#include <array>
#include <utility>
#include <variant>
#include <unordered_map>
#include <string_view>

#include "eta/reader/parser.h"

namespace eta::semantics {

using namespace eta::reader::parser;

namespace {

// Forward declarations for handler registry
struct AnalysisContext;
SemResult<core::Node*> analyze(const SExprPtr& expr, Scope& scope, AnalysisContext& ctx);
SemResult<core::Address> lookup(const std::string& name, Scope* scope, AnalysisContext& ctx, Span span);

// Special form handler signature
using SpecialFormHandler = SemResult<core::Node*>(*)(const List*, Scope&, AnalysisContext&);

// Helper to wrap a list of body expressions into a single node.
// - Empty: returns Const with monostate (nil/void)
// - Single: returns that expression directly
// - Multiple: wraps in Begin
core::Node* wrap_body(std::vector<core::Node*> body_exprs, Span span, ModuleSemantics& mod) {
    if (body_exprs.empty()) {
        return mod.emplace<core::Const>(core::Literal{std::monostate{}}, span);
    } else if (body_exprs.size() == 1) {
        return body_exprs[0];
    } else {
        return mod.emplace<core::Begin>(std::move(body_exprs), false, span);
    }
}

struct AnalysisContext {
    ModuleSemantics& mod;
    uint32_t next_binding_id{0};

    core::BindingId next_id() { return core::BindingId{next_binding_id++}; }

    core::BindingId add_binding(Scope& scope, const std::string& name, BindingInfo::Kind kind, Span span, bool mutable_flag = false) {
        core::BindingId id = next_id();
        scope.table[name] = id;
        mod.bindings.push_back(BindingInfo{kind, name, mutable_flag, span});
        return id;
    }

    core::BindingId add_import(Scope& scope, const std::string& name, const eta::reader::linker::ImportOrigin& origin, Span span) {
        core::BindingId id = next_id();
        scope.table[name] = id;
        mod.bindings.push_back(BindingInfo{BindingInfo::Kind::Import, name, false, span, origin});
        return id;
    }
};

// ============================================================================
// Scope Lookup
// ============================================================================


SemResult<core::Address> lookup(const std::string& name, Scope* scope, AnalysisContext& ctx, Span span) {
    int crosses_lambda = 0;
    Scope* current = scope;
    std::vector<core::Lambda*> path;

    while (current) {
        if (auto it = current->table.find(name); it != current->table.end()) {
            core::BindingId id = it->second;
            const auto& info = ctx.mod.bindings[id.id];

            if (info.kind == BindingInfo::Kind::Global || info.kind == BindingInfo::Kind::Import) {
                return core::Address{core::Address::Global{id.id}};
            }

            if (crosses_lambda > 0) {
                uint16_t slot = 0;
                core::BindingId current_id = id;
                for (auto it_path = path.rbegin(); it_path != path.rend(); ++it_path) {
                    auto* lam = *it_path;
                    auto it_up = std::find(lam->upvals.begin(), lam->upvals.end(), current_id);
                    if (it_up == lam->upvals.end()) {
                        slot = static_cast<uint16_t>(lam->upvals.size());
                        lam->upvals.push_back(current_id);
                    } else {
                        slot = static_cast<uint16_t>(std::distance(lam->upvals.begin(), it_up));
                    }
                }
                return core::Address{core::Address::Upval{static_cast<uint16_t>(crosses_lambda), slot}};
            }
            return core::Address{core::Address::Local{static_cast<uint16_t>(id.id)}};
        }
        
        if (current->is_lambda_boundary) {
            crosses_lambda++;
            if (current->lambda_node) path.push_back(current->lambda_node);
        }
        current = current->parent;
    }
    return std::unexpected(SemanticError{SemanticError::Kind::UndefinedName, span, "Undefined symbol: " + name});
}

SemResult<core::Node*> analyze(const SExprPtr& expr, Scope& scope, AnalysisContext& ctx);

// ============================================================================
// Special Form Handlers - Each handles a specific core or derived form
// ============================================================================

SemResult<core::Node*> handle_if(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.size() != 4)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "if requires 3 args"});
    auto test = analyze(lst->elems[1], scope, ctx);
    if (!test) return test;
    auto conseq = analyze(lst->elems[2], scope, ctx);
    if (!conseq) return conseq;
    auto alt = analyze(lst->elems[3], scope, ctx);
    if (!alt) return alt;
    return ctx.mod.emplace<core::If>(*test, *conseq, *alt, false, lst->span);
}

SemResult<core::Node*> handle_begin(const List* lst, Scope& scope, AnalysisContext& ctx) {
    std::vector<core::Node*> exprs;
    for (size_t i = 1; i < lst->elems.size(); ++i) {
        auto res = analyze(lst->elems[i], scope, ctx);
        if (!res) return res;
        exprs.push_back(*res);
    }
    return ctx.mod.emplace<core::Begin>(std::move(exprs), false, lst->span);
}

SemResult<core::Node*> handle_set(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.size() != 3)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "set! requires 2 args"});
    const auto* target_sym = lst->elems[1]->as<Symbol>();
    if (!target_sym)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[1]->span(), "set! target must be a symbol"});

    auto addr = lookup(target_sym->name, &scope, ctx, target_sym->span);
    if (!addr) return std::unexpected(addr.error());

    // Check mutability and distinguish between set on import vs immutable local
    auto check_mutable = [&](const core::Address& a) -> std::optional<SemanticError::Kind> {
        uint32_t binding_id = UINT32_MAX;
        if (auto* l = std::get_if<core::Address::Local>(&a.where)) {
            binding_id = l->slot;
        } else if (auto* g = std::get_if<core::Address::Global>(&a.where)) {
            binding_id = g->id;
        } else if (auto* u = std::get_if<core::Address::Upval>(&a.where)) {
            if (scope.lambda_node && u->slot < scope.lambda_node->upvals.size()) {
                binding_id = scope.lambda_node->upvals[u->slot].id;
            }
        }

        if (binding_id < ctx.mod.bindings.size()) {
            const auto& info = ctx.mod.bindings[binding_id];
            if (!info.mutable_flag) {
                return info.kind == BindingInfo::Kind::Import
                    ? SemanticError::Kind::SetOnImported
                    : SemanticError::Kind::ImmutableAssignment;
            }
        }
        return std::nullopt; // mutable, ok
    };

    if (auto err_kind = check_mutable(*addr)) {
        std::string msg = (*err_kind == SemanticError::Kind::SetOnImported)
            ? "Cannot set! imported binding"
            : "Cannot set! immutable binding";
        return std::unexpected(SemanticError{*err_kind, target_sym->span, msg});
    }

    auto val = analyze(lst->elems[2], scope, ctx);
    if (!val) return val;
    return ctx.mod.emplace<core::Set>(*addr, *val, lst->span);
}

SemResult<core::Node*> handle_lambda(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.size() < 3)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "lambda requires formals and body"});

    auto* lam_node = ctx.mod.emplace<core::Lambda>();
    auto* lam = std::get_if<core::Lambda>(static_cast<core::NodeBase*>(lam_node));
    lam->span = lst->span;

    Scope lambda_scope{&scope};
    lambda_scope.is_lambda_boundary = true;
    lambda_scope.lambda_node = lam;

    const auto* formals_lst = lst->elems[1]->as<List>();
    const auto* formal_sym = lst->elems[1]->as<Symbol>();
    const auto* formal_nil = lst->elems[1]->as<Nil>();

    if (formal_sym) {
        auto id = ctx.add_binding(lambda_scope, formal_sym->name, BindingInfo::Kind::Param, formal_sym->span, true);
        lam->rest = id;
        lam->arity.has_rest = true;
    } else if (formal_nil) {
        lam->arity.required = 0;
    } else if (formals_lst) {
        for (const auto& arg : formals_lst->elems) {
            if (const auto* s = arg->as<Symbol>()) {
                auto id = ctx.add_binding(lambda_scope, s->name, BindingInfo::Kind::Param, s->span, true);
                lam->params.push_back(id);
                lam->arity.required++;
            } else {
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, arg->span(), "lambda formal must be symbol"});
            }
        }
        if (formals_lst->dotted) {
            if (const auto* s = formals_lst->tail->as<Symbol>()) {
                auto id = ctx.add_binding(lambda_scope, s->name, BindingInfo::Kind::Param, s->span, true);
                lam->rest = id;
                lam->arity.has_rest = true;
            } else {
                 return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, formals_lst->tail->span(), "lambda rest formal must be symbol"});
            }
        }
    } else {
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[1]->span(), "invalid lambda formals"});
    }

    std::vector<core::Node*> body_exprs;
    for (size_t i = 2; i < lst->elems.size(); ++i) {
        auto res = analyze(lst->elems[i], lambda_scope, ctx);
        if (!res) return res;
        body_exprs.push_back(*res);
    }

    lam->body = wrap_body(std::move(body_exprs), lst->span, ctx.mod);
    return lam_node;
}

SemResult<core::Node*> handle_quote(const List* lst, Scope&, AnalysisContext& ctx) {
    if (lst->elems.size() != 2)
         return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "quote requires 1 arg"});
    return ctx.mod.emplace<core::Quote>(std::shared_ptr<SExpr>(lst->elems[1].get(), [](SExpr*){}), lst->span);
}

// Helper to analyze a fixed number of arguments from a form.
// Returns analyzed nodes or error if arity mismatch or analysis fails.
template<std::size_t N>
SemResult<std::array<core::Node*, N>> analyze_n_args(
    const List* lst, Scope& scope, AnalysisContext& ctx, const char* form_name) {
    if (lst->elems.size() != N + 1) {
        std::string msg = std::string(form_name) + " requires " + std::to_string(N) + " arg(s)";
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, msg});
    }
    std::array<core::Node*, N> result;
    for (std::size_t i = 0; i < N; ++i) {
        auto r = analyze(lst->elems[i + 1], scope, ctx);
        if (!r) return std::unexpected(r.error());
        result[i] = *r;
    }
    return result;
}

SemResult<core::Node*> handle_dynamic_wind(const List* lst, Scope& scope, AnalysisContext& ctx) {
    auto args = analyze_n_args<3>(lst, scope, ctx, "dynamic-wind");
    if (!args) return std::unexpected(args.error());
    return ctx.mod.emplace<core::DynamicWind>((*args)[0], (*args)[1], (*args)[2], lst->span);
}

SemResult<core::Node*> handle_values(const List* lst, Scope& scope, AnalysisContext& ctx) {
    std::vector<core::Node*> exprs;
    for (size_t i = 1; i < lst->elems.size(); ++i) {
        auto res = analyze(lst->elems[i], scope, ctx);
        if (!res) return res;
        exprs.push_back(*res);
    }
    return ctx.mod.emplace<core::Values>(std::move(exprs), lst->span);
}

SemResult<core::Node*> handle_call_with_values(const List* lst, Scope& scope, AnalysisContext& ctx) {
    auto args = analyze_n_args<2>(lst, scope, ctx, "call-with-values");
    if (!args) return std::unexpected(args.error());
    return ctx.mod.emplace<core::CallWithValues>((*args)[0], (*args)[1], false, lst->span);
}

SemResult<core::Node*> handle_call_cc(const List* lst, Scope& scope, AnalysisContext& ctx) {
    auto args = analyze_n_args<1>(lst, scope, ctx, "call/cc");
    if (!args) return std::unexpected(args.error());
    return ctx.mod.emplace<core::CallCC>((*args)[0], false, lst->span);
}

// ============================================================================
// NOTE: Derived form handlers (let, letrec, case, do) have been REMOVED.
// These forms MUST be desugared by the Expander before semantic analysis.
// The Expander desugars:
//   let      -> ((lambda (x...) body...) init...)
//   let*     -> nested let
//   letrec   -> (let ((x '()) ...) (set! x e) ... body...)
//   letrec*  -> nested letrec
//   cond     -> nested if
//   case     -> (let ((tmp key)) (if (or (eqv? tmp d1) ...) body ...))
//   do       -> (letrec ((loop (lambda (x...) (if test result (begin body (loop step...)))))) (loop init...))
//   and/or   -> nested if
//   when/unless -> (if test (begin body...) (begin))
// ============================================================================

// Handler registry - maps form names to handler functions
const std::unordered_map<std::string_view, SpecialFormHandler>& get_form_handlers() {
    static const std::unordered_map<std::string_view, SpecialFormHandler> handlers = {
        // Core forms (required - these are the primitive forms that cannot be desugared)
        {"if",                              handle_if},
        {"begin",                           handle_begin},
        {"set!",                            handle_set},
        {"lambda",                          handle_lambda},
        {"quote",                           handle_quote},
        {"dynamic-wind",                    handle_dynamic_wind},
        {"values",                          handle_values},
        {"call-with-values",                handle_call_with_values},
        {"call/cc",                         handle_call_cc},
        {"call-with-current-continuation",  handle_call_cc},
        // NOTE: Derived forms (let, letrec, case, do) are no longer handled here.
        // They MUST be desugared by the Expander. If encountered, they will be
        // treated as function applications, which will produce a meaningful error.
    };
    return handlers;
}

/**
 * Analyze a list form (application or special form).
 * Uses handler registry for extensibility and maintainability.
 */
SemResult<core::Node*> analyze_list(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.empty()) {
        core::Node* n = ctx.mod.emplace<core::Const>();
        auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(n));
        c->value.payload = std::monostate{};
        c->span = lst->span;
        return n;
    }

    // Check for special forms
    if (const auto* head = lst->elems[0]->as<Symbol>()) {
        const auto& handlers = get_form_handlers();
        auto it = handlers.find(head->name);
        if (it != handlers.end()) {
            return it->second(lst, scope, ctx);
        }
    }

    // Generic application
    auto callee = analyze(lst->elems[0], scope, ctx);
    if (!callee) return callee;
    std::vector<core::Node*> args;
    for (size_t i = 1; i < lst->elems.size(); ++i) {
        auto arg = analyze(lst->elems[i], scope, ctx);
        if (!arg) return arg;
        args.push_back(*arg);
    }
    return ctx.mod.emplace<core::Call>(*callee, std::move(args), false, lst->span);
}


SemResult<core::Node*> analyze(const SExprPtr& expr, Scope& scope, AnalysisContext& ctx) {
    if (auto* sym = expr->as<Symbol>()) {
        auto addr = lookup(sym->name, &scope, ctx, sym->span);
        if (!addr) return std::unexpected(addr.error());
        return ctx.mod.emplace<core::Var>(*addr, sym->span);
    }
    if (auto* b = expr->as<Bool>()) {
        core::Node* n = ctx.mod.emplace<core::Const>();
        auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(n));
        c->value.payload = b->value; c->span = b->span;
        return n;
    }
    if (auto* ch = expr->as<Char>()) {
        core::Node* n = ctx.mod.emplace<core::Const>();
        auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(n));
        c->value.payload = ch->value; c->span = ch->span;
        return n;
    }
    if (auto* s = expr->as<String>()) {
        core::Node* n = ctx.mod.emplace<core::Const>();
        auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(n));
        c->value.payload = s->value; c->span = s->span;
        return n;
    }
    if (auto* nil = expr->as<Nil>()) {
        core::Node* n = ctx.mod.emplace<core::Const>();
        auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(n));
        c->value.payload = std::monostate{}; c->span = nil->span;
        return n;
    }

    if (auto* num = expr->as<eta::reader::parser::Number>()) {
        core::Node* n = ctx.mod.emplace<core::Const>();
        auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(n));
        // Store int64_t or double directly in the Literal variant
        std::visit([&](auto&& v) {
            c->value.payload = v;
        }, num->value);
        c->span = num->span;
        return n;
    }

    if (auto* lst = expr->as<List>()) return analyze_list(lst, scope, ctx);
    return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, expr->span(), "Unsupported expression type"});
}

// ============================================================================
// Generic IR Visitor - Centralizes traversal over all IR node types
// ============================================================================

/**
 * Generic IR visitor that visits all children of a node.
 * Subclasses can customize pre/post visit behavior while reusing traversal logic.
 *
 * Template parameters:
 *   - Derived: CRTP derived class
 *   - R: Return type for transforming visitors (use void for simple visitors)
 */
template<typename Derived, typename R = void>
struct IRVisitor {
    // Called before visiting children - derived class can override
    // For mark_tail: sets the tail flag
    // For constant_fold: no-op (folding happens after children)
    void pre_visit(core::Node*, bool) {}

    // Called after visiting children - derived class can override
    // For constant_fold: performs the actual folding
    // Returns potentially transformed node (for folding) or same node
    core::Node* post_visit(core::Node* n, bool) { return n; }

    // Main entry point - visits node and all children
    core::Node* visit(core::Node* node, bool context) {
        if (!node) return nullptr;

        auto* derived = static_cast<Derived*>(this);
        derived->pre_visit(node, context);

        std::visit([&](auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, core::If>) {
                val.test = derived->visit(val.test, false);
                val.conseq = derived->visit(val.conseq, context);
                val.alt = derived->visit(val.alt, context);
            } else if constexpr (std::is_same_v<T, core::Begin>) {
                for (std::size_t i = 0; i < val.exprs.size(); ++i) {
                    bool is_last = (i + 1 == val.exprs.size());
                    val.exprs[i] = derived->visit(val.exprs[i], context && is_last);
                }
            } else if constexpr (std::is_same_v<T, core::Lambda>) {
                // Lambda bodies are always in tail context
                val.body = derived->visit(val.body, true);
            } else if constexpr (std::is_same_v<T, core::Call>) {
                val.callee = derived->visit(val.callee, false);
                for (auto*& a : val.args) {
                    a = derived->visit(a, false);
                }
            } else if constexpr (std::is_same_v<T, core::Set>) {
                val.value = derived->visit(val.value, false);
            } else if constexpr (std::is_same_v<T, core::DynamicWind>) {
                val.before = derived->visit(val.before, false);
                val.body = derived->visit(val.body, false);
                val.after = derived->visit(val.after, false);
            } else if constexpr (std::is_same_v<T, core::Values>) {
                for (auto*& e : val.exprs) {
                    e = derived->visit(e, false);
                }
            } else if constexpr (std::is_same_v<T, core::CallWithValues>) {
                val.producer = derived->visit(val.producer, false);
                val.consumer = derived->visit(val.consumer, false);
            } else if constexpr (std::is_same_v<T, core::CallCC>) {
                val.consumer = derived->visit(val.consumer, false);
            }
            // core::Const, core::Var, core::Quote have no children
        }, static_cast<core::NodeBase&>(*node));

        return derived->post_visit(node, context);
    }
};

/**
 * Marks nodes with their tail context position.
 * Uses IRVisitor to avoid duplicating traversal logic.
 */
struct TailMarker : IRVisitor<TailMarker> {
    void pre_visit(core::Node* node, bool in_tail_context) {
        std::visit([in_tail_context](auto& val) {
            using T = std::decay_t<decltype(val)>;
            // Only set tail flag on nodes that have one
            if constexpr (std::is_same_v<T, core::If> ||
                          std::is_same_v<T, core::Begin> ||
                          std::is_same_v<T, core::Call> ||
                          std::is_same_v<T, core::CallWithValues>) {
                val.tail = in_tail_context;
            }
        }, static_cast<core::NodeBase&>(*node));
    }
};

inline void mark_tail(core::Node* node, bool in_tail_context) {
    TailMarker marker;
    marker.visit(node, in_tail_context);
}

} // namespace

SemResult<std::vector<ModuleSemantics>>
SemanticAnalyzer::analyze_all(std::span<const SExprPtr> forms, const eta::reader::ModuleLinker& linker) {
    std::vector<ModuleSemantics> out;
    for (const auto& f : forms) {
        if (!f || !f->is<List>()) continue;
        const auto* lst = f->as<List>();
        if (lst->elems.size() < 2) continue;
        const auto* h = lst->elems[0] ? lst->elems[0]->as<Symbol>() : nullptr;
        if (!h || h->name != "module") continue;
        const auto* ns = lst->elems[1] ? lst->elems[1]->as<Symbol>() : nullptr;
        if (!ns) continue;
        auto mtref = linker.get(ns->name);
        if (!mtref || mtref->get().state != eta::reader::ModuleState::Linked)
            return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, f->span(), "module not linked"});

        ModuleSemantics mod; mod.name = ns->name;
        Scope toplevel{}; AnalysisContext ctx{mod, 0};
        for (const auto& [ln, orign] : mtref->get().import_origins) ctx.add_import(toplevel, ln, orign, orign.where);
        for (const auto& nm : mtref->get().defined) {
            auto it = mtref->get().define_spans.find(nm);
            auto sp = (it != mtref->get().define_spans.end()) ? it->second : f->span();
            ctx.add_binding(toplevel, nm, BindingInfo::Kind::Global, sp, true);
        }

        for (size_t i = 2; i < lst->elems.size(); ++i) {
            const auto& bf = lst->elems[i];
            if (auto* l = bf->as<List>()) {
                if (!l->elems.empty()) {
                    if (auto* hs = l->elems[0]->as<Symbol>()) {
                        if (hs->name == "export" || hs->name == "import") continue;
                        if (hs->name == "define" && l->elems.size() == 3) {
                            if (auto* vs = l->elems[1]->as<Symbol>()) {
                                auto val = analyze(l->elems[2], toplevel, ctx); 
                                if (!val) return std::unexpected(val.error());
                                auto ar = lookup(vs->name, &toplevel, ctx, vs->span); 
                                if (!ar) return std::unexpected(ar.error());
                                mod.toplevel_inits.push_back(ctx.mod.emplace<core::Set>(*ar, *val, bf->span()));
                                continue;
                            }
                        }
                    }
                }
            }
            auto res = analyze(bf, toplevel, ctx); 
            if (!res) return std::unexpected(res.error());
            mod.toplevel_inits.push_back(*res);
        }
        for (auto* n : mod.toplevel_inits) mark_tail(n, false);
        for (const auto& ex : mtref->get().exports) {
            auto it = toplevel.table.find(ex);
            if (it == toplevel.table.end()) return std::unexpected(SemanticError{SemanticError::Kind::ExportOfUnknownBinding, f->span(), "unknown export"});
            mod.exports.emplace(ex, it->second);
        }
        out.push_back(std::move(mod));
    }
    return out;
}

void SemanticAnalyzer::constant_fold(ModuleSemantics& mod) {
    /**
     * Constant folder using IRVisitor.
     * Folds constant conditionals in core::If nodes after visiting children.
     */
    struct ConstantFolder : IRVisitor<ConstantFolder> {
        core::Node* result = nullptr;  // Used to propagate folded result

        core::Node* post_visit(core::Node* n, bool /*context*/) {
            // Check if this is an If node with a constant test
            if (auto* if_node = std::get_if<core::If>(static_cast<core::NodeBase*>(n))) {
                if (auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(if_node->test))) {
                    if (auto* b = std::get_if<bool>(&c->value.payload)) {
                        return *b ? if_node->conseq : if_node->alt;
                    } else if (!std::holds_alternative<std::monostate>(c->value.payload)) {
                        // Any non-false constant is true in Scheme
                        return if_node->conseq;
                    }
                }
            }
            return n;
        }
    };

    ConstantFolder folder;
    for (auto*& node : mod.toplevel_inits) {
        node = folder.visit(node, false);
    }
}

} // namespace eta::semantics
