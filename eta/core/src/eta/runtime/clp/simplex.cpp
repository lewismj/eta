#include "eta/runtime/clp/simplex.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace eta::runtime::clp {
namespace {

using ObjectId = memory::heap::ObjectId;

/**
 * @brief Internal tableau simplex solver for `A x <= b`, `x >= 0`.
 */
class TableauLP {
public:
    enum class Status : std::uint8_t {
        Optimal,
        Infeasible,
        Unbounded,
        NumericFailure,
    };

    struct Result {
        Status status = Status::NumericFailure;
        double value = 0.0;
        std::vector<double> solution;
    };

    TableauLP(const std::vector<std::vector<double>>& A,
              const std::vector<double>& b,
              const std::vector<double>& c,
              double eps)
        : m_(b.size()),
          n_(c.size()),
          eps_(eps),
          B_(m_),
          N_(n_ + 1),
          D_(m_ + 2, std::vector<double>(n_ + 2, 0.0)) {
        for (std::size_t i = 0; i < m_; ++i) {
            for (std::size_t j = 0; j < n_; ++j) {
                D_[i][j] = A[i][j];
            }
        }
        for (std::size_t i = 0; i < m_; ++i) {
            B_[i] = static_cast<int>(n_ + i);
            D_[i][n_] = -1.0;
            D_[i][n_ + 1] = b[i];
        }
        for (std::size_t j = 0; j < n_; ++j) {
            N_[j] = static_cast<int>(j);
            D_[m_][j] = -c[j];
        }
        N_[n_] = -1; ///< artificial variable id
        D_[m_ + 1][n_] = 1.0;
    }

    [[nodiscard]] Result solve() {
        if (!std::isfinite(eps_) || eps_ <= 0.0) {
            return Result{.status = Status::NumericFailure};
        }

        std::size_t pivot_row = 0;
        for (std::size_t i = 1; i < m_; ++i) {
            if (D_[i][n_ + 1] < D_[pivot_row][n_ + 1] - eps_ ||
                (std::abs(D_[i][n_ + 1] - D_[pivot_row][n_ + 1]) <= eps_ &&
                 B_[i] < B_[pivot_row])) {
                pivot_row = i;
            }
        }

        if (m_ > 0 && D_[pivot_row][n_ + 1] < -eps_) {
            pivot(pivot_row, n_);
            if (!simplex_phase(1)) return Result{.status = Status::Infeasible};
            if (D_[m_ + 1][n_ + 1] < -eps_) {
                return Result{.status = Status::Infeasible};
            }
            if (D_[m_ + 1][n_ + 1] > eps_) {
                return Result{.status = Status::NumericFailure};
            }

            auto art_it = std::find(B_.begin(), B_.end(), -1);
            if (art_it != B_.end()) {
                const std::size_t r = static_cast<std::size_t>(std::distance(B_.begin(), art_it));
                int s = -1;
                for (std::size_t j = 0; j <= n_; ++j) {
                    if (N_[j] == -1) continue;
                    if (std::abs(D_[r][j]) <= eps_) continue;
                    if (s == -1 || N_[j] < N_[static_cast<std::size_t>(s)]) {
                        s = static_cast<int>(j);
                    }
                }
                if (s != -1) {
                    pivot(r, static_cast<std::size_t>(s));
                } else {
                    return Result{.status = Status::NumericFailure};
                }
            }
        }

        if (!simplex_phase(2)) return Result{.status = Status::Unbounded};

        std::vector<double> x(n_, 0.0);
        for (std::size_t i = 0; i < m_; ++i) {
            if (B_[i] >= 0 && B_[i] < static_cast<int>(n_)) {
                x[static_cast<std::size_t>(B_[i])] = D_[i][n_ + 1];
            }
        }
        return Result{
            .status = Status::Optimal,
            .value = D_[m_][n_ + 1],
            .solution = std::move(x),
        };
    }

private:
    /**
     * @brief Bland pivot step over one entering column and leaving row.
     */
    void pivot(std::size_t r, std::size_t s) {
        const double inv = 1.0 / D_[r][s];

        for (std::size_t i = 0; i <= m_ + 1; ++i) {
            if (i == r) continue;
            for (std::size_t j = 0; j <= n_ + 1; ++j) {
                if (j == s) continue;
                D_[i][j] -= D_[r][j] * D_[i][s] * inv;
            }
        }
        for (std::size_t j = 0; j <= n_ + 1; ++j) {
            if (j != s) D_[r][j] *= inv;
        }
        for (std::size_t i = 0; i <= m_ + 1; ++i) {
            if (i != r) D_[i][s] *= -inv;
        }
        D_[r][s] = inv;
        std::swap(B_[r], N_[s]);
    }

