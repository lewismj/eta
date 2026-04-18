#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "eta/runtime/clp/domain.h"
#include "eta/runtime/clp/linear.h"

namespace eta::runtime::clp {

/**
 * @brief Fourier-Motzkin solver status.
 */
enum class FMStatus : std::uint8_t {
    Feasible,
    Infeasible,
    CapExceeded,
};

/**
 * @brief Fourier-Motzkin runtime configuration.
 */
struct FMConfig {
    std::size_t row_cap = 4096;
    double eps = 1e-12;
};

/**
 * @brief Input system for Fourier-Motzkin elimination.
 *
 * Rows are in canonical form:
 *   sum_i (a_i * x_i) + c <= 0
 *
 * Equalities are represented in @ref eq and expanded to paired
 * inequalities internally.
 */
struct FMSystem {
    std::vector<LinearExpr> leq;
    std::vector<LinearExpr> eq;
};

/**
 * @brief Feasibility result for an FM system.
 */
struct FMFeasibilityResult {
    FMStatus status = FMStatus::Feasible;
};

/**
 * @brief Bounds result for a single variable projection.
 */
struct FMBoundsResult {
    FMStatus status = FMStatus::Feasible;
    std::optional<RDomain> bounds;
};

/**
 * @brief Check whether an FM system is feasible.
 */
FMFeasibilityResult fm_feasible(const FMSystem& sys, FMConfig cfg = {});

/**
 * @brief Compute projected bounds for one variable.
 */
FMBoundsResult fm_bounds_for(const FMSystem& sys,
                             memory::heap::ObjectId var_id,
                             FMConfig cfg = {});

} // namespace eta::runtime::clp
