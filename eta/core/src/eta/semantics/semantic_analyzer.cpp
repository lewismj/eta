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

struct AnalysisContext {
    ModuleSemantics& mod;
    uint32_t next_binding_id{0};
    uint32_t* shared_next_global{nullptr};  ///< Unified global slot counter (shared across modules)

    core::BindingId next_id() { return core::BindingId{next_binding_id++}; }

    /// Allocate the next unified global slot
    uint32_t alloc_global_slot() {
        return (*shared_next_global)++;
    }

    core::BindingId add_binding(Scope& scope, const std::string& name, BindingInfo::Kind kind, Span span, bool mutable_flag = false) {
        core::BindingId id = next_id();
        scope.table[name] = id;
        
        std::uint16_t slot = 0;
        if (kind == BindingInfo::Kind::Param || kind == BindingInfo::Kind::Local) {
            /// Find the lambda frame this binding belongs to
            Scope* s = &scope;
            while (s && !s->is_lambda_boundary && s->parent) s = s->parent;
            if (s) slot = s->next_slot++;
        } else {
            /// Global: allocate from the unified counter
            slot = static_cast<std::uint16_t>(alloc_global_slot());
        }

        mod.bindings.push_back(BindingInfo{kind, name, mutable_flag, slot, span, std::nullopt});
        return id;
    }

    /// Add an import that reuses an existing global slot (from the exporting module)
    core::BindingId add_import_at_slot(Scope& scope, const std::string& name,
                                       const eta::reader::linker::ImportOrigin& origin,
                                       Span span, uint32_t global_slot) {
        core::BindingId id = next_id();
        scope.table[name] = id;
        /**
         * REPL/Jupyter submissions are wrapped as synthetic modules (__repl_N).
         * To preserve interactive semantics, names imported from prior REPL
         * modules must remain assignable via set! across submissions.
         */
        const bool mutable_import = origin.from_module.rfind("__repl_", 0) == 0;
        mod.bindings.push_back(BindingInfo{BindingInfo::Kind::Import, name, mutable_import,
                                           static_cast<std::uint16_t>(global_slot), span, origin});
        return id;
    }

    /// Legacy: add an import with a fresh global slot (used when no export slot is known)
    core::BindingId add_import(Scope& scope, const std::string& name,
                               const eta::reader::linker::ImportOrigin& origin, Span span) {
        return add_import_at_slot(scope, name, origin, span, alloc_global_slot());
    }
};

SemResult<core::Node*> analyze(const SExprPtr& expr, Scope& scope, AnalysisContext& ctx);

struct LookupResult {
    core::Address addr;
    core::BindingId id;
};

using eta::reader::parser::deep_copy;

/**
 * Create a core::Const IR node from an S-expression.
 * Centralizes the creation and initialization of constant values.
 */
core::Node* make_const(const SExprPtr& expr, AnalysisContext& ctx) {
    core::Literal lit;
    if (expr->is<Nil>()) {
        lit.payload = std::monostate{};
    } else if (const auto* b = expr->as<Bool>()) {
        lit.payload = b->value;
    } else if (const auto* c = expr->as<Char>()) {
        lit.payload = c->value;
    } else if (const auto* s = expr->as<String>()) {
        lit.payload = s->value;
    } else if (const auto* n = expr->as<eta::reader::parser::Number>()) {
        std::visit([&](auto&& v) { lit.payload = v; }, n->value);
    } else if (const auto* lst = expr->as<List>()) {
        if (lst->elems.empty()) {
            lit.payload = std::monostate{};
        } else {
            return nullptr;
        }
    } else {
        return nullptr;
    }
    return ctx.mod.emplace<core::Const>(expr->span(), lit);
}

/**
 * Parse lambda formal parameters and populate the lambda node.
 * Handles symbols, nil, lists, and dotted lists.
 */
