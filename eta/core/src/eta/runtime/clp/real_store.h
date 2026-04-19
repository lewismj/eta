#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "eta/runtime/clp/fm.h"
#include "eta/runtime/clp/linear.h"
#include "eta/runtime/clp/simplex.h"
#include "eta/runtime/memory/heap.h"

namespace eta::runtime::clp {

/**
 * @brief Relation kind for a posted CLP(R) row.
 */
enum class RealRelation : std::uint8_t {
    Leq,  ///< `expr <= 0`
    Eq,   ///< `expr == 0`
};

/**
 * @brief One append-log entry in the CLP(R) real store.
 */
struct RealConstraintEntry {
    RealRelation                       relation{RealRelation::Leq};
    LinearExpr                         expr;
    std::vector<memory::heap::ObjectId> vars;
};

/**
 * @brief Trailed simplex-bound snapshot stored by @ref RealStore.
 */
struct SimplexBoundState {
    std::optional<Bound> lo;
    std::optional<Bound> hi;
};

/**
 * @brief Per-VM append-only log of posted CLP(R) constraints.
 *
 * Posting history is kept as an append log so trail unwind can
 * restore an earlier state by truncating to a prior size snapshot.
 */
class RealStore {
public:
    /// Current number of posted constraints in the append log.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Drop all posted constraints.
    void clear() noexcept {
        entries_.clear();
        simplex_bounds_.clear();
    }

    /// Truncate the append log to `new_size` entries (no-op if already smaller).
    void truncate(std::size_t new_size) noexcept;

    /// Append a `expr <= 0` relation row.
    void append_leq(LinearExpr expr);

    /// Append a `expr == 0` relation row.
    void append_eq(LinearExpr expr);

    /// Materialise the current append log as an FM system.
    [[nodiscard]] FMSystem to_fm_system() const;

    /// Return every participating logic-variable id (sorted, unique).
    [[nodiscard]] std::vector<memory::heap::ObjectId> participating_vars() const;

    /**
     * @brief Read one simplex bound snapshot by logic-variable id.
     */
    [[nodiscard]] const SimplexBoundState* simplex_bounds(memory::heap::ObjectId id) const noexcept;

    /**
     * @brief Set one simplex bound snapshot without adding trail entries.
     *
     * Used by VM rollback/apply helpers; call through VM trail wrappers for
     * ordinary mutating paths.
     */
    void set_simplex_bounds_no_trail(memory::heap::ObjectId id,
                                     std::optional<Bound> lo,
                                     std::optional<Bound> hi);

    /// Erase one simplex-bound snapshot without trailing.
    void erase_simplex_bounds_no_trail(memory::heap::ObjectId id) noexcept;

    /// Return all variable ids that currently have simplex-bound snapshots.
    [[nodiscard]] std::vector<memory::heap::ObjectId> simplex_bound_vars() const;

    /// Read-only access to the append log.
    [[nodiscard]] const std::vector<RealConstraintEntry>& entries() const noexcept { return entries_; }

private:
    /// Extract participating logic-variable ids from a canonical linear expression.
    [[nodiscard]] static std::vector<memory::heap::ObjectId> collect_vars(const LinearExpr& expr);

    std::vector<RealConstraintEntry> entries_;
    std::unordered_map<memory::heap::ObjectId, SimplexBoundState> simplex_bounds_;
};

} // namespace eta::runtime::clp
