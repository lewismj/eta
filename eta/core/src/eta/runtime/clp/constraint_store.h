// clp/constraint_store.h — Per-VM constraint store.
//
// Maps each logic-variable ObjectId to a Domain (ZDomain or FDDomain).
//
// Phase 1 follow-up: the store no longer keeps a private undo trail.
// All domain mutations are trailed by the VM through a unified
// `TrailEntry::Kind::Domain` entry on `VM::trail_stack_`, which is the
// single source of truth for backtracking.  The store therefore offers
// only direct mutators; trailing is the caller's responsibility (use
// `VM::trail_set_domain` / `VM::trail_erase_domain`).
//
/// No LispVal references are stored here, so no GC integration is needed.

#pragma once

#include <optional>
#include <unordered_map>
#include "domain.h"
#include "eta/runtime/memory/heap.h"

namespace eta::runtime::clp {

using ObjectId = eta::runtime::memory::heap::ObjectId;

class ConstraintStore {
public:
    /// Return a pointer to the domain of `id`, or nullptr if unconstrained.
    [[nodiscard]] const Domain* get_domain(ObjectId id) const noexcept {
        const auto it = domains_.find(id);
        return (it != domains_.end()) ? &it->second : nullptr;
    }

    /// Install (or replace) the domain of `id`.  NOT trailed — use the VM
    /// helper `trail_set_domain` for backtrackable writes.
    void set_domain_no_trail(ObjectId id, Domain dom) {
        domains_[id] = std::move(dom);
    }

    /// Remove the domain of `id`, if any.  NOT trailed — see above.
    void erase_domain_no_trail(ObjectId id) noexcept {
        domains_.erase(id);
    }

    /// Reset to empty state (called on VM destruction / re-initialisation).
    void clear() noexcept {
        domains_.clear();
    }

private:
    std::unordered_map<ObjectId, Domain> domains_;
};

} // namespace eta::runtime::clp

