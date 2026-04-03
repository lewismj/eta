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

BytecodeFunction* Emitter::emit() {
    Context ctx;
    ctx.func.name = sem_.name + "_init";
    ctx.func.stack_size = sem_.stack_size;
    ctx.func.arity = 0;
    ctx.func.has_rest = false;
    
    for (const auto* node : sem_.toplevel_inits) {
        emit_node(node, ctx);
        // Toplevel forms that are expressions will leave values on the stack.
        // We pop them to keep the stack clean.
        ctx.func.code.push_back({OpCode::Pop, 0});
    }
    
    // Module init returns Nil by default.
    ctx.func.code.push_back({OpCode::LoadConst, emit_load_const(Nil, ctx)});
    ctx.func.code.push_back({OpCode::Return, 0});

    uint32_t idx = registry_.add(std::move(ctx.func));
    return registry_.get_mut(idx);
}

void Emitter::emit_node(const core::Node* node, Context& ctx) {
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, core::Const>) emit_const(n, ctx);
        else if constexpr (std::is_same_v<T, core::Var>) emit_var(n, ctx);
        else if constexpr (std::is_same_v<T, core::Call>) emit_call(n, node->tail, ctx);
        else if constexpr (std::is_same_v<T, core::If>) emit_if(n, ctx);
        else if constexpr (std::is_same_v<T, core::Begin>) emit_begin(n, ctx);
        else if constexpr (std::is_same_v<T, core::Lambda>) emit_lambda_node(n, node->span, ctx);
        else if constexpr (std::is_same_v<T, core::Set>) emit_set(n, ctx);
        else if constexpr (std::is_same_v<T, core::Values>) emit_values(n, ctx);
        else if constexpr (std::is_same_v<T, core::CallWithValues>) emit_call_with_values(n, node->tail, ctx);
        else if constexpr (std::is_same_v<T, core::DynamicWind>) emit_dynamic_wind(n, ctx);
        else if constexpr (std::is_same_v<T, core::CallCC>) emit_call_cc(n, node->tail, ctx);
        else if constexpr (std::is_same_v<T, core::Quote>) emit_quote(n, ctx);
    }, node->data);
}

void Emitter::emit_const(const core::Const& n, Context& ctx) {
    if (const auto* s = std::get_if<std::string>(&n.value.payload)) {
        if (auto it = ctx.string_constant_cache.find(*s); it != ctx.string_constant_cache.end()) {
            ctx.func.code.push_back({OpCode::LoadConst, it->second});
            return;
        }
    }

    LispVal val = std::visit([&](auto&& p) -> LispVal {
        using PT = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<PT, std::monostate>) return Nil;
        else if constexpr (std::is_same_v<PT, bool>) return p ? True : False;
        else if constexpr (std::is_same_v<PT, char32_t>) return ops::encode(p).value();
        else if constexpr (std::is_same_v<PT, std::string>) return make_string(heap_, intern_table_, p).value();
        else if constexpr (std::is_same_v<PT, int64_t>) return make_fixnum(heap_, p).value();
        else if constexpr (std::is_same_v<PT, double>) return make_flonum(p).value();
        return Nil;
    }, n.value.payload);

    uint32_t idx = emit_load_const(val, ctx);
    if (const auto* s = std::get_if<std::string>(&n.value.payload)) {
        ctx.string_constant_cache[*s] = idx;
    }
}

uint32_t Emitter::add_const(LispVal val, Context& ctx) {
    uint32_t idx = static_cast<uint32_t>(ctx.func.constants.size());
    ctx.func.constants.push_back(val);
    return idx;
}

uint32_t Emitter::emit_load_const(LispVal val, Context& ctx) {
    uint32_t idx = add_const(val, ctx);
    ctx.func.code.push_back({OpCode::LoadConst, idx});
    return idx;
}

