#include "emitter.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/types.h"

#include <cassert>

namespace eta::semantics {

using namespace runtime::vm;
using namespace runtime::memory::factory;
using namespace runtime::nanbox;
using namespace runtime::types;

// ============================================================================
// Top-level emit
// ============================================================================

BytecodeFunction* Emitter::emit() {
    Context ctx;
    ctx.func.name      = sem_.name + "_init";
    ctx.func.stack_size = sem_.stack_size;
    ctx.func.arity     = 0;
    ctx.func.has_rest  = false;

    Span empty{};
    for (const auto* node : sem_.toplevel_inits) {
        emit_node(node, ctx);
        // Pop the value every top-level form leaves on the stack.
        ctx.emit_instr(OpCode::Pop, 0, node->span);
    }

    // Module init returns Nil (single LoadConst + Return).
    // Fixed: the original code had a double-push of LoadConst here.
    emit_load_const(Nil, ctx, empty);
    ctx.emit_instr(OpCode::Return, 0, empty);

    uint32_t idx = registry_.add(std::move(ctx.func));
    return registry_.get_mut(idx);
}

// ============================================================================
// Node dispatch
// ============================================================================

void Emitter::emit_node(const core::Node* node, Context& ctx) {
    const Span& span = node->span;
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, core::Const>)
            emit_const(n, ctx, span);
        else if constexpr (std::is_same_v<T, core::Var>)
            emit_var(n, ctx, span);
        else if constexpr (std::is_same_v<T, core::Call>)
            emit_call(n, node->tail, ctx, span);
        else if constexpr (std::is_same_v<T, core::If>)
            emit_if(n, ctx, span);
        else if constexpr (std::is_same_v<T, core::Begin>)
            emit_begin(n, ctx, span);
        else if constexpr (std::is_same_v<T, core::Lambda>)
            emit_lambda_node(n, span, ctx);
        else if constexpr (std::is_same_v<T, core::Set>)
            emit_set(n, ctx, span);
        else if constexpr (std::is_same_v<T, core::Values>)
            emit_values(n, ctx, span);
        else if constexpr (std::is_same_v<T, core::CallWithValues>)
            emit_call_with_values(n, node->tail, ctx, span);
        else if constexpr (std::is_same_v<T, core::DynamicWind>)
            emit_dynamic_wind(n, ctx, span);
        else if constexpr (std::is_same_v<T, core::CallCC>)
            emit_call_cc(n, node->tail, ctx, span);
        else if constexpr (std::is_same_v<T, core::Apply>)
            emit_apply(n, node->tail, ctx, span);
        else if constexpr (std::is_same_v<T, core::Quote>)
            emit_quote(n, ctx, span);
    }, node->data);
}

// ============================================================================
// Helpers
// ============================================================================

uint32_t Emitter::add_const(LispVal val, Context& ctx) {
    uint32_t idx = static_cast<uint32_t>(ctx.func.constants.size());
    ctx.func.constants.push_back(val);
    return idx;
}

uint32_t Emitter::emit_load_const(LispVal val, Context& ctx, const Span& span) {
    uint32_t idx = add_const(val, ctx);
    ctx.emit_instr(OpCode::LoadConst, idx, span);
    return idx;
}

void Emitter::emit_address_load(const core::Address& addr, Context& ctx, const Span& span) {
    std::visit([&](auto&& a) {
        using AT = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<AT, core::Address::Local>)
            ctx.emit_instr(OpCode::LoadLocal, a.slot, span);
        else if constexpr (std::is_same_v<AT, core::Address::Upval>)
            ctx.emit_instr(OpCode::LoadUpval, a.slot, span);
        else if constexpr (std::is_same_v<AT, core::Address::Global>)
            ctx.emit_instr(OpCode::LoadGlobal, a.id, span);
    }, addr.where);
}

