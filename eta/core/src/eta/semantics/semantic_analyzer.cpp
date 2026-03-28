#include "eta/semantics/semantic_analyzer.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "eta/reader/parser.h"

namespace eta::semantics {

using namespace eta::reader::parser;

namespace {

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

SemResult<core::Node*> analyze_list(const List* lst, Scope& scope, AnalysisContext& ctx) {
    if (lst->elems.empty()) {
        core::Node* n = ctx.mod.emplace<core::Const>();
        auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(n));
        c->value.payload = std::monostate{};
        c->span = lst->span;
        return n;
    }

    const auto* head = lst->elems[0]->as<Symbol>();
    if (head) {
        if (head->name == "if") {
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

        if (head->name == "begin") {
            std::vector<core::Node*> exprs;
            for (size_t i = 1; i < lst->elems.size(); ++i) {
                auto res = analyze(lst->elems[i], scope, ctx);
                if (!res) return res;
                exprs.push_back(*res);
            }
            return ctx.mod.emplace<core::Begin>(std::move(exprs), false, lst->span);
        }

        if (head->name == "set!") {
            if (lst->elems.size() != 3)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "set! requires 2 args"});
            const auto* target_sym = lst->elems[1]->as<Symbol>();
            if (!target_sym)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[1]->span(), "set! target must be a symbol"});
            
            auto addr = lookup(target_sym->name, &scope, ctx, target_sym->span);
            if (!addr) return std::unexpected(addr.error());

            auto is_mutable = [&](const core::Address& a) -> bool {
                if (auto* l = std::get_if<core::Address::Local>(&a.where)) {
                    if (l->slot < ctx.mod.bindings.size()) return ctx.mod.bindings[l->slot].mutable_flag;
                } else if (auto* g = std::get_if<core::Address::Global>(&a.where)) {
                    if (g->id < ctx.mod.bindings.size()) return ctx.mod.bindings[g->id].mutable_flag;
                } else if (auto* u = std::get_if<core::Address::Upval>(&a.where)) {
                    if (scope.lambda_node && u->slot < scope.lambda_node->upvals.size()) {
                        auto bid = scope.lambda_node->upvals[u->slot];
                        if (bid.id < ctx.mod.bindings.size()) return ctx.mod.bindings[bid.id].mutable_flag;
                    }
                }
                return true; 
            };

            if (!is_mutable(*addr))
                return std::unexpected(SemanticError{SemanticError::Kind::ImmutableAssignment, target_sym->span, "Cannot set! immutable binding"});
            