void Emitter::emit_address_load(const core::Address& addr, Context& ctx) {
    std::visit([&](auto&& a) {
        using AT = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<AT, core::Address::Local>) {
            ctx.func.code.push_back({OpCode::LoadLocal, a.slot});
        } else if constexpr (std::is_same_v<AT, core::Address::Upval>) {
            ctx.func.code.push_back({OpCode::LoadUpval, a.slot});
        } else if constexpr (std::is_same_v<AT, core::Address::Global>) {
            ctx.func.code.push_back({OpCode::LoadGlobal, a.id});
        }
    }, addr.where);
}

void Emitter::emit_address_store(const core::Address& addr, Context& ctx) {
    std::visit([&](auto&& a) {
        using AT = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<AT, core::Address::Local>) {
            ctx.func.code.push_back({OpCode::StoreLocal, a.slot});
        } else if constexpr (std::is_same_v<AT, core::Address::Upval>) {
            ctx.func.code.push_back({OpCode::StoreUpval, a.slot});
        } else if constexpr (std::is_same_v<AT, core::Address::Global>) {
            ctx.func.code.push_back({OpCode::StoreGlobal, a.id});
        }
    }, addr.where);
}

void Emitter::emit_var(const core::Var& n, Context& ctx) {
    emit_address_load(n.addr, ctx);
}

void Emitter::emit_call(const core::Call& n, bool tail, Context& ctx) {
    for (const auto* arg : n.args) {
        emit_node(arg, ctx);
    }
    emit_node(n.callee, ctx);
    ctx.func.code.push_back({tail ? OpCode::TailCall : OpCode::Call, static_cast<uint32_t>(n.args.size())});
}

void Emitter::emit_if(const core::If& n, Context& ctx) {
    emit_node(n.test, ctx);
    uint32_t jump_if_false_idx = static_cast<uint32_t>(ctx.func.code.size());
    ctx.func.code.push_back({OpCode::JumpIfFalse, 0}); // placeholder

    emit_node(n.conseq, ctx);
    uint32_t jump_idx = static_cast<uint32_t>(ctx.func.code.size());
    ctx.func.code.push_back({OpCode::Jump, 0}); // placeholder

    ctx.func.code[jump_if_false_idx].arg = static_cast<uint32_t>(ctx.func.code.size() - jump_if_false_idx - 1);

    emit_node(n.alt, ctx);
    ctx.func.code[jump_idx].arg = static_cast<uint32_t>(ctx.func.code.size() - jump_idx - 1);
}

void Emitter::emit_begin(const core::Begin& n, Context& ctx) {
    for (size_t i = 0; i < n.exprs.size(); ++i) {
        emit_node(n.exprs[i], ctx);
        if (i < n.exprs.size() - 1) {
            ctx.func.code.push_back({OpCode::Pop, 0});
        }
    }
}

void Emitter::emit_lambda_node(const core::Lambda& n, const eta::reader::parser::Span& span, Context& ctx) {
    // Emit the nested lambda to the registry and get its index
    uint32_t func_idx = emit_lambda(n, ctx.func.name, span);

    // Store the function index as a constant (type-safe, not a raw pointer).
    // Uses FUNC_INDEX_TAG from bytecode.h
    LispVal func_idx_val = encode_func_index(func_idx);

    uint32_t const_idx = add_const(func_idx_val, ctx);

    uint32_t num_upvals = static_cast<uint32_t>(n.upval_sources.size());
    // Push upval sources before MakeClosure
    for (const auto& src : n.upval_sources) {
        emit_address_load(src, ctx);
    }
    ctx.func.code.push_back({OpCode::MakeClosure, (const_idx << 16) | num_upvals});
}

