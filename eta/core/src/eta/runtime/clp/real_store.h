#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "eta/runtime/clp/fm.h"
#include "eta/runtime/clp/linear.h"
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
 * @brief Per-VM append-only log of posted CLP(R) constraints.
 *
 * Stage 6.4 keeps the posting history as an append log so trail unwind can
 * restore an earlier state by truncating to a prior size snapshot.
 */
class RealStore {
public:
    /// Current number of posted constraints in the append log.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Drop all posted constraints.
    void clear() noexcept { entries_.clear(); }

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

    /// Read-only access to the append log.
    [[nodiscard]] const std::vector<RealConstraintEntry>& entries() const noexcept { return entries_; }

private:
    /// Extract participating logic-variable ids from a canonical linear expression.
    [[nodiscard]] static std::vector<memory::heap::ObjectId> collect_vars(const LinearExpr& expr);

    std::vector<RealConstraintEntry> entries_;
};

} // namespace eta::runtime::clp