SemResult<void> parse_formals(const SExprPtr& expr, Scope& scope, AnalysisContext& ctx, core::Lambda* lam) {
    if (const auto* formal_sym = expr->as<Symbol>()) {
        auto id = ctx.add_binding(scope, formal_sym->name, BindingInfo::Kind::Param, formal_sym->span, true);
        lam->rest = id;
        lam->arity.has_rest = true;
    } else if (expr->is<Nil>()) {
        lam->arity.required = 0;
    } else if (const auto* formals_lst = expr->as<List>()) {
        for (const auto& arg : formals_lst->elems) {
            if (const auto* s = arg->as<Symbol>()) {
                auto id = ctx.add_binding(scope, s->name, BindingInfo::Kind::Param, s->span, true);
                lam->params.push_back(id);
                lam->arity.required++;
            } else {
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, arg->span(), "lambda formal must be symbol"});
            }
        }
        if (formals_lst->dotted) {
            if (const auto* s = formals_lst->tail->as<Symbol>()) {
                auto id = ctx.add_binding(scope, s->name, BindingInfo::Kind::Param, s->span, true);
                lam->rest = id;
                lam->arity.has_rest = true;
            } else {
                 return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, formals_lst->tail->span(), "lambda rest formal must be symbol"});
            }
        }
    } else {
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, expr->span(), "invalid lambda formals"});
    }
    return {};
}

/**
 * @brief Helper to wrap a list of body expressions into a single node.
 * - Empty: returns Const with monostate (nil/void)
 * - Single: returns that expression directly
 * - Multiple: wraps in Begin
 */
core::Node* wrap_body(std::vector<core::Node*> body_exprs, Span span, ModuleSemantics& mod) {
    if (body_exprs.empty()) {
        return mod.emplace<core::Const>(span, core::Literal{std::monostate{}});
    } else if (body_exprs.size() == 1) {
        return body_exprs[0];
    } else {
        return mod.emplace<core::Begin>(span, std::move(body_exprs));
    }
}

/// Special form handler signature
using SpecialFormHandler = SemResult<core::Node*>(*)(const List*, Scope&, AnalysisContext&);

SemResult<LookupResult> lookup(const std::string& name, Scope* scope, AnalysisContext& ctx, Span span) {
    int crosses_lambda = 0;
    Scope* current = scope;
    std::vector<core::Lambda*> path;

    auto lookup_toplevel = [&](std::string_view base) -> std::optional<LookupResult> {
        Scope* root = scope;
        while (root && root->parent) root = root->parent;
        if (!root) return std::nullopt;

        auto it = root->table.find(std::string(base));
        if (it == root->table.end()) return std::nullopt;

        core::BindingId id = it->second;
        const auto& info = ctx.mod.bindings[id.id];
        if (info.kind != BindingInfo::Kind::Global &&
            info.kind != BindingInfo::Kind::Import) {
            return std::nullopt;
        }
        return LookupResult{core::Address{core::Address::Global{info.slot}}, id};
    };

    while (current) {
        if (auto it = current->table.find(name); it != current->table.end()) {
            core::BindingId id = it->second;
            const auto& info = ctx.mod.bindings[id.id];

            if (info.kind == BindingInfo::Kind::Global || info.kind == BindingInfo::Kind::Import) {
                return LookupResult{core::Address{core::Address::Global{info.slot}}, id};
            }

            if (crosses_lambda > 0) {
                /// Lexical local or parameter from outer scope
                core::Address current_addr = core::Address{core::Address::Local{info.slot}};

                /// Trace through each lambda boundary
                for (auto it_path = path.rbegin(); it_path != path.rend(); ++it_path) {
                    auto* lam = *it_path;
                    auto it_up = std::find(lam->upvals.begin(), lam->upvals.end(), id);
                    uint16_t slot = 0;
                    if (it_up == lam->upvals.end()) {
                        slot = static_cast<uint16_t>(lam->upvals.size());
                        lam->upvals.push_back(id);
                        lam->upval_sources.push_back(current_addr);
                    } else {
                        slot = static_cast<uint16_t>(std::distance(lam->upvals.begin(), it_up));
                    }
                    /// The address for the next (inner) lambda is this lambda's upval
                    current_addr = core::Address{core::Address::Upval{slot}};
                }
                
                return LookupResult{current_addr, id};
            }
            return LookupResult{core::Address{core::Address::Local{info.slot}}, id};
        }
        
        if (current->is_lambda_boundary) {
            crosses_lambda++;
            if (current->lambda_node) path.push_back(current->lambda_node);
        }
        current = current->parent;
    }

    /**
     * Hygienic macro references may encode definition-site capture as
     * "<base>.def:<token>". If the encoded alias is not present locally,
     * fall back to the top-level/import binding for <base> so lexical locals
     * at the use site cannot capture the reference.
     */
    if (const auto marker = name.find(".def:"); marker != std::string::npos) {
        if (auto fallback = lookup_toplevel(name.substr(0, marker)); fallback) {
            return *fallback;
        }
    }

    return std::unexpected(SemanticError{SemanticError::Kind::UndefinedName, span, "Undefined symbol: " + name});
}