void Emitter::emit_set(const core::Set& n, Context& ctx) {
    emit_node(n.value, ctx);
    emit_address_store(n.target, ctx);

    // Fixup for letrec self-reference: if the value is a lambda that captured
    // the target variable as an upval, the upval was stale (nil) at MakeClosure
    // time. Patch it now that the local holds the correct closure.
    if (const auto* lam = std::get_if<core::Lambda>(&n.value->data)) {
        if (const auto* target_local = std::get_if<core::Address::Local>(&n.target.where)) {
            for (size_t i = 0; i < lam->upval_sources.size(); ++i) {
                if (const auto* src_local = std::get_if<core::Address::Local>(&lam->upval_sources[i].where)) {
                    if (src_local->slot == target_local->slot) {
                        // Emit: push closure, push value, PatchClosureUpval
                        ctx.func.code.push_back({OpCode::LoadLocal, target_local->slot}); // closure
                        ctx.func.code.push_back({OpCode::LoadLocal, target_local->slot}); // value (self)
                        ctx.func.code.push_back({OpCode::PatchClosureUpval, static_cast<uint32_t>(i)});
                    }
                }
            }
        }
    }

    // set! returns unspecified value (we push nil)
    emit_load_const(Nil, ctx);
}

void Emitter::emit_values(const core::Values& n, Context& ctx) {
    for (const auto* expr : n.exprs) {
        emit_node(expr, ctx);
    }
    ctx.func.code.push_back({OpCode::Values, static_cast<uint32_t>(n.exprs.size())});
}

void Emitter::emit_call_with_values(const core::CallWithValues& n, bool /*tail*/, Context& ctx) {
    emit_node(n.producer, ctx);
    emit_node(n.consumer, ctx);
    ctx.func.code.push_back({OpCode::CallWithValues, 0});
}

void Emitter::emit_dynamic_wind(const core::DynamicWind& n, Context& ctx) {
    emit_node(n.before, ctx);
    emit_node(n.body, ctx);
    emit_node(n.after, ctx);
    ctx.func.code.push_back({OpCode::DynamicWind, 0});
}

void Emitter::emit_call_cc(const core::CallCC& n, bool /*tail*/, Context& ctx) {
    emit_node(n.consumer, ctx);
    ctx.func.code.push_back({OpCode::CallCC, 0});
}

void Emitter::emit_quote(const core::Quote& n, Context& ctx) {
    // Namespace aliases to disambiguate parser types from runtime types
    namespace P = reader::parser;

    // Convert an arbitrary S-expression datum into a runtime LispVal.
    // For compound data (lists, vectors) this recursively builds cons chains / vectors.
    auto datum_to_lispval = [&](auto&& self, const P::SExpr& expr) -> std::expected<LispVal, RuntimeError> {
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
                // Quoted symbols become runtime Symbol values
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
                // Build cons chain from right to left
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
                // (quote datum) inside a quote — treated as quoted data
                if (node.kind == P::QuoteKind::Quote && node.expr) {
                    // Build (quote <datum>) as a list: cons(symbol("quote"), cons(datum, nil))
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
                // ModuleForm or other unsupported types
                return Nil;
            }
        }, expr.value);
    };

    if (!n.datum) {
        emit_load_const(Nil, ctx);
        return;
    }

    auto result = datum_to_lispval(datum_to_lispval, *n.datum);
    if (result) {
        emit_load_const(*result, ctx);
    } else {
        // Fallback: emit nil on conversion failure
        emit_load_const(Nil, ctx);
    }
}


uint32_t Emitter::emit_lambda(const core::Lambda& lambda, const std::string& parent_name, const eta::reader::parser::Span& span) {
    static uint32_t lambda_count = 0;
    Context ctx;
    ctx.func.name = parent_name + "_lambda" + std::to_string(lambda_count++);
    ctx.func.arity = lambda.arity.required;
    ctx.func.has_rest = lambda.arity.has_rest;
    ctx.func.stack_size = lambda.stack_size;
    
    // Addresses (including local slots and upval indices) were pre-resolved 
    // by the semantic analyzer. We can emit the body directly.

    emit_node(lambda.body, ctx);
    ctx.func.code.push_back({OpCode::Return, 0});

    // Add to registry and return the index
    return registry_.add(std::move(ctx.func));
}

} // namespace eta::semantics