    /**
     * @brief Run one simplex phase (`1` = feasibility, `2` = objective).
     */
    [[nodiscard]] bool simplex_phase(int phase) {
        const std::size_t obj_row = (phase == 1) ? (m_ + 1) : m_;
        for (;;) {
            int entering = -1;
            for (std::size_t j = 0; j <= n_; ++j) {
                if (phase == 2 && N_[j] == -1) continue;
                if (D_[obj_row][j] >= -eps_) continue;
                if (entering == -1 ||
                    N_[j] < N_[static_cast<std::size_t>(entering)]) {
                    entering = static_cast<int>(j);
                }
            }
            if (entering == -1) return true;

            int leaving = -1;
            const std::size_t s = static_cast<std::size_t>(entering);
            for (std::size_t i = 0; i < m_; ++i) {
                const double a = D_[i][s];
                if (a <= eps_) continue;
                if (leaving == -1) {
                    leaving = static_cast<int>(i);
                    continue;
                }
                const std::size_t r = static_cast<std::size_t>(leaving);
                const double ratio_i = D_[i][n_ + 1] / a;
                const double ratio_r = D_[r][n_ + 1] / D_[r][s];
                if (ratio_i < ratio_r - eps_ ||
                    (std::abs(ratio_i - ratio_r) <= eps_ && B_[i] < B_[r])) {
                    leaving = static_cast<int>(i);
                }
            }
            if (leaving == -1) return false;
            pivot(static_cast<std::size_t>(leaving), s);
        }
    }

    std::size_t m_ = 0;
    std::size_t n_ = 0;
    double eps_ = 1e-9;
    std::vector<int> B_;
    std::vector<int> N_;
    std::vector<std::vector<double>> D_;
};

[[nodiscard]] bool tighter_lower(const Bound& candidate, const Bound& current, double eps) noexcept {
    if (candidate.value > current.value + eps) return true;
    if (candidate.value < current.value - eps) return false;
    return candidate.strict && !current.strict;
}

[[nodiscard]] bool tighter_upper(const Bound& candidate, const Bound& current, double eps) noexcept {
    if (candidate.value < current.value - eps) return true;
    if (candidate.value > current.value + eps) return false;
    return candidate.strict && !current.strict;
}

[[nodiscard]] bool finite_bound(double v) noexcept {
    return std::isfinite(v);
}

} // namespace

void Simplex::clear() noexcept {
    leq_.clear();
    eq_.clear();
    bounds_.clear();
}

void Simplex::add_leq(LinearExpr expr) {
    expr.canonicalize();
    leq_.push_back(std::move(expr));
}

void Simplex::add_eq(LinearExpr expr) {
    expr.canonicalize();
    eq_.push_back(std::move(expr));
}

bool Simplex::assert_lower(ObjectId var_id, Bound bound) {
    if (std::isnan(bound.value)) return false;
    if (!std::isfinite(bound.value) && bound.value < 0.0) return false;

    constexpr double kCmpEps = 1e-12;
    auto& slot = bounds_[var_id].lo;
    if (!slot.has_value()) {
        slot = bound;
        return true;
    }
    if (tighter_lower(bound, *slot, kCmpEps)) {
        slot = bound;
        return true;
    }
    return false;
}