/**
 * Special Form Handlers - Each handles a specific core or derived form
 */

SemResult<core::Node*> handle_if(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.size() != 4)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "if requires 3 args"});
    auto test = analyze(lst->elems[1], scope, ctx);
    if (!test) return test;
    auto conseq = analyze(lst->elems[2], scope, ctx);
    if (!conseq) return conseq;
    auto alt = analyze(lst->elems[3], scope, ctx);
    if (!alt) return alt;
    return ctx.mod.emplace<core::If>(lst->span, *test, *conseq, *alt);
}

SemResult<core::Node*> handle_begin(const List* lst, Scope& scope, AnalysisContext& ctx) {
    std::vector<core::Node*> exprs;
    for (size_t i = 1; i < lst->elems.size(); ++i) {
        auto res = analyze(lst->elems[i], scope, ctx);
        if (!res) return res;
        exprs.push_back(*res);
    }
    return ctx.mod.emplace<core::Begin>(lst->span, std::move(exprs));
}

SemResult<core::Node*> handle_set(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.size() != 3)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "set! requires 2 args"});
    const auto* target_sym = lst->elems[1]->as<Symbol>();
    if (!target_sym)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[1]->span(), "set! target must be a symbol"});

    auto lookup_res = lookup(target_sym->name, &scope, ctx, target_sym->span);
    if (!lookup_res) return std::unexpected(lookup_res.error());

    const auto& info = ctx.mod.bindings[lookup_res->id.id];
    if (!info.mutable_flag) {
        return std::unexpected(SemanticError{
            info.kind == BindingInfo::Kind::Import ? SemanticError::Kind::SetOnImported : SemanticError::Kind::ImmutableAssignment,
            target_sym->span,
            "Cannot assign to immutable " + (info.kind == BindingInfo::Kind::Import ? std::string("import") : std::string("variable")) + ": " + target_sym->name
        });
    }

    auto val = analyze(lst->elems[2], scope, ctx);
    if (!val) return val;

    return ctx.mod.emplace<core::Set>(lst->span, lookup_res->addr, *val);
}

SemResult<core::Node*> handle_lambda(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.size() < 3)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "lambda requires formals and body"});

    auto* lam_node = ctx.mod.emplace<core::Lambda>(lst->span);
    auto* lam = &std::get<core::Lambda>(lam_node->data);

    Scope lambda_scope{&scope};
    lambda_scope.is_lambda_boundary = true;
    lambda_scope.lambda_node = lam;

    if (auto res = parse_formals(lst->elems[1], lambda_scope, ctx, lam); !res) return std::unexpected(res.error());

    std::vector<core::Node*> body_exprs;
    for (size_t i = 2; i < lst->elems.size(); ++i) {
        auto res = analyze(lst->elems[i], lambda_scope, ctx);
        if (!res) return res;
        body_exprs.push_back(*res);
    }

    lam->body = wrap_body(std::move(body_exprs), lst->span, ctx.mod);
    lam->stack_size = lambda_scope.next_slot + 32; ///< Include some temporary space
    return lam_node;
}

SemResult<core::Node*> handle_quote(const List* lst, Scope&, AnalysisContext& ctx) {
    if (lst->elems.size() != 2)
         return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "quote requires 1 arg"});
    return ctx.mod.emplace<core::Quote>(lst->span, deep_copy(lst->elems[1]));
}

/**
 * Helper to analyze a fixed number of arguments from a form.
 * Returns analyzed nodes or error if arity mismatch or analysis fails.
 */
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
    return ctx.mod.emplace<core::DynamicWind>(lst->span, (*args)[0], (*args)[1], (*args)[2]);
}

