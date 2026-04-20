#include "eta/runtime/clp/qp_solver.h"

#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "eta/runtime/clp/linear.h"

namespace eta::runtime::clp {
namespace {

using memory::heap::ObjectId;

QPSolveErrorInfo make_error(std::string tag,
                            std::string message,
                            std::vector<ObjectId> offending_vars = {}) {
    return QPSolveErrorInfo{
        .tag = std::move(tag),
        .message = std::move(message),
        .offending_vars = std::move(offending_vars),
    };
}

std::expected<void, QPSolveErrorInfo>
validate_config(const QPSolverConfig& cfg) {
    if (!std::isfinite(cfg.feasibility_tol) || cfg.feasibility_tol <= 0.0) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "invalid feasibility tolerance"));
    }
    if (!std::isfinite(cfg.optimality_tol) || cfg.optimality_tol <= 0.0) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "invalid optimality tolerance"));
    }
    if (!std::isfinite(cfg.step_tol) || cfg.step_tol <= 0.0) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "invalid step tolerance"));
    }
    if (!std::isfinite(cfg.regularization) || cfg.regularization < 0.0) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "invalid Hessian regularization"));
    }
    if (!std::isfinite(cfg.nullspace_tol) || cfg.nullspace_tol < 0.0) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "invalid nullspace tolerance"));
    }
    if (cfg.max_iter == 0) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "max_iter must be positive"));
    }
    return {};
}

std::expected<void, QPSolveErrorInfo>
validate_model(const QPModel& model, const std::vector<double>& initial_x) {
    const auto n = model.dim();
    if (model.vars.size() != n) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "model variable vector is inconsistent"));
    }
    if (model.c.size() != n) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "linear objective vector has inconsistent dimension"));
    }
    if (n > 0 && model.q.size() != n * n) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "Hessian matrix has inconsistent dimension"));
    }
    if (n == 0 && !model.q.empty()) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "Hessian matrix has inconsistent dimension"));
    }
    if (n > 0 && model.a_leq.size() != model.b_leq.size() * n) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "inequality matrix has inconsistent dimension"));
    }
    if (n == 0 && !model.a_leq.empty()) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "inequality matrix has inconsistent dimension"));
    }
    if (n > 0 && model.a_eq.size() != model.b_eq.size() * n) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "equality matrix has inconsistent dimension"));
    }
    if (n == 0 && !model.a_eq.empty()) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "equality matrix has inconsistent dimension"));
    }
    if (!initial_x.empty() && initial_x.size() != n) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "initial guess has inconsistent dimension"));
    }

    std::unordered_set<ObjectId> seen_vars;
    seen_vars.reserve(model.vars.size());
    for (auto id : model.vars) {
        if (!seen_vars.insert(id).second) {
            return std::unexpected(make_error(
                "clp.qp.numeric-failure",
                "duplicate variable id in model",
                {id}));
        }
    }

    auto all_finite = [](const std::vector<double>& xs) -> bool {
        for (double x : xs) {
            if (!std::isfinite(x)) return false;
        }
        return true;
    };

    if (!std::isfinite(model.k) ||
        !all_finite(model.q) ||
        !all_finite(model.c) ||
        !all_finite(model.a_leq) ||
        !all_finite(model.b_leq) ||
        !all_finite(model.a_eq) ||
        !all_finite(model.b_eq) ||
        !all_finite(initial_x)) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "model contains non-finite coefficients"));
    }

    return {};
}

Eigen::MatrixXd to_matrix(const std::vector<double>& data,
                          std::size_t rows,
                          std::size_t cols) {
    Eigen::MatrixXd out(static_cast<Eigen::Index>(rows), static_cast<Eigen::Index>(cols));
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            out(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c)) =
                data[r * cols + c];
        }
    }
    return out;
}

Eigen::VectorXd to_vector(const std::vector<double>& data) {
    Eigen::VectorXd out(static_cast<Eigen::Index>(data.size()));
    for (std::size_t i = 0; i < data.size(); ++i) {
        out(static_cast<Eigen::Index>(i)) = data[i];
    }
    return out;
}

[[nodiscard]] bool point_feasible(const Eigen::MatrixXd& a_eq,
                                  const Eigen::VectorXd& b_eq,
                                  const Eigen::MatrixXd& a_leq,
                                  const Eigen::VectorXd& b_leq,
                                  const Eigen::VectorXd& x,
                                  double tol) {
    if (a_eq.rows() > 0) {
        const Eigen::VectorXd eq_res = (a_eq * x) - b_eq;
        if (eq_res.cwiseAbs().maxCoeff() > tol) return false;
    }
    if (a_leq.rows() > 0) {
        const Eigen::VectorXd ineq_res = (a_leq * x) - b_leq;
        if (ineq_res.maxCoeff() > tol) return false;
    }
    return true;
}