bool Simplex::assert_upper(ObjectId var_id, Bound bound) {
    if (std::isnan(bound.value)) return false;
    if (!std::isfinite(bound.value) && bound.value > 0.0) return false;

    constexpr double kCmpEps = 1e-12;
    auto& slot = bounds_[var_id].hi;
    if (!slot.has_value()) {
        slot = bound;
        return true;
    }
    if (tighter_upper(bound, *slot, kCmpEps)) {
        slot = bound;
        return true;
    }
    return false;
}

double Simplex::strict_adjust(double value, bool is_lower, bool strict, double eps) noexcept {
    if (!strict || !std::isfinite(value)) return value;
    return is_lower ? (value + eps) : (value - eps);
}

Simplex::PreparedProblem Simplex::prepare_problem(const LinearExpr* objective,
                                                  bool maximize_objective,
                                                  double eps) const {
    PreparedProblem out;

    for (const auto& row : leq_) {
        for (const auto& t : row.terms) out.vars.push_back(t.var_id);
    }
    for (const auto& row : eq_) {
        for (const auto& t : row.terms) out.vars.push_back(t.var_id);
    }
    for (const auto& [id, _] : bounds_) out.vars.push_back(id);
    if (objective) {
        for (const auto& t : objective->terms) out.vars.push_back(t.var_id);
    }

    std::sort(out.vars.begin(), out.vars.end());
    out.vars.erase(std::unique(out.vars.begin(), out.vars.end()), out.vars.end());

    out.index_of.reserve(out.vars.size());
    for (std::size_t i = 0; i < out.vars.size(); ++i) {
        out.index_of.emplace(out.vars[i], i);
    }

    const std::size_t n = out.vars.size() * 2;
    out.c.assign(n, 0.0);
    if (objective) {
        const double sign = maximize_objective ? 1.0 : -1.0;
        for (const auto& t : objective->terms) {
            if (!std::isfinite(t.coef)) {
                out.numeric_failure = true;
                continue;
            }
            const auto it = out.index_of.find(t.var_id);
            if (it == out.index_of.end()) {
                out.numeric_failure = true;
                continue;
            }
            const std::size_t base = 2 * it->second;
            const double coef = t.coef * sign;
            out.c[base] += coef;
            out.c[base + 1] -= coef;
        }
    }

    auto add_row = [&](const LinearExpr& expr, double sign) {
        std::vector<double> row(n, 0.0);
        double constant = expr.constant * sign;
        if (!std::isfinite(constant)) {
            out.numeric_failure = true;
            return;
        }

        for (const auto& t : expr.terms) {
            if (!std::isfinite(t.coef)) {
                out.numeric_failure = true;
                return;
            }
            const auto it = out.index_of.find(t.var_id);
            if (it == out.index_of.end()) {
                out.numeric_failure = true;
                return;
            }
            const std::size_t base = 2 * it->second;
            const double coef = t.coef * sign;
            row[base] += coef;
            row[base + 1] -= coef;
        }

        const double rhs = -constant;
        if (!std::isfinite(rhs)) {
            out.numeric_failure = true;
            return;
        }

        out.A.push_back(std::move(row));
        out.b.push_back(rhs);
    };

    for (const auto& row : leq_) add_row(row, 1.0);
    for (const auto& row : eq_) {
        add_row(row, 1.0);
        add_row(row, -1.0);
    }

    for (const auto& [id, bp] : bounds_) {
        const auto it = out.index_of.find(id);
        if (it == out.index_of.end()) continue;
        const std::size_t base = 2 * it->second;

        if (bp.lo.has_value() && bp.hi.has_value() &&
            std::isfinite(bp.lo->value) && std::isfinite(bp.hi->value)) {
            if (bp.lo->value > bp.hi->value + eps) {
                out.contradiction = true;
            } else if (std::abs(bp.lo->value - bp.hi->value) <= eps &&
                       (bp.lo->strict || bp.hi->strict)) {
                out.contradiction = true;
            }
        }

        std::optional<double> lo_val;
        std::optional<double> hi_val;

        if (bp.lo.has_value()) {
            const double lo = strict_adjust(bp.lo->value, true, bp.lo->strict, eps);
            if (std::isnan(lo)) {
                out.numeric_failure = true;
            } else if (std::isfinite(lo)) {
                std::vector<double> row(n, 0.0);
                row[base] = -1.0;
                row[base + 1] = 1.0;
                out.A.push_back(std::move(row));
                out.b.push_back(-lo);
                lo_val = lo;
            } else if (lo > 0.0) {
                out.contradiction = true;
            }
        }

        if (bp.hi.has_value()) {
            const double hi = strict_adjust(bp.hi->value, false, bp.hi->strict, eps);
            if (std::isnan(hi)) {
                out.numeric_failure = true;
            } else if (std::isfinite(hi)) {
                std::vector<double> row(n, 0.0);
                row[base] = 1.0;
                row[base + 1] = -1.0;
                out.A.push_back(std::move(row));
                out.b.push_back(hi);
                hi_val = hi;
            } else if (hi < 0.0) {
                out.contradiction = true;
            }
        }

        if (lo_val.has_value() && hi_val.has_value() &&
            *lo_val > *hi_val + (eps * 1e-6)) {
            out.contradiction = true;
        }
    }

    return out;
}