SemResult<core::Node*> handle_values(const List* lst, Scope& scope, AnalysisContext& ctx) {
    std::vector<core::Node*> exprs;
    for (size_t i = 1; i < lst->elems.size(); ++i) {
        auto res = analyze(lst->elems[i], scope, ctx);
        if (!res) return res;
        exprs.push_back(*res);
    }
    return ctx.mod.emplace<core::Values>(lst->span, std::move(exprs));
}

SemResult<core::Node*> handle_call_with_values(const List* lst, Scope& scope, AnalysisContext& ctx) {
    auto args = analyze_n_args<2>(lst, scope, ctx, "call-with-values");
    if (!args) return std::unexpected(args.error());
    return ctx.mod.emplace<core::CallWithValues>(lst->span, (*args)[0], (*args)[1]);
}

SemResult<core::Node*> handle_call_cc(const List* lst, Scope& scope, AnalysisContext& ctx) {
    auto args = analyze_n_args<1>(lst, scope, ctx, "call/cc");
    if (!args) return std::unexpected(args.error());
    return ctx.mod.emplace<core::CallCC>(lst->span, (*args)[0]);
}

SemResult<core::Node*> handle_apply(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.size() < 3)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "apply requires at least 2 arguments"});
    auto proc = analyze(lst->elems[1], scope, ctx);
    if (!proc) return proc;
    std::vector<core::Node*> args;
    for (size_t i = 2; i < lst->elems.size(); ++i) {
        auto arg = analyze(lst->elems[i], scope, ctx);
        if (!arg) return arg;
        args.push_back(*arg);
    }
    return ctx.mod.emplace<core::Apply>(lst->span, *proc, std::move(args));
}

/// Exception handlers

/**
 * Helper: extract symbol name from a quoted symbol form '(quote tag).
 * Returns empty string if not a valid quoted symbol.
 */
static std::string extract_quoted_symbol(const SExprPtr& e) {
    if (!e) return {};
    if (const auto* lst = e->as<List>()) {
        if (lst->elems.size() == 2
            && lst->elems[0] && lst->elems[0]->is<Symbol>()
            && lst->elems[0]->as<Symbol>()->name == "quote"
            && lst->elems[1] && lst->elems[1]->is<Symbol>()) {
            return lst->elems[1]->as<Symbol>()->name;
        }
    }
    return {};
}

/// (raise 'tag value)  or  (raise value)
SemResult<core::Node*> handle_raise(const List* lst, Scope& scope, AnalysisContext& ctx) {
    /// lst->elems: [raise, ('tag | value), (value)?]
    if (lst->elems.size() < 2 || lst->elems.size() > 3)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape,
            lst->span, "raise requires 1 or 2 arguments"});

    std::string tag_name;
    const SExprPtr* value_expr = nullptr;

    if (lst->elems.size() == 3) {
        tag_name   = extract_quoted_symbol(lst->elems[1]);
        if (tag_name.empty())
            return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape,
                lst->elems[1]->span(), "raise: first arg must be a quoted symbol 'tag"});
        value_expr = &lst->elems[2];
    } else {
        value_expr = &lst->elems[1];
    }

    auto val = analyze(*value_expr, scope, ctx);
    if (!val) return val;
    return ctx.mod.emplace<core::Raise>(lst->span, tag_name, *val);
}

/// (catch 'tag body)  or  (catch body)
SemResult<core::Node*> handle_guard(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.size() < 2 || lst->elems.size() > 3)
        return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape,
            lst->span, "catch requires 1 or 2 arguments"});

    std::string tag_name;
    const SExprPtr* body_expr = nullptr;

    if (lst->elems.size() == 3) {
        tag_name  = extract_quoted_symbol(lst->elems[1]);
        if (tag_name.empty())
            return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape,
                lst->elems[1]->span(), "catch: first arg must be a quoted symbol 'tag"});
        body_expr = &lst->elems[2];
    } else {
        body_expr = &lst->elems[1];
    }

    auto body = analyze(*body_expr, scope, ctx);
    if (!body) return body;
    return ctx.mod.emplace<core::Guard>(lst->span, tag_name, *body);
}

