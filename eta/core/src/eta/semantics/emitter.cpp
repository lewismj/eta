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

std::vector<BytecodeFunction*> Emitter::emit() {
    std::vector<BytecodeFunction*> out;
    for (const auto& node : sem_.toplevel_inits) {
        if (const auto* lambda = std::get_if<core::Lambda>(static_cast<const core::NodeBase*>(node))) {
            uint32_t idx = emit_lambda(*lambda);
            out.push_back(registry_.get_mut(idx));
        }
    }
    return out;
}

void Emitter::emit_node(const core::Node* node, Context& ctx) {
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, core::Const>) emit_const(n, ctx);
        else if constexpr (std::is_same_v<T, core::Var>) emit_var(n, ctx);
        else if constexpr (std::is_same_v<T, core::Call>) emit_call(n, ctx);
        else if constexpr (std::is_same_v<T, core::If>) emit_if(n, ctx);
        else if constexpr (std::is_same_v<T, core::Begin>) emit_begin(n, ctx);
        else if constexpr (std::is_same_v<T, core::Lambda>) emit_lambda_node(n, ctx);
        else if constexpr (std::is_same_v<T, core::Set>) emit_set(n, ctx);
        else if constexpr (std::is_same_v<T, core::Values>) emit_values(n, ctx);
        else if constexpr (std::is_same_v<T, core::CallWithValues>) emit_call_with_values(n, ctx);
        else if constexpr (std::is_same_v<T, core::DynamicWind>) emit_dynamic_wind(n, ctx);
        else if constexpr (std::is_same_v<T, core::CallCC>) emit_call_cc(n, ctx);
        else if constexpr (std::is_same_v<T, core::Quote>) emit_quote(n, ctx);
    }, *node);
}

void Emitter::emit_const(const core::Const& n, Context& ctx) {
    std::optional<LispVal> val = std::visit([&](auto&& p) -> std::optional<LispVal> {
        using PT = std::decay_t<decltype(p)>;

        if constexpr (std::is_same_v<PT, std::monostate>) {
            return Nil;
        } else if constexpr (std::is_same_v<PT, bool>) {
            return p ? True : False;
        } else if constexpr (std::is_same_v<PT, char32_t>) {
            return ops::encode(p).value();
        } else if constexpr (std::is_same_v<PT, std::string>) {
            // Deduplicate string constants across the current function
            if (auto cache_it = ctx.string_constant_cache.find(p); cache_it != ctx.string_constant_cache.end()) {
                ctx.func.code.push_back({OpCode::LoadConst, cache_it->second});
                return std::nullopt;
            }
            LispVal v = make_string(heap_, intern_table_, p).value();
            uint32_t idx = emit_load_const(v, ctx);
            ctx.string_constant_cache[p] = idx;
            return std::nullopt;
        } else if constexpr (std::is_same_v<PT, int64_t>) {
            return make_fixnum(heap_, p).value();
        } else if constexpr (std::is_same_v<PT, double>) {
            return make_flonum(p).value();
        }
        return Nil;
    }, n.value.payload);

    if (val) {
        emit_load_const(*val, ctx);
    }
}

