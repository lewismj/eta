#include "eta/runtime/clp/fm.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace eta::runtime::clp {
namespace {

using memory::heap::ObjectId;

/**
 * @brief Return true when @p v is numerically zero under @p eps.
 */
[[nodiscard]] bool near_zero(double v, double eps) noexcept {
    return std::abs(v) <= eps;
}

/**
 * @brief Deterministic row ordering for stable oracle output.
 */
[[nodiscard]] bool row_less(const LinearExpr& a, const LinearExpr& b) noexcept {
    if (a.terms.size() != b.terms.size()) return a.terms.size() < b.terms.size();
    for (std::size_t i = 0; i < a.terms.size(); ++i) {
        if (a.terms[i].var_id != b.terms[i].var_id) {
            return a.terms[i].var_id < b.terms[i].var_id;
        }
        if (a.terms[i].coef != b.terms[i].coef) {
            return a.terms[i].coef < b.terms[i].coef;
        }
    }
    return a.constant < b.constant;
}

/**
 * @brief Canonicalise row coefficients and drop near-zero terms.
 */
void normalize_row(LinearExpr& row, double eps) {
    row.canonicalize();

    std::vector<LinearTerm> filtered;
    filtered.reserve(row.terms.size());
    for (const auto& t : row.terms) {
        if (!near_zero(t.coef, eps)) filtered.push_back(t);
    }
    row.terms = std::move(filtered);
    row.canonicalize();

    filtered.clear();
    filtered.reserve(row.terms.size());
    for (const auto& t : row.terms) {
        if (!near_zero(t.coef, eps)) filtered.push_back(t);
    }
    row.terms = std::move(filtered);
    if (near_zero(row.constant, eps)) row.constant = 0.0;
}

/**
 * @brief True when the row is a violated constant inequality.
 */
[[nodiscard]] bool is_contradiction(const LinearExpr& row, double eps) noexcept {
    return row.terms.empty() && row.constant > eps;
}

/**
 * @brief True when the row is a trivially true constant inequality.
 */
[[nodiscard]] bool is_tautology(const LinearExpr& row, double eps) noexcept {
    return row.terms.empty() && row.constant <= eps;
}

/**
 * @brief Negate every coefficient and the constant term.
 */
[[nodiscard]] LinearExpr negate_expr(LinearExpr row) {
    row.constant = -row.constant;
    for (auto& t : row.terms) t.coef = -t.coef;
    return row;
}

/**
 * @brief Coefficient lookup in a canonical row.
 */
[[nodiscard]] double coef_of(const LinearExpr& row, ObjectId var_id) noexcept {
    for (const auto& t : row.terms) {
        if (t.var_id == var_id) return t.coef;
        if (t.var_id > var_id) break;
    }
    return 0.0;
}

/**
 * @brief Linear combination used by Fourier-Motzkin elimination.
 */
[[nodiscard]] LinearExpr combine_rows(const LinearExpr& a,
                                      double a_scale,
                                      const LinearExpr& b,
                                      double b_scale) {
    LinearExpr out;
    out.constant = a.constant * a_scale + b.constant * b_scale;
    out.terms.reserve(a.terms.size() + b.terms.size());
    for (const auto& t : a.terms) {
        out.terms.push_back(LinearTerm{.var_id = t.var_id, .coef = t.coef * a_scale});
    }
    for (const auto& t : b.terms) {
        out.terms.push_back(LinearTerm{.var_id = t.var_id, .coef = t.coef * b_scale});
    }
    return out;
}

/**
 * @brief Approximate structural equality (same vars and coefficients).
 */
[[nodiscard]] bool same_structure(const LinearExpr& a,
                                  const LinearExpr& b,
                                  double eps) noexcept {
    if (a.terms.size() != b.terms.size()) return false;
    for (std::size_t i = 0; i < a.terms.size(); ++i) {
        if (a.terms[i].var_id != b.terms[i].var_id) return false;
        if (std::abs(a.terms[i].coef - b.terms[i].coef) > eps) return false;
    }
    return true;
}

/**
 * @brief Row candidate for single-variable dominance pruning.
 */
struct SingleVarPick {
    ObjectId var_id = 0;
    bool is_upper = false;
    double bound = 0.0;
    LinearExpr row;
};

/**
 * @brief Tightness comparator for single-variable upper/lower bounds.
 */
[[nodiscard]] bool is_tighter_bound(bool is_upper,
                                    double candidate,
                                    double current,
                                    double eps) noexcept {
    if (is_upper) return candidate < current - eps;
    return candidate > current + eps;
}

/**
 * @brief Merge exact duplicates and prune dominated single-variable rows.
 */
FMStatus prune_rows(std::vector<LinearExpr>& rows, const FMConfig& cfg) {
    std::vector<LinearExpr> normalized;
    normalized.reserve(rows.size());

    for (auto row : rows) {
        normalize_row(row, cfg.eps);
        if (is_contradiction(row, cfg.eps)) return FMStatus::Infeasible;
        if (is_tautology(row, cfg.eps)) continue;
        normalized.push_back(std::move(row));
    }
    if (normalized.size() > cfg.row_cap) return FMStatus::CapExceeded;

    std::vector<LinearExpr> dedup;
    dedup.reserve(normalized.size());
    for (auto& row : normalized) {
        bool merged = false;
        for (auto& existing : dedup) {
            if (!same_structure(existing, row, cfg.eps)) continue;
            existing.constant = std::max(existing.constant, row.constant);
            merged = true;
            break;
        }
        if (!merged) dedup.push_back(std::move(row));
    }
    if (dedup.size() > cfg.row_cap) return FMStatus::CapExceeded;

    std::vector<LinearExpr> kept;
    kept.reserve(dedup.size());
    std::vector<SingleVarPick> picks;
    picks.reserve(dedup.size());

    for (auto& row : dedup) {
        if (row.terms.size() != 1) {
            kept.push_back(std::move(row));
            continue;
        }
        const auto& t = row.terms[0];
        if (near_zero(t.coef, cfg.eps)) {
            if (is_contradiction(row, cfg.eps)) return FMStatus::Infeasible;
            continue;
        }

        const bool is_upper = t.coef > cfg.eps;
        const double bound = -row.constant / t.coef;

        auto it = std::find_if(
            picks.begin(), picks.end(),
            [=](const SingleVarPick& p) { return p.var_id == t.var_id && p.is_upper == is_upper; });

        if (it == picks.end()) {
            picks.push_back(SingleVarPick{
                .var_id = t.var_id,
                .is_upper = is_upper,
                .bound = bound,
                .row = std::move(row),
            });
            continue;
        }

        const bool tighter = is_tighter_bound(is_upper, bound, it->bound, cfg.eps);
        const bool tie = std::abs(bound - it->bound) <= cfg.eps;
        const bool stronger_constant = row.constant > it->row.constant + cfg.eps;
        if (tighter || (tie && stronger_constant)) {
            it->bound = bound;
            it->row = std::move(row);
        }
    }

    std::sort(
        picks.begin(), picks.end(),
        [](const SingleVarPick& a, const SingleVarPick& b) {
            if (a.var_id != b.var_id) return a.var_id < b.var_id;
            return static_cast<int>(a.is_upper) < static_cast<int>(b.is_upper);
        });

    rows = std::move(kept);
    rows.reserve(rows.size() + picks.size());
    for (auto& pick : picks) rows.push_back(std::move(pick.row));
    std::sort(rows.begin(), rows.end(), row_less);
    if (rows.size() > cfg.row_cap) return FMStatus::CapExceeded;
    return FMStatus::Feasible;
}

/**
 * @brief Expand equalities and run initial normalisation/pruning.
 */
FMStatus build_rows(const FMSystem& sys, const FMConfig& cfg, std::vector<LinearExpr>& rows) {
    rows.clear();
    rows.reserve(sys.leq.size() + 2 * sys.eq.size());

    for (const auto& e : sys.leq) {
        rows.push_back(e);
    }
    for (const auto& e : sys.eq) {
        rows.push_back(e);
        rows.push_back(negate_expr(e));
    }

    return prune_rows(rows, cfg);
}

/**
 * @brief Collect variables in deterministic ascending ObjectId order.
 */
[[nodiscard]] std::vector<ObjectId> collect_var_order(const std::vector<LinearExpr>& rows) {
    std::vector<ObjectId> ids;
    for (const auto& row : rows) {
        for (const auto& t : row.terms) ids.push_back(t.var_id);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

/**
 * @brief Eliminate one variable from a <=-system.
 */
FMStatus eliminate_var(std::vector<LinearExpr>& rows, ObjectId var_id, const FMConfig& cfg) {
    struct TaggedRow {
        const LinearExpr* row = nullptr;
        double coef = 0.0;
    };

    std::vector<TaggedRow> pos;
    std::vector<TaggedRow> neg;
    std::vector<LinearExpr> zero;
    zero.reserve(rows.size());

    for (const auto& row : rows) {
        const double c = coef_of(row, var_id);
        if (c > cfg.eps) {
            pos.push_back(TaggedRow{.row = &row, .coef = c});
        } else if (c < -cfg.eps) {
            neg.push_back(TaggedRow{.row = &row, .coef = c});
        } else {
            zero.push_back(row);
        }
    }

    if (pos.empty() || neg.empty()) {
        rows = std::move(zero);
        return prune_rows(rows, cfg);
    }

    std::vector<LinearExpr> next = std::move(zero);
    next.reserve(next.size() + pos.size() * neg.size());

    for (const auto& p : pos) {
        for (const auto& n : neg) {
            /// (-a_n) * p + a_p * n cancels var_id exactly.
            LinearExpr merged = combine_rows(*p.row, -n.coef, *n.row, p.coef);
            normalize_row(merged, cfg.eps);
            if (is_contradiction(merged, cfg.eps)) return FMStatus::Infeasible;
            if (!is_tautology(merged, cfg.eps)) {
                next.push_back(std::move(merged));
                if (next.size() > cfg.row_cap) return FMStatus::CapExceeded;
            }
        }
    }

    rows = std::move(next);
    return prune_rows(rows, cfg);
}

/**
 * @brief Solve preprocessing + elimination for feasibility only.
 */
FMStatus solve_feasible(const FMSystem& sys, const FMConfig& cfg) {
    std::vector<LinearExpr> rows;
    FMStatus status = build_rows(sys, cfg, rows);
    if (status != FMStatus::Feasible) return status;

    const std::vector<ObjectId> order = collect_var_order(rows);
    for (ObjectId var_id : order) {
        status = eliminate_var(rows, var_id, cfg);
        if (status != FMStatus::Feasible) return status;
    }
    return FMStatus::Feasible;
}

/**
 * @brief Solve preprocessing + elimination, preserving one variable.
 */
FMStatus solve_bounds(const FMSystem& sys,
                      ObjectId target_var,
                      const FMConfig& cfg,
                      std::vector<LinearExpr>& projected_rows) {
    projected_rows.clear();
    FMStatus status = build_rows(sys, cfg, projected_rows);
    if (status != FMStatus::Feasible) return status;

    const std::vector<ObjectId> order = collect_var_order(projected_rows);
    for (ObjectId var_id : order) {
        if (var_id == target_var) continue;
        status = eliminate_var(projected_rows, var_id, cfg);
        if (status != FMStatus::Feasible) return status;
    }
    return FMStatus::Feasible;
}

} // namespace

FMFeasibilityResult fm_feasible(const FMSystem& sys, FMConfig cfg) {
    return FMFeasibilityResult{
        .status = solve_feasible(sys, cfg),
    };
}

FMBoundsResult fm_bounds_for(const FMSystem& sys, ObjectId var_id, FMConfig cfg) {
    std::vector<LinearExpr> rows;
    const FMStatus status = solve_bounds(sys, var_id, cfg, rows);
    if (status != FMStatus::Feasible) {
        return FMBoundsResult{
            .status = status,
            .bounds = std::nullopt,
        };
    }

    const double neg_inf = -std::numeric_limits<double>::infinity();
    const double pos_inf = std::numeric_limits<double>::infinity();
    double lo = neg_inf;
    double hi = pos_inf;

    for (const auto& row : rows) {
        double a = 0.0;
        bool has_other_terms = false;
        for (const auto& t : row.terms) {
            if (t.var_id == var_id) {
                a += t.coef;
            } else if (!near_zero(t.coef, cfg.eps)) {
                has_other_terms = true;
            }
        }
        if (has_other_terms) return FMBoundsResult{.status = FMStatus::CapExceeded, .bounds = std::nullopt};

        if (near_zero(a, cfg.eps)) {
            if (is_contradiction(row, cfg.eps)) {
                return FMBoundsResult{.status = FMStatus::Infeasible, .bounds = std::nullopt};
            }
            continue;
        }

        const double bound = -row.constant / a;
        if (a > cfg.eps) {
            hi = std::min(hi, bound);
        } else {
            lo = std::max(lo, bound);
        }
    }

    if (lo > hi + cfg.eps) {
        return FMBoundsResult{
            .status = FMStatus::Infeasible,
            .bounds = std::nullopt,
        };
    }

    return FMBoundsResult{
        .status = FMStatus::Feasible,
        .bounds = RDomain{
            .lo = lo,
            .hi = hi,
            .lo_open = false,
            .hi_open = false,
        },
    };
}

} // namespace eta::runtime::clp