/**
 * NOTE: Derived form handlers (let, letrec, case, do) have been REMOVED.
 * These forms MUST be desugared by the Expander before semantic analysis.
 * The Expander desugars:
 *   let      -> ((lambda (x...) body...) init...)
 *   let*     -> nested let
 *   letrec   -> (let ((x '()) ...) (set! x e) ... body...)
 *   letrec*  -> nested letrec
 *   cond     -> nested if
 *   case     -> (let ((tmp key)) (if (or (eqv? tmp d1) ...) body ...))
 *   do       -> (letrec ((loop (lambda (x...) (if test result (begin body (loop step...)))))) (loop init...))
 *   and/or   -> nested if
 *   when/unless -> (if test (begin body...) (begin))
 */

/// Handler registry - maps form names to handler functions
const std::unordered_map<std::string_view, SpecialFormHandler>& get_form_handlers() {
    static const std::unordered_map<std::string_view, SpecialFormHandler> handlers = {
        /// Core forms (required - these are the primitive forms that cannot be desugared)
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
        {"apply",                           handle_apply},
        /// Exception handling
        {"raise",                           handle_raise},
        {"catch",                           handle_guard},
        /// Logic variables / unification
        {"logic-var",    [](const List* lst, Scope&, AnalysisContext& ctx) -> SemResult<core::Node*> {
            return ctx.mod.emplace<core::MakeLogicVar>(lst->span);
        }},
        {"unify",        [](const List* lst, Scope& scope, AnalysisContext& ctx) -> SemResult<core::Node*> {
            if (lst->elems.size() != 3)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "unify requires 2 arguments"});
            auto a = analyze(lst->elems[1], scope, ctx); if (!a) return a;
            auto b = analyze(lst->elems[2], scope, ctx); if (!b) return b;
            return ctx.mod.emplace<core::Unify>(lst->span, *a, *b);
        }},
        {"deref-lvar",   [](const List* lst, Scope& scope, AnalysisContext& ctx) -> SemResult<core::Node*> {
            if (lst->elems.size() != 2)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "deref-lvar requires 1 argument"});
            auto x = analyze(lst->elems[1], scope, ctx); if (!x) return x;
            return ctx.mod.emplace<core::DerefLogicVar>(lst->span, *x);
        }},
        {"trail-mark",   [](const List* lst, Scope&, AnalysisContext& ctx) -> SemResult<core::Node*> {
            return ctx.mod.emplace<core::TrailMark>(lst->span);
        }},
        {"unwind-trail", [](const List* lst, Scope& scope, AnalysisContext& ctx) -> SemResult<core::Node*> {
            if (lst->elems.size() != 2)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "unwind-trail requires 1 argument"});
            auto m = analyze(lst->elems[1], scope, ctx); if (!m) return m;
            return ctx.mod.emplace<core::UnwindTrail>(lst->span, *m);
        }},
        {"copy-term",    [](const List* lst, Scope& scope, AnalysisContext& ctx) -> SemResult<core::Node*> {
            if (lst->elems.size() != 2)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "copy-term requires 1 argument"});
            auto t = analyze(lst->elems[1], scope, ctx); if (!t) return t;
            return ctx.mod.emplace<core::CopyTerm>(lst->span, *t);
        }},
        /**
         * NOTE: Derived forms (let, letrec, case, do) are no longer handled here.
         * They MUST be desugared by the Expander. If encountered, they will be
         * treated as function applications, which will produce a meaningful error.
         */
    };
    return handlers;
}

void collect_symbol_names_from_datum(
    const SExprPtr& datum,
    std::vector<std::string>& ordered_names,
    std::unordered_set<std::string>& seen) {
    if (!datum) return;

    if (const auto* sym = datum->as<Symbol>()) {
        if (!sym->name.empty() && seen.insert(sym->name).second) {
            ordered_names.push_back(sym->name);
        }
        return;
    }

    if (const auto* lst = datum->as<List>()) {
        for (const auto& elem : lst->elems) {
            collect_symbol_names_from_datum(elem, ordered_names, seen);
        }
        if (lst->dotted && lst->tail) {
            collect_symbol_names_from_datum(lst->tail, ordered_names, seen);
        }
        return;
    }

    if (const auto* reader_form = datum->as<ReaderForm>()) {
        if (reader_form->expr) {
            collect_symbol_names_from_datum(reader_form->expr, ordered_names, seen);
        }
    }
}

