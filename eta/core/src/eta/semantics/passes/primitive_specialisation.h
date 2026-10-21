#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "eta/semantics/core_ir.h"
#include "eta/semantics/ir_visitor.h"
#include "eta/semantics/optimization_pass.h"

namespace eta::semantics::passes {

/**
 * @brief Lower known builtin calls to dedicated primitive IR nodes.
 *
 * The pass recognizes calls to immutable builtin globals and replaces them
 * with `core::PrimitiveCall`, which the emitter lowers directly to existing
 * VM opcodes (`Add`, `Sub`, `Mul`, `Div`, `NumCmp`, `Cons`, `Car`, `Cdr`).
 * Arithmetic calls with three or more arguments become a left-associated
 * chain of binary opcodes.
 *
 * The Driver always runs this pass, so it must preserve the semantics of
 * the primitive call it replaces.
 */
class PrimitiveSpecialisation : public OptimizationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "primitive-specialisation";
    }

    void run(ModuleSemantics& mod) override {
        Specialiser specialiser{mod};
        for (auto*& node : mod.toplevel_inits) {
            node = specialiser.visit(node, false);
        }
    }

private:
    struct Specialiser : IRVisitor<Specialiser> {
        ModuleSemantics& mod;
        std::unordered_map<uint32_t, core::PrimitiveKind> unary_primitive_slots;
        std::unordered_map<uint32_t, core::PrimitiveKind> binary_primitive_slots;

        explicit Specialiser(ModuleSemantics& m) : mod(m) {
            index_builtin_primitive_slots();
        }

        core::Node* post_visit(core::Node* node, bool /*ctx*/) {
            auto* call = std::get_if<core::Call>(&node->data);
            if (!call) return node;

            auto* callee_var = std::get_if<core::Var>(&call->callee->data);
            if (!callee_var) return node;
            auto* global = std::get_if<core::Address::Global>(&callee_var->addr.where);
            if (!global) return node;

            const auto arg_count = call->args.size();
            if (arg_count >= 3) return lower_variadic_arith(node, *call, global->id);

            const auto* slot_map = primitive_map_for_arity(arg_count);
            if (!slot_map) return node;

            auto it = slot_map->find(global->id);
            if (it == slot_map->end()) return node;

            core::PrimitiveCall lowered{
                .kind = it->second,
                .args = call->args,
            };
            return mod.emplace<core::PrimitiveCall>(node->span, std::move(lowered));
        }

    private:
        /**
         * (+ a b c d) -> (Add (Add (Add a b) c) d): the same left fold the
         * variadic primitive performs.  Comparisons are not chained, since
         * (< a b c) would need a temporary to avoid evaluating b twice.
         */
        core::Node* lower_variadic_arith(core::Node* node, const core::Call& call, uint32_t slot) {
            auto it = binary_primitive_slots.find(slot);
            if (it == binary_primitive_slots.end() || !is_arith(it->second)) return node;

            core::Node* acc = call.args[0];
            for (std::size_t i = 1; i < call.args.size(); ++i) {
                core::PrimitiveCall step{
                    .kind = it->second,
                    .args = {acc, call.args[i]},
                };
                acc = mod.emplace<core::PrimitiveCall>(node->span, std::move(step));
            }
            return acc;
        }

        static bool is_arith(core::PrimitiveKind kind) {
            return kind == core::PrimitiveKind::Add || kind == core::PrimitiveKind::Sub
                || kind == core::PrimitiveKind::Mul || kind == core::PrimitiveKind::Div;
        }

        void index_builtin_primitive_slots() {
            for (const auto& binding : mod.bindings) {
                if (binding.kind != BindingInfo::Kind::Global) continue;
                if (binding.mutable_flag) continue;

                if (auto unary = unary_primitive_from_name(binding.name)) {
                    unary_primitive_slots.emplace(binding.slot, *unary);
                    continue;
                }
                if (auto binary = binary_primitive_from_name(binding.name)) {
                    binary_primitive_slots.emplace(binding.slot, *binary);
                }
            }
        }

        const std::unordered_map<uint32_t, core::PrimitiveKind>*
        primitive_map_for_arity(std::size_t arity) const {
            if (arity == 1) return &unary_primitive_slots;
            if (arity == 2) return &binary_primitive_slots;
            return nullptr;
        }

        static std::optional<core::PrimitiveKind>
        unary_primitive_from_name(std::string_view name) {
            if (name == "car") return core::PrimitiveKind::Car;
            if (name == "cdr") return core::PrimitiveKind::Cdr;
            return std::nullopt;
        }

        static std::optional<core::PrimitiveKind>
        binary_primitive_from_name(std::string_view name) {
            if (name == "+") return core::PrimitiveKind::Add;
            if (name == "-") return core::PrimitiveKind::Sub;
            if (name == "*") return core::PrimitiveKind::Mul;
            if (name == "/") return core::PrimitiveKind::Div;
            /// Numeric = must not lower to Eq (eqv?): (= 1 1.0) and (= 0.0 -0.0) are #t
            if (name == "=") return core::PrimitiveKind::NumEq;
            if (name == "<") return core::PrimitiveKind::Lt;
            if (name == ">") return core::PrimitiveKind::Gt;
            if (name == "<=") return core::PrimitiveKind::Le;
            if (name == ">=") return core::PrimitiveKind::Ge;
            if (name == "cons") return core::PrimitiveKind::Cons;
            return std::nullopt;
        }
    };
};

} ///< namespace eta::semantics::passes