void Emitter::emit_address_store(const core::Address& addr, Context& ctx, const Span& span) {
    std::visit([&](auto&& a) {
        using AT = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<AT, core::Address::Local>)
            ctx.emit_instr(OpCode::StoreLocal, a.slot, span);
        else if constexpr (std::is_same_v<AT, core::Address::Upval>)
            ctx.emit_instr(OpCode::StoreUpval, a.slot, span);
        else if constexpr (std::is_same_v<AT, core::Address::Global>)
            ctx.emit_instr(OpCode::StoreGlobal, a.id, span);
    }, addr.where);
}

// ============================================================================
// Leaf emitters
// ============================================================================

void Emitter::emit_const(const core::Const& n, Context& ctx, const Span& span) {
    if (const auto* s = std::get_if<std::string>(&n.value.payload)) {
        if (auto it = ctx.string_constant_cache.find(*s);
            it != ctx.string_constant_cache.end()) {
            ctx.emit_instr(OpCode::LoadConst, it->second, span);
            return;
        }
    }

    LispVal val = std::visit([&](auto&& p) -> LispVal {
        using PT = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<PT, std::monostate>)    return Nil;
        else if constexpr (std::is_same_v<PT, bool>)         return p ? True : False;
        else if constexpr (std::is_same_v<PT, char32_t>)     return ops::encode(p).value();
        else if constexpr (std::is_same_v<PT, std::string>)  return make_string(heap_, intern_table_, p).value();
        else if constexpr (std::is_same_v<PT, int64_t>)      return make_fixnum(heap_, p).value();
        else if constexpr (std::is_same_v<PT, double>)       return make_flonum(p).value();
        return Nil;
    }, n.value.payload);

    uint32_t idx = emit_load_const(val, ctx, span);
    if (const auto* s = std::get_if<std::string>(&n.value.payload))
        ctx.string_constant_cache[*s] = idx;
}

void Emitter::emit_var(const core::Var& n, Context& ctx, const Span& span) {
    emit_address_load(n.addr, ctx, span);
}

void Emitter::emit_call(const core::Call& n, bool tail, Context& ctx, const Span& span) {
    for (const auto* arg : n.args)
        emit_node(arg, ctx);
    emit_node(n.callee, ctx);
    ctx.emit_instr(tail ? OpCode::TailCall : OpCode::Call,
                   static_cast<uint32_t>(n.args.size()), span);
}

void Emitter::emit_if(const core::If& n, Context& ctx, const Span& span) {
    emit_node(n.test, ctx);
    uint32_t jump_if_false_idx = static_cast<uint32_t>(ctx.func.code.size());
    ctx.emit_instr(OpCode::JumpIfFalse, 0, span);  // placeholder

    emit_node(n.conseq, ctx);
    uint32_t jump_idx = static_cast<uint32_t>(ctx.func.code.size());
    ctx.emit_instr(OpCode::Jump, 0, span);          // placeholder

    ctx.func.code[jump_if_false_idx].arg =
        static_cast<uint32_t>(ctx.func.code.size() - jump_if_false_idx - 1);
    emit_node(n.alt, ctx);
    ctx.func.code[jump_idx].arg =
        static_cast<uint32_t>(ctx.func.code.size() - jump_idx - 1);
}

void Emitter::emit_begin(const core::Begin& n, Context& ctx, const Span& span) {
    // Detect letrec-expanded pattern: a leading run of Set{Local} nodes whose
    // values are Lambdas.  After all closures are created, earlier closures may
    // have captured placeholder ('()) values for later-assigned locals.
    struct LetrecInit { uint32_t slot; const core::Lambda* lam; };
    std::vector<LetrecInit> letrec_inits;
    for (const auto* expr : n.exprs) {
        const auto* set = std::get_if<core::Set>(&expr->data);
        if (!set) break;
        const auto* tgt = std::get_if<core::Address::Local>(&set->target.where);
        if (!tgt) break;
        const auto* lam = std::get_if<core::Lambda>(&set->value->data);
        if (!lam) break;
        letrec_inits.push_back({tgt->slot, lam});
    }
    const size_t ninits = letrec_inits.size();

    for (size_t i = 0; i < n.exprs.size(); ++i) {
        emit_node(n.exprs[i], ctx);
        if (i < n.exprs.size() - 1)
            ctx.emit_instr(OpCode::Pop, 0, n.exprs[i]->span);

        // After the last letrec initializer, patch cross-references.
        if (ninits > 1 && i + 1 == ninits) {
            for (size_t later = 1; later < ninits; ++later) {
                uint32_t later_slot = letrec_inits[later].slot;
                for (size_t earlier = 0; earlier < later; ++earlier) {
                    const auto& ei = letrec_inits[earlier];
                    for (size_t uv = 0; uv < ei.lam->upval_sources.size(); ++uv) {
                        const auto* src = std::get_if<core::Address::Local>(
                            &ei.lam->upval_sources[uv].where);
                        if (src && src->slot == later_slot) {
                            ctx.emit_instr(OpCode::LoadLocal, ei.slot, span);
                            ctx.emit_instr(OpCode::LoadLocal, later_slot, span);
                            ctx.emit_instr(OpCode::PatchClosureUpval,
                                           static_cast<uint32_t>(uv), span);
                        }
                    }
                }
            }
        }
    }
}

