#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

#include "eta/runtime/clp/linear.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"

namespace eta::runtime::clp {

/**
 * @brief Canonical quadratic term `coef * x_i * x_j`.
 *
 * Canonical ordering requires `var_i <= var_j`.
 */
struct QuadraticTerm {
    memory::heap::ObjectId var_i = 0;
    memory::heap::ObjectId var_j = 0;
    double coef = 0.0;

    [[nodiscard]] bool operator<(const QuadraticTerm& other) const noexcept {
        if (var_i != other.var_i) return var_i < other.var_i;
        return var_j < other.var_j;
    }
};

/**
 * @brief Quadratic objective expression in canonical polynomial form.
 */
struct QuadraticExpr {
    std::vector<QuadraticTerm> quadratic_terms;
    std::vector<LinearTerm> linear_terms;
    double constant = 0.0;

    /// Normalize term ordering, merge duplicates, and prune zeros.
    void canonicalize();
};

/**
 * @brief Diagnostic payload for quadratic-objective linearization failures.
 */
struct QuadraticLinearizeErrorInfo {
    std::string tag;
    std::string message;
    std::vector<memory::heap::ObjectId> offending_vars;
};

/**
 * @brief Dense quadratic objective model `0.5*x^T*Q*x + c^T*x + k`.
 *
 * Variable ordering is deterministic and stored in @ref vars.
 * `q` is row-major with shape `dim() * dim()`.
 */
struct QuadraticObjectiveMatrix {
    std::vector<memory::heap::ObjectId> vars;
    std::vector<double> q;
    std::vector<double> c;
    double k = 0.0;

    [[nodiscard]] std::size_t dim() const noexcept {
        return vars.size();
    }

    [[nodiscard]] double q_at(std::size_t row, std::size_t col) const noexcept {
        const auto n = dim();
        return q[row * n + col];
    }
};

/**
 * @brief Tolerance policy for PSD validation.
 *
 * A Hessian with minimum eigenvalue `lambda_min` is accepted when
 * `lambda_min >= -tol`, where `tol = abs_tol + rel_tol * scale`, and
 * `scale` is the max absolute Hessian entry.
 *
 * The jitter budget (`jitter_abs + jitter_rel * scale`) allows a small
 * additional diagonal shift before reporting a non-convex objective.
 */
struct QuadraticConvexityConfig {
    double abs_tol = 1e-12;
    double rel_tol = 1e-10;
    double jitter_abs = 1e-12;
    double jitter_rel = 1e-10;
};

/**
 * @brief Diagnostic payload for QP objective matrix/convexity failures.
 */
struct QuadraticModelErrorInfo {
    std::string tag;
    std::string message;
    std::vector<memory::heap::ObjectId> offending_vars;
};

/**
 * @brief Linearize an arithmetic term into a quadratic objective expression.
 *
 * Accepted forms include constants, variables, addition/subtraction, scalar
 * multiplication, and products of affine terms. Any operation that exceeds
 * quadratic degree is rejected with a structured error.
 */
std::expected<QuadraticExpr, QuadraticLinearizeErrorInfo>
linearize_quadratic_objective(nanbox::LispVal term,
                              memory::heap::Heap& heap,
                              memory::intern::InternTable& intern_table);

/**
 * @brief Materialize a canonical quadratic expression into dense `(Q, c, k)`.
 *
 * The source polynomial `sum(a_ij * x_i * x_j) + sum(b_i * x_i) + constant`
 * is mapped to `0.5*x^T*Q*x + c^T*x + k`.
 */
std::expected<QuadraticObjectiveMatrix, QuadraticModelErrorInfo>
materialize_quadratic_objective_matrix(const QuadraticExpr& expr);

/**
 * @brief Validate convexity of the effective Hessian used for minimization.
 *
 * @param objective Materialized objective model.
 * @param hessian_sign Multiplier applied to `Q` before PSD testing.
 *        Use `+1` for minimization of `f(x)` and `-1` for maximization
 *        (equivalent minimization of `-f(x)`).
 * @param cfg Numeric tolerance and jitter policy.
 */
std::expected<void, QuadraticModelErrorInfo>
check_quadratic_convexity(const QuadraticObjectiveMatrix& objective,
                          double hessian_sign = 1.0,
                          QuadraticConvexityConfig cfg = {});

} // namespace eta::runtime::clp