struct KKTStep {
    Eigen::VectorXd p;
    Eigen::VectorXd lambda;
};

std::optional<KKTStep>
solve_kkt_step(const Eigen::MatrixXd& hessian,
               const Eigen::VectorXd& grad,
               const Eigen::MatrixXd& a_work,
               const QPSolverConfig& cfg) {
    const auto n = hessian.rows();
    const auto m = a_work.rows();

    Eigen::MatrixXd reg_hessian = hessian;
    if (cfg.regularization > 0.0) {
        reg_hessian.diagonal().array() += cfg.regularization;
    }

    if (m == 0) {
        Eigen::LDLT<Eigen::MatrixXd> ldlt(reg_hessian);
        Eigen::VectorXd p;
        if (ldlt.info() == Eigen::Success) {
            p = ldlt.solve(-grad);
        } else {
            p = reg_hessian.completeOrthogonalDecomposition().solve(-grad);
        }
        if (!p.allFinite()) return std::nullopt;
        return KKTStep{
            .p = std::move(p),
            .lambda = Eigen::VectorXd(0),
        };
    }

    Eigen::MatrixXd kkt(static_cast<Eigen::Index>(n + m), static_cast<Eigen::Index>(n + m));
    kkt.setZero();
    kkt.topLeftCorner(n, n) = reg_hessian;
    kkt.topRightCorner(n, m) = a_work.transpose();
    kkt.bottomLeftCorner(m, n) = a_work;

    Eigen::VectorXd rhs(static_cast<Eigen::Index>(n + m));
    rhs.head(n) = -grad;
    rhs.tail(m).setZero();

    auto cod = kkt.completeOrthogonalDecomposition();
    Eigen::VectorXd sol = cod.solve(rhs);
    if (!sol.allFinite()) return std::nullopt;

    const Eigen::VectorXd residual = (kkt * sol) - rhs;
    const double rhs_norm = (std::max)(1.0, rhs.norm());
    if (residual.norm() > (cfg.feasibility_tol * 10.0 * rhs_norm)) {
        return std::nullopt;
    }

    return KKTStep{
        .p = sol.head(n),
        .lambda = sol.tail(m),
    };
}

std::expected<bool, QPSolveErrorInfo>
has_unbounded_nullspace_direction(const Eigen::MatrixXd& hessian,
                                  const Eigen::VectorXd& linear,
                                  const Eigen::MatrixXd& a_eq,
                                  const Eigen::MatrixXd& a_leq,
                                  const QPSolverConfig& cfg) {
    if (hessian.rows() == 0) return false;

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig;
    eig.compute(hessian, Eigen::ComputeEigenvectors);
    if (eig.info() != Eigen::Success) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "failed to compute Hessian eigensystem"));
    }

    std::vector<Eigen::Index> null_cols;
    null_cols.reserve(static_cast<std::size_t>(eig.eigenvalues().size()));
    const auto evals = eig.eigenvalues();
    for (Eigen::Index i = 0; i < evals.size(); ++i) {
        if (evals[i] <= cfg.nullspace_tol) {
            null_cols.push_back(i);
        }
    }
    if (null_cols.empty()) return false;

    Eigen::MatrixXd basis(hessian.rows(), static_cast<Eigen::Index>(null_cols.size()));
    for (std::size_t j = 0; j < null_cols.size(); ++j) {
        basis.col(static_cast<Eigen::Index>(j)) = eig.eigenvectors().col(null_cols[j]);
    }

    const Eigen::VectorXd projected_linear = basis.transpose() * linear;
    if (projected_linear.cwiseAbs().maxCoeff() <= cfg.nullspace_tol) {
        return false;
    }

    Simplex simplex;
    constexpr std::uint64_t kBaseVar = 1'000'000ULL;
    const auto max_obj_id = static_cast<std::uint64_t>(std::numeric_limits<ObjectId>::max());
    if (null_cols.size() > (max_obj_id - kBaseVar)) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "nullspace basis is too large for internal LP ids"));
    }

    auto y_var = [kBaseVar](std::size_t idx) -> ObjectId {
        return static_cast<ObjectId>(kBaseVar + idx);
    };

    auto add_matrix_row = [&](const Eigen::RowVectorXd& row, bool equality) {
        LinearExpr expr;
        expr.constant = 0.0;
        for (Eigen::Index j = 0; j < row.size(); ++j) {
            const double coef = row[j];
            if (std::abs(coef) <= cfg.nullspace_tol) continue;
            expr.terms.push_back(LinearTerm{
                .var_id = y_var(static_cast<std::size_t>(j)),
                .coef = coef,
            });
        }
        expr.canonicalize();
        if (equality) {
            simplex.add_eq(std::move(expr));
        } else {
            simplex.add_leq(std::move(expr));
        }
    };

    for (Eigen::Index i = 0; i < a_leq.rows(); ++i) {
        add_matrix_row(a_leq.row(i) * basis, false);
    }
    for (Eigen::Index i = 0; i < a_eq.rows(); ++i) {
        add_matrix_row(a_eq.row(i) * basis, true);
    }

    LinearExpr objective_row;
    objective_row.constant = 1.0; // q^T d <= -1  <=> q^T d + 1 <= 0
    for (Eigen::Index j = 0; j < projected_linear.size(); ++j) {
        const double coef = projected_linear[j];
        if (std::abs(coef) <= cfg.nullspace_tol) continue;
        objective_row.terms.push_back(LinearTerm{
            .var_id = y_var(static_cast<std::size_t>(j)),
            .coef = coef,
        });
    }
    objective_row.canonicalize();
    if (objective_row.terms.empty()) return false;
    simplex.add_leq(std::move(objective_row));

    const auto status = simplex.check(cfg.feasibility_tol);
    switch (status) {
        case SimplexStatus::Feasible:
        case SimplexStatus::Unbounded:
            return true;
        case SimplexStatus::Infeasible:
            return false;
        case SimplexStatus::NumericFailure:
            return std::unexpected(make_error(
                "clp.qp.numeric-failure",
                "failed to check nullspace recession direction"));
    }
    return false;
}