            auto val = analyze(lst->elems[2], scope, ctx);
            if (!val) return val;
            return ctx.mod.emplace<core::Set>(*addr, *val, lst->span);
        }

        if (head->name == "lambda") {
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
            
            if (body_exprs.size() == 1) lam->body = body_exprs[0];
            else if (body_exprs.empty()) lam->body = ctx.mod.emplace<core::Const>(core::Literal{std::monostate{}}, lst->span);
            else lam->body = ctx.mod.emplace<core::Begin>(std::move(body_exprs), false, lst->span);

            return lam_node;
        }
        
        if (head->name == "quote") {
            if (lst->elems.size() != 2)
                 return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "quote requires 1 arg"});
            return ctx.mod.emplace<core::Quote>(std::shared_ptr<SExpr>(lst->elems[1].get(), [](SExpr*){}), lst->span);
        }

        if (head->name == "let") {
            if (lst->elems.size() < 3)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "let requires bindings and body"});
            const auto* b_lst = lst->elems[1]->as<List>();
            if (!b_lst)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[1]->span(), "let bindings must be a list"});
            
            std::vector<core::Let::Bind> binds;
            Scope let_scope{&scope};
            for (const auto& b : b_lst->elems) {
                const auto* item = b->as<List>();
                if (!item || item->elems.size() != 2)
                    return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, b->span(), "let binding must be (sym val)"});
                const auto* s = item->elems[0]->as<Symbol>();
                if (!s)
                    return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, item->elems[0]->span(), "let binding name must be symbol"});
                
                auto val = analyze(item->elems[1], scope, ctx);
                if (!val) return val;
                auto id = ctx.add_binding(let_scope, s->name, BindingInfo::Kind::Local, s->span);
                binds.push_back({id, *val});
            }
            
            std::vector<core::Node*> body_exprs;
            for (size_t i = 2; i < lst->elems.size(); ++i) {
                auto res = analyze(lst->elems[i], let_scope, ctx);
                if (!res) return res;
                body_exprs.push_back(*res);
            }
            
            core::Node* body = nullptr;
            if (body_exprs.size() == 1) body = body_exprs[0];
            else if (body_exprs.empty()) body = ctx.mod.emplace<core::Const>(core::Literal{std::monostate{}}, lst->span);
            else body = ctx.mod.emplace<core::Begin>(std::move(body_exprs), false, lst->span);
            
            return ctx.mod.emplace<core::Let>(std::move(binds), body, false, lst->span);
        }

        if (head->name == "letrec") {
            if (lst->elems.size() < 3)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "letrec requires bindings and body"});
            const auto* b_lst = lst->elems[1]->as<List>();
            if (!b_lst)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[1]->span(), "letrec bindings must be a list"});

            Scope letrec_scope{&scope};
            std::vector<core::BindingId> binding_ids;
            std::vector<const List*> items;
            
            for (const auto& b : b_lst->elems) {
                const auto* item = b->as<List>();
                if (!item || item->elems.size() != 2)
                    return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, b->span(), "letrec binding must be (sym val)"});
                const auto* s = item->elems[0]->as<Symbol>();
                if (!s)
                    return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, item->elems[0]->span(), "letrec binding name must be symbol"});
                
                auto id = ctx.add_binding(letrec_scope, s->name, BindingInfo::Kind::Local, s->span, true);
                binding_ids.push_back(id);
                items.push_back(item);
            }
            
            std::vector<core::Node*> inits;
            for (size_t i = 0; i < items.size(); ++i) {
                auto val = analyze(items[i]->elems[1], letrec_scope, ctx);
                if (!val) return val;
                auto* sym = items[i]->elems[0]->as<Symbol>();
                auto addr_res = lookup(sym->name, &letrec_scope, ctx, sym->span);
                if (!addr_res) return std::unexpected(addr_res.error());
                inits.push_back(ctx.mod.emplace<core::Set>(*addr_res, *val, items[i]->span));
            }
            
            std::vector<core::Node*> body_exprs;
            for (size_t i = 2; i < lst->elems.size(); ++i) {
                auto res = analyze(lst->elems[i], letrec_scope, ctx);
                if (!res) return res;
                body_exprs.push_back(*res);
            }
            
            core::Node* body_node = nullptr;
            if (body_exprs.empty()) body_node = ctx.mod.emplace<core::Const>(core::Literal{std::monostate{}}, lst->span);
            else if (body_exprs.size() == 1) body_node = body_exprs[0];
            else body_node = ctx.mod.emplace<core::Begin>(std::move(body_exprs), false, lst->span);

            if (!inits.empty()) {
                std::vector<core::Node*> full = std::move(inits);
                full.push_back(body_node);
                body_node = ctx.mod.emplace<core::Begin>(std::move(full), false, lst->span);
            }
            return ctx.mod.emplace<core::LetRec>(std::move(binding_ids), body_node, false, lst->span);
        }

        if (head->name == "dynamic-wind") {
            if (lst->elems.size() != 4)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "dynamic-wind requires 3 args"});
            auto b = analyze(lst->elems[1], scope, ctx); if (!b) return b;
            auto m = analyze(lst->elems[2], scope, ctx); if (!m) return m;
            auto a = analyze(lst->elems[3], scope, ctx); if (!a) return a;
            return ctx.mod.emplace<core::DynamicWind>(*b, *m, *a, lst->span);
        }

        if (head->name == "case") {
            if (lst->elems.size() < 2)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "case requires a key"});
            auto key_res = analyze(lst->elems[1], scope, ctx);
            if (!key_res) return key_res;

            std::vector<core::Case::Clause> clauses;
            for (size_t i = 2; i < lst->elems.size(); ++i) {
                const auto* clause_lst = lst->elems[i]->as<List>();
                if (!clause_lst || clause_lst->elems.empty())
                    return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[i]->span(), "case clause must be a list"});
                
                core::Case::Clause c;
                c.is_else = false;
                if (const auto* s = clause_lst->elems[0]->as<Symbol>()) {
                    if (s->name == "else") {
                        if (i != lst->elems.size() - 1)
                            return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, s->span, "else must be the last clause in case"});
                        c.is_else = true;
                    }
                }

                if (!c.is_else) {
                    const auto* datums_lst = clause_lst->elems[0]->as<List>();
                    if (!datums_lst)
                         return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, clause_lst->elems[0]->span(), "case clause datums must be a list or else"});
                    for (const auto& d : datums_lst->elems) {
                         c.datums.push_back(std::shared_ptr<SExpr>(d.get(), [](SExpr*){}));
                    }
                }

                std::vector<core::Node*> body_exprs;
                for (size_t j = 1; j < clause_lst->elems.size(); ++j) {
                    auto res = analyze(clause_lst->elems[j], scope, ctx);
                    if (!res) return res;
                    body_exprs.push_back(*res);
                }
                if (body_exprs.empty()) c.body = ctx.mod.emplace<core::Const>(core::Literal{std::monostate{}}, clause_lst->span);
                else if (body_exprs.size() == 1) c.body = body_exprs[0];
                else c.body = ctx.mod.emplace<core::Begin>(std::move(body_exprs), false, clause_lst->span);
                
                clauses.push_back(std::move(c));
            }
            return ctx.mod.emplace<core::Case>(*key_res, std::move(clauses), lst->span);
        }

        if (head->name == "values") {
            std::vector<core::Node*> exprs;
            for (size_t i = 1; i < lst->elems.size(); ++i) {
                auto res = analyze(lst->elems[i], scope, ctx);
                if (!res) return res;
                exprs.push_back(*res);
            }
            return ctx.mod.emplace<core::Values>(std::move(exprs), lst->span);
        }

        if (head->name == "call-with-values") {
            if (lst->elems.size() != 3)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "call-with-values requires 2 args"});
            auto prod = analyze(lst->elems[1], scope, ctx); if (!prod) return prod;
            auto cons = analyze(lst->elems[2], scope, ctx); if (!cons) return cons;
            return ctx.mod.emplace<core::CallWithValues>(*prod, *cons, false, lst->span);
        }

        if (head->name == "call/cc" || head->name == "call-with-current-continuation") {
            if (lst->elems.size() != 2)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "call/cc requires 1 arg"});
            auto cons = analyze(lst->elems[1], scope, ctx); if (!cons) return cons;
            return ctx.mod.emplace<core::CallCC>(*cons, false, lst->span);
        }

        if (head->name == "do") {
            if (lst->elems.size() < 3)
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->span, "do requires at least 2 args (test and body)"});
            
            // (do ((<var> <init> <step>) ...) (<test> <expr> ...) <body> ...)
            const auto* binds_lst = lst->elems[1]->as<List>();
            if (!binds_lst) return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[1]->span(), "do bindings must be a list"});
            
            const auto* test_clause = lst->elems[2]->as<List>();
            if (!test_clause || test_clause->elems.empty())
                return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, lst->elems[2]->span(), "do test clause must be a non-empty list"});

            // Desugar do to letrec + named lambda or similar.
            // Simplified here: we'll use a LetRec with a loop function.
            // (letrec ((loop (lambda (vars...) 
            //                  (if test (begin exprs...)
            //                      (begin body... (loop steps...))))))
            //   (loop inits...))
            
            // 1. Analyze inits in outer scope
            std::vector<core::Node*> inits;
            std::vector<std::string> var_names;
            for (const auto& b : binds_lst->elems) {
                const auto* b_lst = b->as<List>();
                if (!b_lst || b_lst->elems.empty()) return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, b->span(), "do binding must be a list"});
                const auto* sym = b_lst->elems[0]->as<Symbol>();
                if (!sym) return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, b_lst->elems[0]->span(), "do binding var must be a symbol"});
                var_names.push_back(sym->name);
                
                auto init_res = analyze(b_lst->elems[1], scope, ctx);
                if (!init_res) return init_res;
                inits.push_back(*init_res);
            }

            // 2. Create inner scope for the loop
            Scope do_scope;
            do_scope.parent = &scope;
            std::vector<core::BindingId> binding_ids;
            for (const auto& name : var_names) {
                auto bid = ctx.add_binding(do_scope, name, BindingInfo::Kind::Local, lst->span, true);
                binding_ids.push_back(bid);
            }

            // 3. Analyze test, result exprs, and body in do_scope
            auto test_res = analyze(test_clause->elems[0], do_scope, ctx);
            if (!test_res) return test_res;

            std::vector<core::Node*> result_exprs;
            for (size_t i = 1; i < test_clause->elems.size(); ++i) {
                auto res = analyze(test_clause->elems[i], do_scope, ctx);
                if (!res) return res;
                result_exprs.push_back(*res);
            }
            core::Node* result_node = nullptr;
            if (result_exprs.empty()) result_node = ctx.mod.emplace<core::Const>(core::Literal{std::monostate{}}, test_clause->span);
            else if (result_exprs.size() == 1) result_node = result_exprs[0];
            else result_node = ctx.mod.emplace<core::Begin>(std::move(result_exprs), false, test_clause->span);

            std::vector<core::Node*> body_exprs;
            for (size_t i = 3; i < lst->elems.size(); ++i) {
                auto res = analyze(lst->elems[i], do_scope, ctx);
                if (!res) return res;
                body_exprs.push_back(*res);
            }
            
            // 4. Analyze steps
            std::vector<core::Node*> steps;
            for (size_t i = 0; i < binds_lst->elems.size(); ++i) {
                const auto* b_lst = binds_lst->elems[i]->as<List>();
                if (b_lst->elems.size() > 2) {
                    auto step_res = analyze(b_lst->elems[2], do_scope, ctx);
                    if (!step_res) return step_res;
                    steps.push_back(*step_res);
                } else {
                    // No step means variable stays the same
                    auto addr = lookup(var_names[i], &do_scope, ctx, b_lst->span);
                    steps.push_back(ctx.mod.emplace<core::Var>(*addr, b_lst->span));
                }
            }

            // Now assemble the desugared form.
            // We'll use a core::Lambda for the loop and a core::LetRec for the recursion.
            auto loop_id = ctx.add_binding(scope, "%do-loop", BindingInfo::Kind::Local, lst->span, false);
            
            core::Node* loop_lambda_node = ctx.mod.emplace<core::Lambda>();
            core::Lambda* loop_lambda = std::get_if<core::Lambda>(static_cast<core::NodeBase*>(loop_lambda_node));
            loop_lambda->params = binding_ids;
            loop_lambda->span = lst->span;
            loop_lambda->arity = { (uint16_t)binding_ids.size(), 0, false };
            
            // Inside loop_lambda: (if test result (begin body... (loop steps...)))
            auto loop_var_addr = core::Address{ core::Address::Local{ (uint16_t)loop_id.id } };
            auto loop_var = ctx.mod.emplace<core::Var>(loop_var_addr, lst->span);
            auto loop_call = ctx.mod.emplace<core::Call>(loop_var, std::move(steps), true, lst->span);
            
            core::Node* alternative = nullptr;
            if (body_exprs.empty()) {
                alternative = loop_call;
            } else {
                body_exprs.push_back(loop_call);
                alternative = ctx.mod.emplace<core::Begin>(std::move(body_exprs), true, lst->span);
            }
            
            loop_lambda->body = ctx.mod.emplace<core::If>(*test_res, result_node, alternative, true, lst->span);
            
            core::Node* letrec_node = ctx.mod.emplace<core::LetRec>();
            core::LetRec* letrec = std::get_if<core::LetRec>(static_cast<core::NodeBase*>(letrec_node));
            letrec->binds = { loop_id };
            letrec->span = lst->span;
            
            // The body of letrec: (begin (set! loop lambda) (loop inits...))
            auto set_loop = ctx.mod.emplace<core::Set>(loop_var_addr, loop_lambda_node, lst->span);
            auto initial_call = ctx.mod.emplace<core::Call>(loop_var, std::move(inits), false, lst->span);
            letrec->body = ctx.mod.emplace<core::Begin>(std::vector<core::Node*>{set_loop, initial_call}, false, lst->span);
            
            return letrec_node;
        }
    }

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
        c->value.payload = core::LiteralNumber{num->value};
        c->span = num->span;
        return n;
    }

    if (auto* lst = expr->as<List>()) return analyze_list(lst, scope, ctx);
    return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, expr->span(), "Unsupported expression type"});
}

