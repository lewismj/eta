#pragma once

#include <optional>
#include <string>

#include "eta/semantics/optimization_pass.h"
#include "eta/semantics/ir_visitor.h"
#include "eta/semantics/core_ir.h"
#include "eta/runtime/numeric_ops.h"

namespace eta::semantics::passes {

/**
 * @brief IR-level constant folding pass.
 *
 * Folds constant arithmetic expressions at the IR level:
 *
 * Only folds calls to builtin arithmetic primitives (+, -, *, /), either
 * still as calls or already lowered to PrimitiveCall, where both arguments
 * are Const literal nodes with numeric payloads.  Lowered n-ary chains fold
 * bottom-up, so (+ 1 2 3) becomes 6.
 *
 * This is a conservative peephole: non-constant operands are left untouched.
 */
class ConstantFolding : public OptimizationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "constant-folding";
    }

    void run(ModuleSemantics& mod) override {
        Folder folder{mod};
        for (auto*& node : mod.toplevel_inits) {
            node = folder.visit(node, false);
        }
    }

private:
    struct Folder : IRVisitor<Folder> {
        ModuleSemantics& mod;

        explicit Folder(ModuleSemantics& m) : mod(m) {}

        core::Node* post_visit(core::Node* node, bool /*ctx*/) {
            using runtime::numeric_ops::ArithOp;

            /// Already lowered by PrimitiveSpecialisation (which the Driver always runs)
            if (auto* prim = std::get_if<core::PrimitiveCall>(&node->data)) {
                auto op = arith_op_for(prim->kind);
                if (!op || prim->args.size() != 2) return node;
                return fold(node, *op, prim->args[0], prim->args[1]);
            }

            auto* call = std::get_if<core::Call>(&node->data);
            if (!call) return node;

            /// Must be a call to a global variable (builtin)
            auto* callee_var = std::get_if<core::Var>(&call->callee->data);
            if (!callee_var) return node;
            auto* global = std::get_if<core::Address::Global>(&callee_var->addr.where);
            if (!global) return node;

            /// Must be exactly 2 arguments
            if (call->args.size() != 2) return node;

            /**
             * We need to know which builtin the global slot refers to.
             * Builtins are at fixed global slots, seeded as immutable bindings;
             * a user (define (+ a b) ...) is a mutable global with its own slot
             * and must not be folded.
             */
            std::string op_name;
            for (const auto& bi : mod.bindings) {
                if (bi.kind == BindingInfo::Kind::Global && bi.slot == global->id) {
                    if (!bi.mutable_flag) op_name = bi.name;
                    break;
                }
            }
            if (op_name.empty()) return node;

            ArithOp op;
            if (op_name == "+")      op = ArithOp::Add;
            else if (op_name == "-") op = ArithOp::Sub;
            else if (op_name == "*") op = ArithOp::Mul;
            else if (op_name == "/") op = ArithOp::Div;
            else return node;

            return fold(node, op, call->args[0], call->args[1]);
        }

    private:
        /// Both args must be Const with numeric literals
        core::Node* fold(core::Node* node, runtime::numeric_ops::ArithOp op,
                         const core::Node* lhs, const core::Node* rhs) {
            auto* lhs_const = std::get_if<core::Const>(&lhs->data);
            auto* rhs_const = std::get_if<core::Const>(&rhs->data);
            if (!lhs_const || !rhs_const) return node;

            auto lhs_num = to_numeric(lhs_const->value);
            auto rhs_num = to_numeric(rhs_const->value);
            if (!lhs_num || !rhs_num) return node;

            /// Fold with the runtime's own semantics (overflow promotion, exact division).
            /// Division by zero is left for the runtime to report.
            const auto result = runtime::numeric_ops::arith(op, *lhs_num, *rhs_num);
            if (!result) return node;

            const core::Literal folded = result->is_fixnum() ? core::Literal{result->int_val}
                                                             : core::Literal{result->float_val};
            return mod.emplace<core::Const>(node->span, folded);
        }

        static std::optional<runtime::numeric_ops::ArithOp> arith_op_for(core::PrimitiveKind kind) {
            using runtime::numeric_ops::ArithOp;
            if (kind == core::PrimitiveKind::Add) return ArithOp::Add;
            if (kind == core::PrimitiveKind::Sub) return ArithOp::Sub;
            if (kind == core::PrimitiveKind::Mul) return ArithOp::Mul;
            if (kind == core::PrimitiveKind::Div) return ArithOp::Div;
            return std::nullopt;
        }

        static std::optional<runtime::NumericValue> to_numeric(const core::Literal& lit) {
            using runtime::NumType;
            if (auto* iv = std::get_if<int64_t>(&lit.payload)) {
                return runtime::NumericValue{NumType::Fixnum, *iv, 0.0};
            }
            if (auto* dv = std::get_if<double>(&lit.payload)) {
                return runtime::NumericValue{NumType::Flonum, 0, *dv};
            }
            return std::nullopt;
        }
    };
};

} ///< namespace eta::semantics::passes

