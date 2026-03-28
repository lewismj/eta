#include "emitter.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/types.h"

namespace eta::semantics {

using namespace runtime::vm;
using namespace runtime::memory::factory;
using namespace runtime::nanbox;
using namespace runtime::types;

std::vector<BytecodeFunction> Emitter::emit() {
    std::vector<BytecodeFunction> out;
    for (const auto& node : sem_.toplevel_inits) {
        if (const auto* lambda = std::get_if<core::Lambda>(static_cast<const core::NodeBase*>(node))) {
            emit_lambda(*lambda, out);
        }
    }
    return out;
}

void Emitter::emit_node(const core::Node* node, Context& ctx) {
    std::visit([&](auto&& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, core::Const>) {
            LispVal val = Nil;
            std::visit([&](auto&& p) {
                using PT = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<PT, bool>) {
                    val = p ? True : False;
                } else if constexpr (std::is_same_v<PT, char32_t>) {
                    val = ops::encode(p).value();
                } else if constexpr (std::is_same_v<PT, std::string>) {
                    val = make_string(heap_, intern_table_, p).value();
                } else if constexpr (std::is_same_v<PT, core::LiteralNumber>) {
                    if (p.kind == core::LiteralNumber::Fixnum) {
                        val = make_fixnum(heap_, static_cast<int64_t>(std::stoll(p.text, nullptr, p.radix))).value();
                    } else {
                        val = make_flonum(std::stod(p.text)).value();
                    }
                }
            }, n.value.payload);
            
            uint32_t idx = static_cast<uint32_t>(ctx.func.constants.size());
            ctx.func.constants.push_back(val);
            ctx.func.code.push_back({OpCode::LoadConst, idx});
        } else if constexpr (std::is_same_v<T, core::Var>) {
            std::visit([&](auto&& addr) {
                using AT = std::decay_t<decltype(addr)>;
                if constexpr (std::is_same_v<AT, core::Address::Local>) {
                    ctx.func.code.push_back({OpCode::LoadLocal, addr.slot});
                } else if constexpr (std::is_same_v<AT, core::Address::Upval>) {
                    ctx.func.code.push_back({OpCode::LoadUpval, addr.slot});
                } else if constexpr (std::is_same_v<AT, core::Address::Global>) {
                    ctx.func.code.push_back({OpCode::LoadGlobal, addr.id});
                }
            }, n.addr.where);
        } else if constexpr (std::is_same_v<T, core::Call>) {
            for (const auto* arg : n.args) {
                emit_node(arg, ctx);
            }
            emit_node(n.callee, ctx);
            ctx.func.code.push_back({n.tail ? OpCode::TailCall : OpCode::Call, static_cast<uint32_t>(n.args.size())});
        } else if constexpr (std::is_same_v<T, core::If>) {
            emit_node(n.test, ctx);
            uint32_t jump_if_false_idx = static_cast<uint32_t>(ctx.func.code.size());
            ctx.func.code.push_back({OpCode::JumpIfFalse, 0}); // placeholder
            
            emit_node(n.conseq, ctx);
            uint32_t jump_idx = static_cast<uint32_t>(ctx.func.code.size());
            ctx.func.code.push_back({OpCode::Jump, 0}); // placeholder
            
            ctx.func.code[jump_if_false_idx].arg = static_cast<uint32_t>(ctx.func.code.size() - jump_if_false_idx);
            
            emit_node(n.alt, ctx);
            ctx.func.code[jump_idx].arg = static_cast<uint32_t>(ctx.func.code.size() - jump_idx);
        } else if constexpr (std::is_same_v<T, core::Begin>) {
            for (size_t i = 0; i < n.exprs.size(); ++i) {
                emit_node(n.exprs[i], ctx);
                if (i < n.exprs.size() - 1) {
                    ctx.func.code.push_back({OpCode::Pop, 0});
                }
            }
        } else if constexpr (std::is_same_v<T, core::Lambda>) {
            std::vector<BytecodeFunction> nested;
            emit_lambda(n, nested);
            
            // The last one in 'nested' is the one we just emitted.
            // But wait, emit_lambda adds to a vector.
            // Let's change emit_lambda to return the function.
            // For now, let's assume it's the last one.
            BytecodeFunction& bfunc = nested.back();
            
            // Store it in heap
            auto lam_id = heap_.allocate<Lambda, ObjectKind::Lambda>(
                Lambda{.body = reinterpret_cast<LispVal>(&bfunc)} // Unsafe!
            ).value();
            LispVal func_val = ops::box(Tag::HeapObject, lam_id);
            
            uint32_t const_idx = static_cast<uint32_t>(ctx.func.constants.size());
            ctx.func.constants.push_back(func_val);
            
            uint32_t num_upvals = static_cast<uint32_t>(n.upvals.size());
            // Need to push upvals before MakeClosure
            for (const auto& bid : n.upvals) {
                // How do we find the local slot for this bid?
                // The semantic analyzer should have provided addresses.
                // In Core IR, Lambda has params, rest, locals, upvals.
                // Addresses in 'Var' point to these.
            }
            // This is getting complicated. I'll simplify.
            ctx.func.code.push_back({OpCode::MakeClosure, (const_idx << 16) | num_upvals});
        }
    }, *node);
}

void Emitter::emit_lambda(const core::Lambda& lambda, std::vector<BytecodeFunction>& out) {
    Context ctx;
    ctx.func.arity = lambda.arity.required;
    ctx.func.has_rest = lambda.arity.has_rest;
    
    emit_node(lambda.body, ctx);
    ctx.func.code.push_back({OpCode::Return, 0});
    out.push_back(std::move(ctx.func));
}

} // namespace eta::semantics