void capture_eval_quoted_lexicals(const List& eval_form, Scope& scope, AnalysisContext& ctx) {
    if (eval_form.elems.size() != 2 || !eval_form.elems[1]) return;

    const SExprPtr* quoted_datum = nullptr;
    if (const auto* arg_list = eval_form.elems[1]->as<List>()) {
        if (!arg_list->dotted &&
            arg_list->elems.size() == 2 &&
            arg_list->elems[0] &&
            arg_list->elems[0]->is<Symbol>() &&
            arg_list->elems[0]->as<Symbol>()->name == "quote") {
            quoted_datum = &arg_list->elems[1];
        }
    } else if (const auto* rf = eval_form.elems[1]->as<ReaderForm>()) {
        if (rf->kind == QuoteKind::Quote && rf->expr) {
            quoted_datum = &rf->expr;
        }
    }

    if (!quoted_datum || !*quoted_datum) return;

    std::vector<std::string> symbol_names;
    std::unordered_set<std::string> seen;
    collect_symbol_names_from_datum(*quoted_datum, symbol_names, seen);

    for (const auto& symbol_name : symbol_names) {
        auto lookup_res = lookup(symbol_name, &scope, ctx, eval_form.span);
        if (!lookup_res) continue;
    }
}

/**
 * Analyze a list form (application or special form).
 * Uses handler registry for extensibility and maintainability.
 */
SemResult<core::Node*> analyze_list(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.empty()) {
        return ctx.mod.emplace<core::Const>(lst->span, core::Literal{std::monostate{}});
    }

    /// Check for special forms
    if (const auto* head = lst->elems[0]->as<Symbol>()) {
        const auto& handlers = get_form_handlers();
        auto it = handlers.find(head->name);
        if (it != handlers.end()) {
            return it->second(lst, scope, ctx);
        }

        if (head->name == "eval") {
            capture_eval_quoted_lexicals(*lst, scope, ctx);
        }
    }

    /// Generic application
    auto callee = analyze(lst->elems[0], scope, ctx);
    if (!callee) return callee;
    std::vector<core::Node*> args;
    for (size_t i = 1; i < lst->elems.size(); ++i) {
        auto arg = analyze(lst->elems[i], scope, ctx);
        if (!arg) return arg;
        args.push_back(*arg);
    }
    return ctx.mod.emplace<core::Call>(lst->span, *callee, std::move(args));
}


SemResult<core::Node*> analyze(const SExprPtr& expr, Scope& scope, AnalysisContext& ctx) {
    if (auto* sym = expr->as<Symbol>()) {
        auto lookup_res = lookup(sym->name, &scope, ctx, sym->span);
        if (!lookup_res) return std::unexpected(lookup_res.error());
        return ctx.mod.emplace<core::Var>(sym->span, lookup_res->addr);
    }

    if (auto* n = make_const(expr, ctx)) return n;

    if (auto* lst = expr->as<List>()) return analyze_list(lst, scope, ctx);
    return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, expr->span(), "Unsupported expression type"});
}

/**
 * Generic IR Visitor - Centralizes traversal over all IR node types
 */

/**
 * The canonical IRVisitor CRTP base is defined in ir_visitor.h.
 * We reuse it here for the tail-position marker.
 */
} ///< close anonymous namespace

#include "eta/semantics/ir_visitor.h"

namespace {

/**
 * Marks nodes with their tail context position.
 * Uses IRVisitor to avoid duplicating traversal logic.
 */
struct TailMarker : eta::semantics::IRVisitor<TailMarker> {
    void pre_visit(core::Node* node, bool in_tail_context) {
        node->tail = in_tail_context;
    }
};

inline void mark_tail(core::Node* node, bool in_tail_context) {
    TailMarker marker;
    marker.visit(node, in_tail_context);
}

} ///< namespace

SemResult<std::vector<ModuleSemantics>>
SemanticAnalyzer::analyze_all(std::span<const SExprPtr> forms, const ::eta::reader::ModuleLinker& linker) {
    ::eta::runtime::BuiltinEnvironment empty;
    return analyze_all(forms, linker, empty);
}

