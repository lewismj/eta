#pragma once

#include "eta/semantics/optimization_pass.h"
#include "eta/semantics/ir_visitor.h"
#include "eta/semantics/core_ir.h"

namespace eta::semantics::passes {

/**
 * @brief IR-level constant folding pass.
 *
 * Folds constant arithmetic expressions at the IR level:
 *
 * Only folds calls to builtin arithmetic primitives (+, -, *, /)
 * where both arguments are Const literal nodes with numeric payloads.
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
            auto* call = std::get_if<core::Call>(&node->data);
            if (!call) return node;

            /// Must be a call to a global variable (builtin)
            auto* callee_var = std::get_if<core::Var>(&call->callee->data);
            if (!callee_var) return node;
            auto* global = std::get_if<core::Address::Global>(&callee_var->addr.where);
            if (!global) return node;

            /// Must be exactly 2 arguments
            if (call->args.size() != 2) return node;

            /// Both args must be Const with numeric literals
            auto* lhs_const = std::get_if<core::Const>(&call->args[0]->data);
            auto* rhs_const = std::get_if<core::Const>(&call->args[1]->data);
            if (!lhs_const || !rhs_const) return node;

            /// Extract numeric values
            auto lhs_num = to_numeric(lhs_const->value);
            auto rhs_num = to_numeric(rhs_const->value);
            if (!lhs_num || !rhs_num) return node;

            /**
             * We need to know which builtin the global slot refers to.
             * Builtins are at fixed global slots. We identify them by slot number.
             * The binding info for the callee tells us its name through the module.
             * we instead check if the callee's address matches a known arithmetic
             * builtin by looking up the binding name in the module's bindings list.
             */
            std::string op_name;
            for (const auto& bi : mod.bindings) {
                if (bi.kind == BindingInfo::Kind::Global && bi.slot == global->id) {
                    op_name = bi.name;
                    break;
                }
            }
            if (op_name.empty()) return node;

            /// Fold the operation
            std::optional<core::Literal> result;
            if (op_name == "+")      result = fold_add(*lhs_num, *rhs_num);
            else if (op_name == "-") result = fold_sub(*lhs_num, *rhs_num);
            else if (op_name == "*") result = fold_mul(*lhs_num, *rhs_num);
            else if (op_name == "/") result = fold_div(*lhs_num, *rhs_num);

            if (!result) return node;

            return mod.emplace<core::Const>(node->span, *result);
        }

    private:
        struct NumVal {
            bool is_int;
            int64_t i;
            double d;
        };

        static std::optional<NumVal> to_numeric(const core::Literal& lit) {
            if (auto* iv = std::get_if<int64_t>(&lit.payload)) {
                return NumVal{true, *iv, static_cast<double>(*iv)};
            }
            if (auto* dv = std::get_if<double>(&lit.payload)) {
                return NumVal{false, 0, *dv};
            }
            return std::nullopt;
        }

        static core::Literal make_result(const NumVal& a, const NumVal& b, int64_t i_result, double d_result) {
            if (a.is_int && b.is_int) {
                return core::Literal{i_result};
            }
            return core::Literal{d_result};
        }

        static std::optional<core::Literal> fold_add(const NumVal& a, const NumVal& b) {
            return make_result(a, b, a.i + b.i, a.d + b.d);
        }

        static std::optional<core::Literal> fold_sub(const NumVal& a, const NumVal& b) {
            return make_result(a, b, a.i - b.i, a.d - b.d);
        }

        static std::optional<core::Literal> fold_mul(const NumVal& a, const NumVal& b) {
            return make_result(a, b, a.i * b.i, a.d * b.d);
        }

        static std::optional<core::Literal> fold_div(const NumVal& a, const NumVal& b) {
            if (b.d == 0.0) return std::nullopt; ///< avoid division by zero
            if (a.is_int && b.is_int && b.i != 0 && a.i % b.i == 0) {
                return core::Literal{a.i / b.i};
            }
            return core::Literal{a.d / b.d};
        }
    };
};

} ///< namespace eta::semantics::passes