SimplexStatus Simplex::check(double eps) const {
    if (!std::isfinite(eps) || eps <= 0.0) eps = 1e-9;

    auto prep = prepare_problem(nullptr, true, eps);
    if (prep.numeric_failure) return SimplexStatus::NumericFailure;
    if (prep.contradiction) return SimplexStatus::Infeasible;

    TableauLP solver(prep.A, prep.b, prep.c, eps);
    const auto result = solver.solve();
    switch (result.status) {
        case TableauLP::Status::Optimal:
            return SimplexStatus::Feasible;
        case TableauLP::Status::Infeasible:
            return SimplexStatus::Infeasible;
        case TableauLP::Status::Unbounded:
            return SimplexStatus::Feasible;
        case TableauLP::Status::NumericFailure:
            return SimplexStatus::NumericFailure;
    }
    return SimplexStatus::NumericFailure;
}

SimplexBoundsResult Simplex::bounds_for(ObjectId var_id, double eps) const {
    if (!std::isfinite(eps) || eps <= 0.0) eps = 1e-9;

    LinearExpr objective;
    objective.terms.push_back(LinearTerm{
        .var_id = var_id,
        .coef = 1.0,
    });
    objective.constant = 0.0;
    objective.canonicalize();

    const auto max_prep = prepare_problem(&objective, true, eps);
    if (max_prep.numeric_failure) {
        return SimplexBoundsResult{.status = SimplexStatus::NumericFailure, .bounds = std::nullopt};
    }
    if (max_prep.contradiction) {
        return SimplexBoundsResult{.status = SimplexStatus::Infeasible, .bounds = std::nullopt};
    }

    TableauLP max_solver(max_prep.A, max_prep.b, max_prep.c, eps);
    const auto max_result = max_solver.solve();
    if (max_result.status == TableauLP::Status::Infeasible) {
        return SimplexBoundsResult{.status = SimplexStatus::Infeasible, .bounds = std::nullopt};
    }
    if (max_result.status == TableauLP::Status::NumericFailure) {
        return SimplexBoundsResult{.status = SimplexStatus::NumericFailure, .bounds = std::nullopt};
    }

    const auto min_prep = prepare_problem(&objective, false, eps);
    if (min_prep.numeric_failure) {
        return SimplexBoundsResult{.status = SimplexStatus::NumericFailure, .bounds = std::nullopt};
    }
    if (min_prep.contradiction) {
        return SimplexBoundsResult{.status = SimplexStatus::Infeasible, .bounds = std::nullopt};
    }

    TableauLP min_solver(min_prep.A, min_prep.b, min_prep.c, eps);
    const auto min_result = min_solver.solve();
    if (min_result.status == TableauLP::Status::Infeasible) {
        return SimplexBoundsResult{.status = SimplexStatus::Infeasible, .bounds = std::nullopt};
    }
    if (min_result.status == TableauLP::Status::NumericFailure) {
        return SimplexBoundsResult{.status = SimplexStatus::NumericFailure, .bounds = std::nullopt};
    }

    const double pos_inf = std::numeric_limits<double>::infinity();
    const double neg_inf = -std::numeric_limits<double>::infinity();

    double hi = pos_inf;
    if (max_result.status == TableauLP::Status::Optimal) hi = max_result.value;

    double lo = neg_inf;
    if (min_result.status == TableauLP::Status::Optimal) lo = -min_result.value;

    if (finite_bound(lo) && finite_bound(hi) && lo > hi + eps) {
        return SimplexBoundsResult{.status = SimplexStatus::Infeasible, .bounds = std::nullopt};
    }

    return SimplexBoundsResult{
        .status = SimplexStatus::Feasible,
        .bounds = RDomain{
            .lo = lo,
            .hi = hi,
            .lo_open = false,
            .hi_open = false,
        },
    };
}