void Emitter::emit_lambda_node(const core::Lambda& n, const Span& span, Context& ctx) {
    uint32_t func_idx = emit_lambda(n, ctx.func.name, span);

    LispVal func_idx_val = encode_func_index(func_idx);
    uint32_t const_idx   = add_const(func_idx_val, ctx);
    uint32_t num_upvals  = static_cast<uint32_t>(n.upval_sources.size());

    for (const auto& src : n.upval_sources)
        emit_address_load(src, ctx, span);

    ctx.emit_instr(OpCode::MakeClosure, (const_idx << 16) | num_upvals, span);
}

void Emitter::emit_set(const core::Set& n, Context& ctx, const Span& span) {
    emit_node(n.value, ctx);
    emit_address_store(n.target, ctx, span);

    // Fixup for letrec self-reference
    if (const auto* lam = std::get_if<core::Lambda>(&n.value->data)) {
        if (const auto* target_local = std::get_if<core::Address::Local>(&n.target.where)) {
            for (size_t i = 0; i < lam->upval_sources.size(); ++i) {
                if (const auto* src_local =
                        std::get_if<core::Address::Local>(&lam->upval_sources[i].where)) {
                    if (src_local->slot == target_local->slot) {
                        ctx.emit_instr(OpCode::LoadLocal, target_local->slot, span);
                        ctx.emit_instr(OpCode::LoadLocal, target_local->slot, span);
                        ctx.emit_instr(OpCode::PatchClosureUpval,
                                       static_cast<uint32_t>(i), span);
                    }
                }
            }
        }
    }

    // set! returns unspecified value (we push nil)
    emit_load_const(Nil, ctx, span);
}

void Emitter::emit_values(const core::Values& n, Context& ctx, const Span& span) {
    for (const auto* expr : n.exprs)
        emit_node(expr, ctx);
    ctx.emit_instr(OpCode::Values, static_cast<uint32_t>(n.exprs.size()), span);
}

void Emitter::emit_call_with_values(const core::CallWithValues& n, bool /*tail*/,
                                    Context& ctx, const Span& span) {
    emit_node(n.producer, ctx);
    emit_node(n.consumer, ctx);
    ctx.emit_instr(OpCode::CallWithValues, 0, span);
}

void Emitter::emit_dynamic_wind(const core::DynamicWind& n, Context& ctx, const Span& span) {
    emit_node(n.before, ctx);
    emit_node(n.body,   ctx);
    emit_node(n.after,  ctx);
    ctx.emit_instr(OpCode::DynamicWind, 0, span);
}

void Emitter::emit_call_cc(const core::CallCC& n, bool /*tail*/,
                           Context& ctx, const Span& span) {
    emit_node(n.consumer, ctx);
    ctx.emit_instr(OpCode::CallCC, 0, span);
}

void Emitter::emit_apply(const core::Apply& n, bool tail, Context& ctx, const Span& span) {
    for (const auto* arg : n.args)
        emit_node(arg, ctx);
    emit_node(n.proc, ctx);
    ctx.emit_instr(tail ? OpCode::TailApply : OpCode::Apply,
                   static_cast<uint32_t>(n.args.size()), span);
}

