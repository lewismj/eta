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
 * VM opcodes (`Add`, `Sub`, `Mul`, `Div`, `Eq`, `Cons`, `Car`, `Cdr`).
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
            if (name == "=") return core::PrimitiveKind::Eq;
            if (name == "cons") return core::PrimitiveKind::Cons;
            return std::nullopt;
        }
    };
};

} ///< namespace eta::semantics::passes
