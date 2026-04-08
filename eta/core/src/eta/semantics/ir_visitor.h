#pragma once

#include "eta/semantics/core_ir.h"
#include <variant>

namespace eta::semantics {

/**
 * @brief Generic CRTP IR visitor that walks all children of a Node.
 *
 * Subclasses override pre_visit / post_visit while reusing the
 * traversal logic for every Core IR node type.
 *
 * Template parameters:
 *   Derived – CRTP derived class
 *
 * The `context` bool propagated through visit() is typically the
 * "tail position" flag but can carry any per-node boolean state.
 */
template<typename Derived>
struct IRVisitor {
    /// Called before visiting children.
    void pre_visit(core::Node*, bool) {}

    /// Called after visiting children.  May return a replacement node.
    core::Node* post_visit(core::Node* n, bool) { return n; }

    /// Walk node and all of its children depth-first.
    core::Node* visit(core::Node* node, bool context) {
        if (!node) return nullptr;

        auto* derived = static_cast<Derived*>(this);
        derived->pre_visit(node, context);

        std::visit([&](auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, core::If>) {
                val.test   = derived->visit(val.test, false);
                val.conseq = derived->visit(val.conseq, context);
                val.alt    = derived->visit(val.alt, context);
            } else if constexpr (std::is_same_v<T, core::Begin>) {
                for (std::size_t i = 0; i < val.exprs.size(); ++i) {
                    bool is_last = (i + 1 == val.exprs.size());
                    val.exprs[i] = derived->visit(val.exprs[i], context && is_last);
                }
            } else if constexpr (std::is_same_v<T, core::Lambda>) {
                val.body = derived->visit(val.body, true);
            } else if constexpr (std::is_same_v<T, core::Call>) {
                val.callee = derived->visit(val.callee, false);
                for (auto*& a : val.args) a = derived->visit(a, false);
            } else if constexpr (std::is_same_v<T, core::Set>) {
                val.value = derived->visit(val.value, false);
            } else if constexpr (std::is_same_v<T, core::DynamicWind>) {
                val.before = derived->visit(val.before, false);
                val.body   = derived->visit(val.body, false);
                val.after  = derived->visit(val.after, false);
            } else if constexpr (std::is_same_v<T, core::Values>) {
                for (auto*& e : val.exprs) e = derived->visit(e, false);
            } else if constexpr (std::is_same_v<T, core::CallWithValues>) {
                val.producer = derived->visit(val.producer, false);
                val.consumer = derived->visit(val.consumer, false);
            } else if constexpr (std::is_same_v<T, core::CallCC>) {
                val.consumer = derived->visit(val.consumer, false);
            } else if constexpr (std::is_same_v<T, core::Apply>) {
                val.proc = derived->visit(val.proc, false);
                for (auto*& a : val.args) a = derived->visit(a, false);
            } else if constexpr (std::is_same_v<T, core::Raise>) {
                val.value = derived->visit(val.value, false);
            } else if constexpr (std::is_same_v<T, core::Guard>) {
                val.body = derived->visit(val.body, context);
            } else if constexpr (std::is_same_v<T, core::Unify>) {
                val.a = derived->visit(val.a, false);
                val.b = derived->visit(val.b, false);
            } else if constexpr (std::is_same_v<T, core::DerefLogicVar>) {
                val.lvar = derived->visit(val.lvar, false);
            } else if constexpr (std::is_same_v<T, core::UnwindTrail>) {
                val.mark = derived->visit(val.mark, false);
            }
            // Const, Var, Quote, MakeLogicVar, TrailMark — leaf nodes
        }, node->data);

        return derived->post_visit(node, context);
    }
};

} // namespace eta::semantics