inline void mark_tail(core::Node* node, bool in_tail_context) {
    std::visit([&](auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, core::If>) {
            n.tail = in_tail_context;
            mark_tail(n.test, false); mark_tail(n.conseq, in_tail_context); mark_tail(n.alt, in_tail_context);
        } else if constexpr (std::is_same_v<T, core::Begin>) {
            n.tail = in_tail_context;
            for (std::size_t i = 0; i < n.exprs.size(); ++i)
                mark_tail(n.exprs[i], in_tail_context && (i + 1 == n.exprs.size()));
        } else if constexpr (std::is_same_v<T, core::Let>) {
            n.tail = in_tail_context;
            for (auto& b : n.binds) mark_tail(b.init, false);
            mark_tail(n.body, in_tail_context);
        } else if constexpr (std::is_same_v<T, core::LetRec>) {
            n.tail = in_tail_context; mark_tail(n.body, in_tail_context);
        } else if constexpr (std::is_same_v<T, core::Lambda>) {
            mark_tail(n.body, true);
        } else if constexpr (std::is_same_v<T, core::Call>) {
            n.tail = in_tail_context; mark_tail(n.callee, false);
            for (auto* a : n.args) mark_tail(a, false);
        } else if constexpr (std::is_same_v<T, core::Set>) {
            mark_tail(n.value, false);
        } else if constexpr (std::is_same_v<T, core::DynamicWind>) {
            mark_tail(n.before, false); mark_tail(n.body, false); mark_tail(n.after, false);
        } else if constexpr (std::is_same_v<T, core::Case>) {
            mark_tail(n.key, false);
            for (auto& c : n.clauses) mark_tail(c.body, in_tail_context);
        } else if constexpr (std::is_same_v<T, core::Values>) {
            for (auto* e : n.exprs) mark_tail(e, false);
        } else if constexpr (std::is_same_v<T, core::CallWithValues>) {
            n.tail = in_tail_context;
            mark_tail(n.producer, false); mark_tail(n.consumer, false);
        } else if constexpr (std::is_same_v<T, core::Const>) {
            // no tail children
        } else if constexpr (std::is_same_v<T, core::Var>) {
            // no tail children
        } else if constexpr (std::is_same_v<T, core::Quote>) {
            // no tail children
        }
    }, static_cast<core::NodeBase&>(*node));
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
    // Basic constant folding implementation for core::If
    for (auto*& node : mod.toplevel_inits) {
        auto fold = [&](auto& self, core::Node* n) -> core::Node* {
            if (!n) return nullptr;
            std::visit([&](auto& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, core::If>) {
                    val.test = self(self, val.test);
                    val.conseq = self(self, val.conseq);
                    val.alt = self(self, val.alt);
                    if (auto* c = std::get_if<core::Const>(static_cast<core::NodeBase*>(val.test))) {
                        if (auto* b = std::get_if<bool>(&c->value.payload)) {
                            n = *b ? val.conseq : val.alt;
                        } else if (!std::holds_alternative<std::monostate>(c->value.payload)) {
                            // Any non-false constant is true in Scheme (except maybe empty list, but let's be simple)
                            n = val.conseq;
                        }
                    }
                } else if constexpr (std::is_same_v<T, core::Begin>) {
                    for (auto*& e : val.exprs) e = self(self, e);
                } else if constexpr (std::is_same_v<T, core::Let>) {
                    for (auto& b : val.binds) b.init = self(self, b.init);
                    val.body = self(self, val.body);
                } else if constexpr (std::is_same_v<T, core::LetRec>) {
                    val.body = self(self, val.body);
                } else if constexpr (std::is_same_v<T, core::Lambda>) {
                    val.body = self(self, val.body);
                } else if constexpr (std::is_same_v<T, core::Call>) {
                    val.callee = self(self, val.callee);
                    for (auto*& a : val.args) a = self(self, a);
                } else if constexpr (std::is_same_v<T, core::Set>) {
                    val.value = self(self, val.value);
                } else if constexpr (std::is_same_v<T, core::DynamicWind>) {
                    val.before = self(self, val.before);
                    val.body = self(self, val.body);
                    val.after = self(self, val.after);
                } else if constexpr (std::is_same_v<T, core::Case>) {
                    val.key = self(self, val.key);
                    for (auto& c : val.clauses) c.body = self(self, c.body);
                } else if constexpr (std::is_same_v<T, core::Values>) {
                    for (auto*& e : val.exprs) e = self(self, e);
                } else if constexpr (std::is_same_v<T, core::CallWithValues>) {
                    val.producer = self(self, val.producer);
                    val.consumer = self(self, val.consumer);
                } else if constexpr (std::is_same_v<T, core::CallCC>) {
                    val.consumer = self(self, val.consumer);
                }
            }, static_cast<core::NodeBase&>(*n));
            return n;
        };
        node = fold(fold, node);
    }
}

} // namespace eta::semantics
