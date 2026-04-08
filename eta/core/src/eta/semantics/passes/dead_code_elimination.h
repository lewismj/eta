#pragma once

#include "eta/semantics/optimization_pass.h"
#include "eta/semantics/ir_visitor.h"
#include "eta/semantics/core_ir.h"

namespace eta::semantics::passes {

/**
 * @brief IR-level dead code elimination pass.
 *
 * Eliminates dead expressions in `begin` blocks:
 *   (begin 42 99)  → 99           (constant with no side effects dropped)
 *   (begin (+ 1 2) x)  → x       (pure expression result discarded)
 *
 * A node is considered "pure" (side-effect-free) if it is:
 *   - A Const literal
 *   - A Var reference (load does not side-effect)
 *   - A Quote
 *
 * Non-tail expressions in a Begin that are pure are removed.
 * The last expression in a Begin is always kept (it produces the value).
 *
 * Also simplifies single-element Begin blocks:
 *   (begin x) → x
 */
class DeadCodeElimination : public OptimizationPass {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "dead-code-elimination";
    }

    void run(ModuleSemantics& mod) override {
        Eliminator elim{mod};
        for (auto*& node : mod.toplevel_inits) {
            node = elim.visit(node, false);
        }
    }

private:
    struct Eliminator : IRVisitor<Eliminator> {
        ModuleSemantics& mod;

        explicit Eliminator(ModuleSemantics& m) : mod(m) {}

        core::Node* post_visit(core::Node* node, bool /*ctx*/) {
            auto* begin = std::get_if<core::Begin>(&node->data);
            if (!begin) return node;

            // Remove pure (side-effect-free) non-tail expressions
            std::vector<core::Node*> kept;
            for (std::size_t i = 0; i < begin->exprs.size(); ++i) {
                bool is_last = (i + 1 == begin->exprs.size());
                if (is_last || !is_pure(begin->exprs[i])) {
                    kept.push_back(begin->exprs[i]);
                }
            }

            // Simplify
            if (kept.empty()) {
                // Should not happen (Begin always has at least one expr),
                // but guard against it.
                return mod.emplace<core::Const>(node->span, core::Literal{std::monostate{}});
            }
            if (kept.size() == 1) {
                return kept[0];
            }

            begin->exprs = std::move(kept);
            return node;
        }

    private:
        static bool is_pure(const core::Node* node) {
            return std::holds_alternative<core::Const>(node->data)
                || std::holds_alternative<core::Var>(node->data)
                || std::holds_alternative<core::Quote>(node->data);
        }
    };
};

} // namespace eta::semantics::passes