SemResult<std::vector<ModuleSemantics>>
SemanticAnalyzer::analyze_all(std::span<const SExprPtr> forms, const ::eta::reader::ModuleLinker& linker,
                              const ::eta::runtime::BuiltinEnvironment& builtins) {
    std::vector<ModuleSemantics> out;

    /**
     * Unified global slot counter shared across all modules.
     * Builtins occupy slots 0..N-1; subsequent modules share the rest.
     */
    uint32_t next_global = static_cast<uint32_t>(builtins.size());


    /**
     * Map (module_name, export_name) -> unified global slot.
     * Built incrementally as modules are analyzed.
     */
    std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> export_slots;

    for (const auto& f : forms) {
        if (!f || !f->is<List>()) continue;
        const auto* lst = f->as<List>();
        if (lst->elems.size() < 2) continue;
        const auto* h = lst->elems[0] ? lst->elems[0]->as<Symbol>() : nullptr;
        if (!h || h->name != "module") continue;
        const auto* ns = lst->elems[1] ? lst->elems[1]->as<Symbol>() : nullptr;
        if (!ns) continue;
        auto mtref = linker.get(ns->name);
        if (!mtref || mtref->get().state != ::eta::reader::ModuleState::Linked)
            return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, f->span(), "module not linked"});

        ModuleSemantics mod; mod.name = ns->name;
        Scope toplevel{}; AnalysisContext ctx{mod, 0, &next_global};

        /**
         * Seed builtins as immutable globals at slots 0..N-1.
         * These use fixed slots (not allocated from the counter, which already starts past them).
         */
        Span builtin_span{}; ///< synthetic zero span for builtins
        {
            /**
             * Temporarily point the counter to a local that tracks builtin slots,
             * so builtins always get slots 0..N-1 regardless of module order.
             */
            uint32_t builtin_slot = 0;
            ctx.shared_next_global = &builtin_slot;
            for (const auto& spec : builtins.specs()) {
                ctx.add_binding(toplevel, spec.name, BindingInfo::Kind::Global, builtin_span, /*mutable_flag=*/false);
            }
            ctx.shared_next_global = &next_global; ///< restore shared counter
        }


        /// Wire imports to the exporting module's unified slot
        for (const auto& [ln, orign] : mtref->get().import_origins) {
            auto mod_it = export_slots.find(orign.from_module);
            if (mod_it != export_slots.end()) {
                auto slot_it = mod_it->second.find(orign.remote_name);
                if (slot_it != mod_it->second.end()) {
                    ctx.add_import_at_slot(toplevel, ln, orign, orign.where, slot_it->second);
                    continue;
                }
            }
            /// Fallback: allocate a fresh slot (should not happen if modules are in dependency order)
            ctx.add_import(toplevel, ln, orign, orign.where);
        }

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
                                mod.toplevel_inits.push_back(ctx.mod.emplace<core::Set>(bf->span(), ar->addr, *val));
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
        mod.stack_size = toplevel.next_slot + 32;
        for (const auto& ex : mtref->get().exports) {
            auto it = toplevel.table.find(ex);
            if (it == toplevel.table.end()) return std::unexpected(SemanticError{SemanticError::Kind::ExportOfUnknownBinding, f->span(), "unknown export"});
            mod.exports.emplace(ex, it->second);
        }

        /// Record this module's export slots for downstream modules
        for (const auto& [export_name, binding_id] : mod.exports) {
            export_slots[ns->name][export_name] = mod.bindings[binding_id.id].slot;
        }

        /**
         * Detect optional (defun main ...) entry point
         * Look for a top-level global binding named "main"
         */
        if (auto it = toplevel.table.find("main"); it != toplevel.table.end()) {
            const auto& bi = mod.bindings[it->second.id];
            if (bi.kind == BindingInfo::Kind::Global) {
                mod.main_func_slot = bi.slot;
            }
        }

        out.push_back(std::move(mod));
    }

    /// Stamp every module with the unified global count so callers can size globals once
    for (auto& m : out) {
        m.total_globals = next_global;
    }

    return out;
}


} ///< namespace eta::semantics
