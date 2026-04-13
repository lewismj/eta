// clp/constraint_store.h — Per-VM constraint store with trail-based undo.
//
// Maps each logic-variable ObjectId to a Domain (ZDomain or FDDomain).
// Trail entries record the previous state so UnwindTrail can restore it.
//
// No LispVal references are stored here, so no GC integration is needed.

#pragma once

#include <optional>
#include <unordered_map>
#include <vector>
#include "domain.h"
#include "eta/runtime/memory/heap.h"

namespace eta::runtime::clp {

using ObjectId = eta::runtime::memory::heap::ObjectId;

class ConstraintStore {
public:
    // Queries

    /// Return a pointer to the domain of `id`, or nullptr if unconstrained.
    [[nodiscard]] const Domain* get_domain(ObjectId id) const noexcept {
        const auto it = domains_.find(id);
        return (it != domains_.end()) ? &it->second : nullptr;
    }

    // Mutators (trailed — always undoable via unwind)

    /// Set the domain of `id`, recording the old state on the trail for undo.
    void set_domain(ObjectId id, Domain dom) {
        const auto it = domains_.find(id);
        if (it != domains_.end()) {
            trail_.push_back({ id, it->second });
            it->second = std::move(dom);
        } else {
            trail_.push_back({ id, std::nullopt });
            domains_.emplace(id, std::move(dom));
        }
    }

    // Trail interface

    [[nodiscard]] std::size_t trail_size() const noexcept { return trail_.size(); }

    /// Undo all domain changes made since trail position `mark`.
    void unwind(std::size_t mark) {
        while (trail_.size() > mark) {
            auto& e = trail_.back();
            if (e.prev.has_value()) {
                domains_[e.id] = std::move(*e.prev);
            } else {
                domains_.erase(e.id);
            }
            trail_.pop_back();
        }
    }

    /// Reset to empty state (called on VM destruction / re-initialisation).
    void clear() noexcept {
        domains_.clear();
        trail_.clear();
    }

private:
    struct TrailEntry {
        ObjectId              id;
        std::optional<Domain> prev;   ///< nullopt → variable had no domain before
    };

    std::unordered_map<ObjectId, Domain> domains_;
    std::vector<TrailEntry>              trail_;
};

} // namespace eta::runtime::clp

