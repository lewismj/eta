#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eta/runtime/clp/domain.h"
#include "eta/runtime/clp/linear.h"
#include "eta/runtime/memory/heap.h"

namespace eta::runtime::clp {

/**
 * @brief One numeric bound used by the simplex backend.
 *
 * `strict = true` means `<` / `>`; `strict = false` means `<=` / `>=`.
 */
struct Bound {
    double value = 0.0;
    bool strict = false;

    [[nodiscard]] bool operator==(const Bound& other) const noexcept = default;
};

/**
 * @brief Feasibility status returned by the simplex backend.
 */
enum class SimplexStatus : std::uint8_t {
    Feasible,
    Infeasible,
    Unbounded,
    NumericFailure,
};

/**
 * @brief Objective direction used by @ref Simplex::optimize.
 */
enum class SimplexDirection : std::uint8_t {
    Minimize,
    Maximize,
};

/**
 * @brief Bounds query result for one projected variable.
 */
struct SimplexBoundsResult {
    SimplexStatus status = SimplexStatus::Feasible;
    std::optional<RDomain> bounds;
};

/**
 * @brief Optimization result returned by @ref Simplex::optimize.
 */
struct SimplexOptResult {
    enum class Status : std::uint8_t {
        Optimal,
        Unbounded,
        Infeasible,
        NumericFailure,
    };

    Status status = Status::Infeasible;
    double value = 0.0;
    std::vector<std::pair<memory::heap::ObjectId, double>> witness;
};

/**
 * @brief Incremental LP container over linear rows and bound assertions.
 *
 * The object keeps posted `<=` / `==` rows plus optional per-variable lower
 * and upper bounds. `check()` and `bounds_for()` solve over the current state.
 */
class Simplex {
public:
    using ObjectId = memory::heap::ObjectId;

    /**
     * @brief Per-variable bound pair.
     */
    struct BoundPair {
        std::optional<Bound> lo;
        std::optional<Bound> hi;
    };

    /// Drop all rows and all asserted bounds.
    void clear() noexcept;

    /// Append one `expr <= 0` row.
    void add_leq(LinearExpr expr);

    /// Append one `expr == 0` row.
    void add_eq(LinearExpr expr);

    /**
     * @brief Assert/tighten a lower bound for one variable.
     * @return true iff the stored lower bound changed.
     */
    bool assert_lower(ObjectId var_id, Bound bound);

    /**
     * @brief Assert/tighten an upper bound for one variable.
     * @return true iff the stored upper bound changed.
     */
    bool assert_upper(ObjectId var_id, Bound bound);

    /// Read-only access to asserted bounds.
    [[nodiscard]] const std::unordered_map<ObjectId, BoundPair>& asserted_bounds() const noexcept {
        return bounds_;
    }

    /// Feasibility check over the current rows and asserted bounds.
    [[nodiscard]] SimplexStatus check(double eps = 1e-9) const;

    /// Project best lower/upper bounds for one variable.
    [[nodiscard]] SimplexBoundsResult bounds_for(ObjectId var_id, double eps = 1e-9) const;

    /**
     * @brief Optimize one linear objective over the current constraint state.
     *
     * @param objective Linear objective expression.
     * @param direction Objective direction (min/max).
     * @param eps Numeric tolerance.
     * @return Optimization status, objective value, and witness assignment.
     */
    [[nodiscard]] SimplexOptResult optimize(LinearExpr objective,
                                            SimplexDirection direction,
                                            double eps = 1e-9) const;

private:
    struct PreparedProblem {
        std::vector<ObjectId> vars;
        std::unordered_map<ObjectId, std::size_t> index_of;
        std::vector<std::vector<double>> A;
        std::vector<double> b;
        std::vector<double> c;
        bool contradiction = false;
        bool numeric_failure = false;
    };

    [[nodiscard]] PreparedProblem prepare_problem(const LinearExpr* objective,
                                                  bool maximize_objective,
                                                  double eps) const;
    [[nodiscard]] static double strict_adjust(double value,
                                              bool is_lower,
                                              bool strict,
                                              double eps) noexcept;

    std::vector<LinearExpr> leq_;
    std::vector<LinearExpr> eq_;
    std::unordered_map<ObjectId, BoundPair> bounds_;
};

} // namespace eta::runtime::clp
