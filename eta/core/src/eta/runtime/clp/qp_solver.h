#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "eta/runtime/clp/simplex.h"
#include "eta/runtime/memory/heap.h"

namespace eta::runtime::clp {

/**
 * @brief Dense convex QP model in row-major matrix form.
 *
 * Objective:
 *   `0.5*x^T*Q*x + c^T*x + k`
 *
 * Constraints:
 *   `A_leq*x <= b_leq`
 *   `A_eq*x  == b_eq`
 *
 * Variable ordering is deterministic and stored in @ref vars.
 */
struct QPModel {
    std::vector<memory::heap::ObjectId> vars;
    std::vector<double> q;
    std::vector<double> c;
    double k = 0.0;
    std::vector<double> a_leq;
    std::vector<double> b_leq;
    std::vector<double> a_eq;
    std::vector<double> b_eq;

    [[nodiscard]] std::size_t dim() const noexcept {
        return vars.size();
    }

    [[nodiscard]] std::size_t leq_rows() const noexcept {
        const auto n = dim();
        if (n == 0) return b_leq.size();
        return a_leq.size() / n;
    }

    [[nodiscard]] std::size_t eq_rows() const noexcept {
        const auto n = dim();
        if (n == 0) return b_eq.size();
        return a_eq.size() / n;
    }
};

/**
 * @brief Numeric controls for the active-set QP backend.
 */
struct QPSolverConfig {
    double feasibility_tol = 1e-8;
    double optimality_tol = 1e-8;
    double step_tol = 1e-10;
    double regularization = 1e-9;
    double nullspace_tol = 1e-10;
    std::size_t max_iter = 256;
};

/**
 * @brief Structured QP backend error payload.
 */
struct QPSolveErrorInfo {
    std::string tag;
    std::string message;
    std::vector<memory::heap::ObjectId> offending_vars;
};

/**
 * @brief QP optimization status and witness payload.
 */
struct QPSolveResult {
    enum class Status : std::uint8_t {
        Optimal,
        Infeasible,
        Unbounded,
    };

    Status status = Status::Infeasible;
    double value = 0.0;
    std::vector<std::pair<memory::heap::ObjectId, double>> witness;
};

/**
 * @brief Solve a convex QP objective under linear equalities/inequalities.
 *
 * When @p direction is @ref SimplexDirection::Maximize the backend solves the
 * equivalent minimization of `-f(x)` internally and returns the original
 * maximization value.
 */
std::expected<QPSolveResult, QPSolveErrorInfo>
solve_quadratic_program(const QPModel& model,
                        SimplexDirection direction,
                        std::vector<double> initial_x = {},
                        QPSolverConfig cfg = {});

} // namespace eta::runtime::clp