double evaluate_objective(const Eigen::MatrixXd& q,
                          const Eigen::VectorXd& c,
                          double k,
                          const Eigen::VectorXd& x) {
    return (0.5 * x.dot(q * x)) + c.dot(x) + k;
}

} // namespace

std::expected<QPSolveResult, QPSolveErrorInfo>
solve_quadratic_program(const QPModel& model,
                        SimplexDirection direction,
                        std::vector<double> initial_x,
                        QPSolverConfig cfg) {
    auto cfg_ok = validate_config(cfg);
    if (!cfg_ok) return std::unexpected(cfg_ok.error());

    auto model_ok = validate_model(model, initial_x);
    if (!model_ok) return std::unexpected(model_ok.error());

#ifndef ETA_CLP_QP_BACKEND
    (void)direction;
    (void)cfg;
    return std::unexpected(make_error(
        "clp.qp.backend-unavailable",
        "quadratic objective requires ETA_CLP_QP_BACKEND"));
#else
    const auto n = model.dim();
    const auto me = model.eq_rows();
    const auto mi = model.leq_rows();

    if (n == 0) {
        for (double b : model.b_eq) {
            if (std::abs(b) > cfg.feasibility_tol) {
                return QPSolveResult{
                    .status = QPSolveResult::Status::Infeasible,
                };
            }
        }
        for (double b : model.b_leq) {
            if (0.0 > (b + cfg.feasibility_tol)) {
                return QPSolveResult{
                    .status = QPSolveResult::Status::Infeasible,
                };
            }
        }
        return QPSolveResult{
            .status = QPSolveResult::Status::Optimal,
            .value = model.k,
            .witness = {},
        };
    }

    const Eigen::MatrixXd q_orig = to_matrix(model.q, n, n);
    const Eigen::VectorXd c_orig = to_vector(model.c);
    Eigen::MatrixXd q_obj = 0.5 * (q_orig + q_orig.transpose());
    if (!q_obj.allFinite()) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "objective Hessian contains non-finite entries"));
    }
    if (!c_orig.allFinite()) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "objective linear vector contains non-finite entries"));
    }

    const Eigen::MatrixXd a_eq = to_matrix(model.a_eq, me, n);
    const Eigen::VectorXd b_eq = to_vector(model.b_eq);
    const Eigen::MatrixXd a_leq = to_matrix(model.a_leq, mi, n);
    const Eigen::VectorXd b_leq = to_vector(model.b_leq);

    const double sign = (direction == SimplexDirection::Minimize) ? 1.0 : -1.0;
    Eigen::MatrixXd hessian = q_obj * sign;
    Eigen::VectorXd linear = c_orig * sign;

    Eigen::VectorXd x(static_cast<Eigen::Index>(n));
    if (initial_x.empty()) {
        x.setZero();
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            x[static_cast<Eigen::Index>(i)] = initial_x[i];
        }
    }

    if (!point_feasible(a_eq, b_eq, a_leq, b_leq, x, cfg.feasibility_tol)) {
        return QPSolveResult{
            .status = QPSolveResult::Status::Infeasible,
        };
    }

    auto unbounded = has_unbounded_nullspace_direction(
        hessian, linear, a_eq, a_leq, cfg);
    if (!unbounded) return std::unexpected(unbounded.error());
    if (*unbounded) {
        return QPSolveResult{
            .status = QPSolveResult::Status::Unbounded,
        };
    }

    std::vector<Eigen::Index> active_ineq;
    active_ineq.reserve(mi);
    for (std::size_t i = 0; i < mi; ++i) {
        const double slack =
            b_leq[static_cast<Eigen::Index>(i)] -
            a_leq.row(static_cast<Eigen::Index>(i)).dot(x);
        if (slack <= cfg.feasibility_tol) {
            active_ineq.push_back(static_cast<Eigen::Index>(i));
        }
    }

    bool solved = false;
    for (std::size_t iter = 0; iter < cfg.max_iter; ++iter) {
        const Eigen::VectorXd grad = (hessian * x) + linear;

        Eigen::MatrixXd a_work(
            static_cast<Eigen::Index>(me + active_ineq.size()),
            static_cast<Eigen::Index>(n));
        if (me > 0) {
            a_work.topRows(static_cast<Eigen::Index>(me)) = a_eq;
        }
        for (std::size_t i = 0; i < active_ineq.size(); ++i) {
            a_work.row(static_cast<Eigen::Index>(me + i)) =
                a_leq.row(active_ineq[i]);
        }

        auto step = solve_kkt_step(hessian, grad, a_work, cfg);
        if (!step.has_value()) {
            return std::unexpected(make_error(
                "clp.qp.numeric-failure",
                "failed to solve KKT system"));
        }

        const Eigen::VectorXd& p = step->p;
        if (!p.allFinite()) {
            return std::unexpected(make_error(
                "clp.qp.numeric-failure",
                "search direction is non-finite"));
        }

        if (p.norm() <= cfg.step_tol) {
            std::optional<std::size_t> drop_pos;
            double most_negative = -cfg.optimality_tol;
            for (std::size_t i = 0; i < active_ineq.size(); ++i) {
                const Eigen::Index lambda_idx = static_cast<Eigen::Index>(me + i);
                if (lambda_idx >= step->lambda.size()) break;
                const double lambda = step->lambda[lambda_idx];
                if (lambda < most_negative) {
                    most_negative = lambda;
                    drop_pos = i;
                }
            }

            if (drop_pos.has_value()) {
                active_ineq.erase(active_ineq.begin() +
                                  static_cast<std::ptrdiff_t>(*drop_pos));
                continue;
            }

            solved = true;
            break;
        }

        double alpha = 1.0;
        std::optional<Eigen::Index> blocker;
        for (std::size_t i = 0; i < mi; ++i) {
            const Eigen::Index idx = static_cast<Eigen::Index>(i);
            if (std::find(active_ineq.begin(), active_ineq.end(), idx) != active_ineq.end()) {
                continue;
            }
            const double aip = a_leq.row(idx).dot(p);
            if (aip <= cfg.step_tol) continue;
            const double slack = b_leq[idx] - a_leq.row(idx).dot(x);
            const double candidate = slack / aip;
            if (candidate < alpha) {
                alpha = candidate;
                blocker = idx;
            }
        }

        if (!std::isfinite(alpha) || alpha < 0.0) {
            return std::unexpected(make_error(
                "clp.qp.numeric-failure",
                "invalid line-search step"));
        }

        x = x + (alpha * p);
        if (!x.allFinite()) {
            return std::unexpected(make_error(
                "clp.qp.numeric-failure",
                "iterate became non-finite"));
        }

        if (blocker.has_value() && alpha < (1.0 - cfg.step_tol)) {
            active_ineq.push_back(*blocker);
            std::sort(active_ineq.begin(), active_ineq.end());
            active_ineq.erase(std::unique(active_ineq.begin(), active_ineq.end()),
                              active_ineq.end());
        }

        if (!point_feasible(a_eq, b_eq, a_leq, b_leq, x, cfg.feasibility_tol * 10.0)) {
            return std::unexpected(make_error(
                "clp.qp.numeric-failure",
                "active-set iterate left feasible region"));
        }
    }

    if (!solved) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "active-set iteration limit exceeded"));
    }

    if (!point_feasible(a_eq, b_eq, a_leq, b_leq, x, cfg.feasibility_tol * 10.0)) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "final iterate is not feasible"));
    }

    const double objective_value = evaluate_objective(q_obj, c_orig, model.k, x);
    if (!std::isfinite(objective_value)) {
        return std::unexpected(make_error(
            "clp.qp.numeric-failure",
            "objective value is non-finite"));
    }

    QPSolveResult out;
    out.status = QPSolveResult::Status::Optimal;
    out.value = objective_value;
    out.witness.reserve(model.vars.size());
    for (std::size_t i = 0; i < model.vars.size(); ++i) {
        double v = x[static_cast<Eigen::Index>(i)];
        if (std::abs(v) <= cfg.feasibility_tol) v = 0.0;
        out.witness.emplace_back(model.vars[i], v);
    }
    return out;
#endif
}

} // namespace eta::runtime::clp