uint32_t Emitter::emit_load_const(LispVal val, Context& ctx) {
    uint32_t idx = static_cast<uint32_t>(ctx.func.constants.size());
    ctx.func.constants.push_back(val);
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

void Emitter::emit_call(const core::Call& n, Context& ctx) {
    for (const auto* arg : n.args) {
        emit_node(arg, ctx);
    }
    emit_node(n.callee, ctx);
    ctx.func.code.push_back({n.tail ? OpCode::TailCall : OpCode::Call, static_cast<uint32_t>(n.args.size())});
}

void Emitter::emit_if(const core::If& n, Context& ctx) {
    emit_node(n.test, ctx);
    uint32_t jump_if_false_idx = static_cast<uint32_t>(ctx.func.code.size());
    ctx.func.code.push_back({OpCode::JumpIfFalse, 0}); // placeholder

    emit_node(n.conseq, ctx);
    uint32_t jump_idx = static_cast<uint32_t>(ctx.func.code.size());
    ctx.func.code.push_back({OpCode::Jump, 0}); // placeholder

    ctx.func.code[jump_if_false_idx].arg = static_cast<uint32_t>(ctx.func.code.size() - jump_if_false_idx);

    emit_node(n.alt, ctx);
    ctx.func.code[jump_idx].arg = static_cast<uint32_t>(ctx.func.code.size() - jump_idx);
}

void Emitter::emit_begin(const core::Begin& n, Context& ctx) {
    for (size_t i = 0; i < n.exprs.size(); ++i) {
        emit_node(n.exprs[i], ctx);
        if (i < n.exprs.size() - 1) {
            ctx.func.code.push_back({OpCode::Pop, 0});
        }
    }
}

void Emitter::emit_lambda_node(const core::Lambda& n, Context& ctx) {
    // Emit the nested lambda to the registry and get its index
    uint32_t func_idx = emit_lambda(n);

    // Store the function index as a constant (type-safe, not a raw pointer).
    // Uses FUNC_INDEX_TAG from bytecode.h
    LispVal func_idx_val = FUNC_INDEX_TAG | static_cast<uint64_t>(func_idx);

    uint32_t const_idx = emit_load_const(func_idx_val, ctx);

    uint32_t num_upvals = static_cast<uint32_t>(n.upvals.size());
    // Push upvals before MakeClosure - each captured variable must be loaded
    for (const auto& bid : n.upvals) {
        if (auto it = ctx.binding_to_slot.find(bid); it != ctx.binding_to_slot.end()) {
            ctx.func.code.push_back({OpCode::LoadLocal, it->second});
        } else if (auto it_up = ctx.binding_to_upval.find(bid); it_up != ctx.binding_to_upval.end()) {
            ctx.func.code.push_back({OpCode::LoadUpval, it_up->second});
        } else {
            ctx.func.code.push_back({OpCode::LoadGlobal, bid.id});
        }
    }
    ctx.func.code.push_back({OpCode::MakeClosure, (const_idx << 16) | num_upvals});
}

void Emitter::emit_set(const core::Set& n, Context& ctx) {
    emit_node(n.value, ctx);
    emit_address_store(n.target, ctx);
    // set! returns unspecified value (we push nil)
    emit_load_const(Nil, ctx);
}

void Emitter::emit_values(const core::Values& n, Context& ctx) {
    for (const auto* expr : n.exprs) {
        emit_node(expr, ctx);
    }
    ctx.func.code.push_back({OpCode::Values, static_cast<uint32_t>(n.exprs.size())});
}

void Emitter::emit_call_with_values(const core::CallWithValues& n, Context& ctx) {
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

void Emitter::emit_call_cc(const core::CallCC& n, Context& ctx) {
    emit_node(n.consumer, ctx);
    ctx.func.code.push_back({OpCode::CallCC, 0});
}

void Emitter::emit_quote(const core::Quote& n, Context& ctx) {
    // TODO: Implement proper datum->LispVal conversion
    // For now, emit nil as placeholder
    emit_load_const(Nil, ctx);
}


uint32_t Emitter::emit_lambda(const core::Lambda& lambda) {
    Context ctx;
    ctx.func.arity = lambda.arity.required;
    ctx.func.has_rest = lambda.arity.has_rest;
    
    // Map parameters to local slots (they're pushed by the caller)
    uint32_t slot = 0;
    for (const auto& param : lambda.params) {
        ctx.binding_to_slot[param] = slot++;
    }
    if (lambda.rest.has_value()) {
        ctx.binding_to_slot[*lambda.rest] = slot++;
    }

    // Map upvalues to their indices (for LoadUpval in nested lambdas)
    for (uint32_t i = 0; i < lambda.upvals.size(); ++i) {
        ctx.binding_to_upval[lambda.upvals[i]] = i;
    }

    emit_node(lambda.body, ctx);
    ctx.func.code.push_back({OpCode::Return, 0});

    // Add to registry and return the index
    return registry_.add(std::move(ctx.func));
}

} // namespace eta::semantics