SimplexOptResult Simplex::optimize(LinearExpr objective,
                                   SimplexDirection direction,
                                   double eps) const {
    if (!std::isfinite(eps) || eps <= 0.0) eps = 1e-9;
    objective.canonicalize();
    if (!std::isfinite(objective.constant)) {
        return SimplexOptResult{
            .status = SimplexOptResult::Status::NumericFailure,
        };
    }

    const bool maximize = (direction == SimplexDirection::Maximize);
    const auto prep = prepare_problem(&objective, maximize, eps);
    if (prep.numeric_failure) {
        return SimplexOptResult{
            .status = SimplexOptResult::Status::NumericFailure,
        };
    }
    if (prep.contradiction) {
        return SimplexOptResult{
            .status = SimplexOptResult::Status::Infeasible,
        };
    }

    TableauLP solver(prep.A, prep.b, prep.c, eps);
    const auto result = solver.solve();
    switch (result.status) {
        case TableauLP::Status::Optimal:
            break;
        case TableauLP::Status::Unbounded:
            return SimplexOptResult{
                .status = SimplexOptResult::Status::Unbounded,
            };
        case TableauLP::Status::Infeasible:
            return SimplexOptResult{
                .status = SimplexOptResult::Status::Infeasible,
            };
        case TableauLP::Status::NumericFailure:
            return SimplexOptResult{
                .status = SimplexOptResult::Status::NumericFailure,
            };
    }

    const double objective_sign = maximize ? 1.0 : -1.0;
    if (!std::isfinite(result.value)) {
        return SimplexOptResult{
            .status = SimplexOptResult::Status::NumericFailure,
        };
    }

    SimplexOptResult out;
    out.status = SimplexOptResult::Status::Optimal;
    out.value = (result.value * objective_sign) + objective.constant;
    if (!std::isfinite(out.value)) {
        return SimplexOptResult{
            .status = SimplexOptResult::Status::NumericFailure,
        };
    }
    out.witness.reserve(prep.vars.size());
    for (std::size_t i = 0; i < prep.vars.size(); ++i) {
        const std::size_t base = 2 * i;
        if (base + 1 >= result.solution.size()) {
            return SimplexOptResult{
                .status = SimplexOptResult::Status::NumericFailure,
            };
        }
        double value = result.solution[base] - result.solution[base + 1];
        if (!std::isfinite(value)) {
            return SimplexOptResult{
                .status = SimplexOptResult::Status::NumericFailure,
            };
        }
        if (std::abs(value) <= eps) value = 0.0;
        out.witness.emplace_back(prep.vars[i], value);
    }
    return out;
}

} // namespace eta::runtime::clp