void Emitter::emit_quote(const core::Quote& n, Context& ctx, const Span& span) {
    namespace P = reader::parser;

    auto datum_to_lispval = [&](auto&& self, const P::SExpr& expr)
        -> std::expected<LispVal, RuntimeError> {
        return std::visit([&](const auto& node) -> std::expected<LispVal, RuntimeError> {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, P::Nil>) {
                return Nil;
            } else if constexpr (std::is_same_v<T, P::Bool>) {
                return node.value ? True : False;
            } else if constexpr (std::is_same_v<T, P::Char>) {
                return ops::encode(node.value).value();
            } else if constexpr (std::is_same_v<T, P::String>) {
                return make_string(heap_, intern_table_, node.value);
            } else if constexpr (std::is_same_v<T, P::Symbol>) {
                auto res = intern_table_.intern(node.name);
                if (!res) return std::unexpected(res.error());
                return ops::box(Tag::Symbol, *res);
            } else if constexpr (std::is_same_v<T, P::Number>) {
                return std::visit([&](auto&& v) -> std::expected<LispVal, RuntimeError> {
                    using N = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<N, int64_t>) return make_fixnum(heap_, v);
                    else if constexpr (std::is_same_v<N, double>) return make_flonum(v);
                    return Nil;
                }, node.value);
            } else if constexpr (std::is_same_v<T, P::List>) {
                LispVal result;
                if (node.dotted && node.tail) {
                    auto tail_res = self(self, *node.tail);
                    if (!tail_res) return std::unexpected(tail_res.error());
                    result = *tail_res;
                } else {
                    result = Nil;
                }
                for (auto it = node.elems.rbegin(); it != node.elems.rend(); ++it) {
                    if (!*it) continue;
                    auto elem = self(self, **it);
                    if (!elem) return std::unexpected(elem.error());
                    auto cons = make_cons(heap_, *elem, result);
                    if (!cons) return std::unexpected(cons.error());
                    result = *cons;
                }
                return result;
            } else if constexpr (std::is_same_v<T, P::Vector>) {
                std::vector<LispVal> elems;
                elems.reserve(node.elems.size());
                for (const auto& e : node.elems) {
                    if (!e) continue;
                    auto elem = self(self, *e);
                    if (!elem) return std::unexpected(elem.error());
                    elems.push_back(*elem);
                }
                return make_vector(heap_, std::move(elems));
            } else if constexpr (std::is_same_v<T, P::ByteVector>) {
                return make_bytevector(heap_, node.bytes);
            } else if constexpr (std::is_same_v<T, P::ReaderForm>) {
                if (node.kind == P::QuoteKind::Quote && node.expr) {
                    auto sym_res = intern_table_.intern("quote");
                    if (!sym_res) return std::unexpected(sym_res.error());
                    auto sym_val = ops::box(Tag::Symbol, *sym_res);
                    auto datum_val = self(self, *node.expr);
                    if (!datum_val) return std::unexpected(datum_val.error());
                    auto inner = make_cons(heap_, *datum_val, Nil);
                    if (!inner) return std::unexpected(inner.error());
                    return make_cons(heap_, sym_val, *inner);
                }
                return Nil;
            } else {
                return Nil;
            }
        }, expr.value);
    };

    if (!n.datum) {
        emit_load_const(Nil, ctx, span);
        return;
    }

    auto result = datum_to_lispval(datum_to_lispval, *n.datum);
    emit_load_const(result ? *result : Nil, ctx, span);
}

uint32_t Emitter::emit_lambda(const core::Lambda& lambda,
                              const std::string& parent_name, const Span& span) {
    Context ctx;
    ctx.func.name       = parent_name + "_lambda" + std::to_string(lambda_count_++);
    ctx.func.arity      = lambda.arity.required;
    ctx.func.has_rest   = lambda.arity.has_rest;
    ctx.func.stack_size = lambda.stack_size;

    emit_node(lambda.body, ctx);
    ctx.emit_instr(OpCode::Return, 0, span);

    return registry_.add(std::move(ctx.func));
}

} // namespace eta::semantics
