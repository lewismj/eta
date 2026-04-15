#pragma once

/// @file stats_primitives.h
/// @brief Eigen-backed multi-variate statistical primitives for Eta.
///
/// Provides multi-variate OLS (via QR decomposition), covariance/correlation
/// matrices, and column-wise descriptive statistics that operate on FactTables.
///
/// Registration order matters — slot indices must match builtin_names.h
/// (stats section).  All primitives capture Heap& by reference for allocation.

#include <cmath>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/types/fact_table.h>
#include <eta/runtime/stats_math.h>
#include <eta/runtime/vm/vm.h>

namespace eta::stats_bindings {

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;
using Args = const std::vector<LispVal>&;

// ─── Helpers ─────────────────────────────────────────────────────────

inline RuntimeError stats_error(const std::string& msg) {
    return RuntimeError{VMError{RuntimeErrorCode::TypeError, msg}};
}

/// Extract a FactTable* from a LispVal, or return a type error.
inline std::expected<types::FactTable*, RuntimeError>
get_fact_table(Heap& heap, LispVal v, const char* who) {
    if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
        return std::unexpected(stats_error(std::string(who) + ": argument must be a fact-table"));
    auto* ft = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(v));
    if (!ft)
        return std::unexpected(stats_error(std::string(who) + ": argument must be a fact-table"));
    return ft;
}

/// Walk an Eta list of numbers → std::vector<double>.
inline std::expected<std::vector<double>, RuntimeError>
list_to_doubles(Heap& heap, LispVal lst, const char* who) {
    std::vector<double> result;
    LispVal cur = lst;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
            return std::unexpected(stats_error(std::string(who) + ": expected a list of numbers"));
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cons)
            return std::unexpected(stats_error(std::string(who) + ": expected a list of numbers"));
        auto nv = classify_numeric(cons->car, heap);
        if (!nv.is_valid())
            return std::unexpected(stats_error(std::string(who) + ": list element is not a number"));
        result.push_back(nv.as_double());
        cur = cons->cdr;
    }
    return result;
}

/// Walk an Eta list of integers (column indices) → std::vector<std::size_t>.
inline std::expected<std::vector<std::size_t>, RuntimeError>
list_to_col_indices(Heap& heap, LispVal lst, const char* who) {
    std::vector<std::size_t> result;
    LispVal cur = lst;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
            return std::unexpected(stats_error(std::string(who) + ": expected a list of column indices"));
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cons)
            return std::unexpected(stats_error(std::string(who) + ": expected a list of column indices"));
        auto nv = classify_numeric(cons->car, heap);
        if (!nv.is_valid() || !nv.is_fixnum() || nv.int_val < 0)
            return std::unexpected(stats_error(std::string(who) + ": column index must be a non-negative integer"));
        result.push_back(static_cast<std::size_t>(nv.int_val));
        cur = cons->cdr;
    }
    return result;
}

/// Extract a FactTable column as an Eigen::VectorXd.
inline std::expected<Eigen::VectorXd, RuntimeError>
column_to_eigen(types::FactTable& ft, std::size_t col, Heap& heap, const char* who) {
    if (col >= ft.columns.size())
        return std::unexpected(stats_error(std::string(who) + ": column index out of range"));
    Eigen::VectorXd v(static_cast<Eigen::Index>(ft.row_count));
    for (std::size_t r = 0; r < ft.row_count; ++r) {
        auto nv = classify_numeric(ft.columns[col][r], heap);
        if (!nv.is_valid())
            return std::unexpected(stats_error(std::string(who) + ": column contains non-numeric value at row " + std::to_string(r)));
        v(static_cast<Eigen::Index>(r)) = nv.as_double();
    }
    return v;
}

/// Convert an Eigen::VectorXd to an Eta list of flonums (built in reverse).
inline std::expected<LispVal, RuntimeError>
eigen_to_list(const Eigen::VectorXd& v, Heap& heap) {
    LispVal result = Nil;
    for (Eigen::Index i = v.size() - 1; i >= 0; --i) {
        auto enc = ops::encode(v(i));
        if (!enc)
            return std::unexpected(stats_error("stats: failed to encode double"));
        auto cell = make_cons(heap, *enc, result);
        if (!cell) return std::unexpected(cell.error());
        result = *cell;
    }
    return result;
}

/// Convert an Eigen::MatrixXd to a nested Eta list (list of row-lists).
inline std::expected<LispVal, RuntimeError>
matrix_to_list(const Eigen::MatrixXd& m, Heap& heap) {
    LispVal result = Nil;
    for (Eigen::Index i = m.rows() - 1; i >= 0; --i) {
        // Build row list
        LispVal row = Nil;
        for (Eigen::Index j = m.cols() - 1; j >= 0; --j) {
            auto enc = ops::encode(m(i, j));
            if (!enc) return std::unexpected(stats_error("stats: failed to encode double"));
            auto cell = make_cons(heap, *enc, row);
            if (!cell) return std::unexpected(cell.error());
            row = *cell;
        }
        auto cell = make_cons(heap, row, result);
        if (!cell) return std::unexpected(cell.error());
        result = *cell;
    }
    return result;
}

/// Build an alist pair: (symbol . value).
inline std::expected<LispVal, RuntimeError>
make_alist_pair(Heap& heap, InternTable& intern_table, const char* key, LispVal value) {
    auto sym = intern_table.intern(key);
    if (!sym) return std::unexpected(stats_error(std::string("stats: failed to intern symbol: ") + key));
    auto sym_val = ops::box(Tag::Symbol, *sym);
    return make_cons(heap, sym_val, value);
}

// ─── Registration (live implementations) ─────────────────────────────

inline void register_stats_primitives(BuiltinEnvironment& env, Heap& heap,
                                       InternTable& intern_table,
                                       [[maybe_unused]] vm::VM* vm = nullptr) {

    // ────────────────────────────────────────────────────────────────
    // stats/mean-vec : fact-table col-index-list → list
    //   Column-wise means for the given columns of a fact-table.
    // ────────────────────────────────────────────────────────────────
    env.register_builtin("stats/mean-vec", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(heap, args[0], "stats/mean-vec");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto cols = list_to_col_indices(heap, args[1], "stats/mean-vec");
        if (!cols) return std::unexpected(cols.error());
        if (cols->empty())
            return std::unexpected(stats_error("stats/mean-vec: need at least one column index"));

        auto& ft = **ft_res;
        Eigen::VectorXd means(static_cast<Eigen::Index>(cols->size()));
        for (std::size_t i = 0; i < cols->size(); ++i) {
            auto col = column_to_eigen(ft, (*cols)[i], heap, "stats/mean-vec");
            if (!col) return std::unexpected(col.error());
            means(static_cast<Eigen::Index>(i)) = col->mean();
        }
        return eigen_to_list(means, heap);
    });

    // ────────────────────────────────────────────────────────────────
    // stats/var-vec : fact-table col-index-list → list
    //   Column-wise sample variances (N-1) for the given columns.
    // ────────────────────────────────────────────────────────────────
    env.register_builtin("stats/var-vec", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(heap, args[0], "stats/var-vec");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto cols = list_to_col_indices(heap, args[1], "stats/var-vec");
        if (!cols) return std::unexpected(cols.error());
        if (cols->empty())
            return std::unexpected(stats_error("stats/var-vec: need at least one column index"));

        auto& ft = **ft_res;
        if (ft.row_count < 2)
            return std::unexpected(stats_error("stats/var-vec: need at least 2 rows"));

        Eigen::VectorXd vars(static_cast<Eigen::Index>(cols->size()));
        double n = static_cast<double>(ft.row_count);
        for (std::size_t i = 0; i < cols->size(); ++i) {
            auto col = column_to_eigen(ft, (*cols)[i], heap, "stats/var-vec");
            if (!col) return std::unexpected(col.error());
            double m = col->mean();
            double ss = (col->array() - m).square().sum();
            vars(static_cast<Eigen::Index>(i)) = ss / (n - 1.0);
        }
        return eigen_to_list(vars, heap);
    });

    // ────────────────────────────────────────────────────────────────
    // stats/cov : fact-table col-index-list → list-of-lists
    //   Sample covariance matrix (N-1) for the given columns.
    // ────────────────────────────────────────────────────────────────
    env.register_builtin("stats/cov", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(heap, args[0], "stats/cov");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto cols = list_to_col_indices(heap, args[1], "stats/cov");
        if (!cols) return std::unexpected(cols.error());
        if (cols->empty())
            return std::unexpected(stats_error("stats/cov: need at least one column index"));

        auto& ft = **ft_res;
        auto n = static_cast<Eigen::Index>(ft.row_count);
        auto p = static_cast<Eigen::Index>(cols->size());
        if (n < 2)
            return std::unexpected(stats_error("stats/cov: need at least 2 rows"));

        // Build data matrix (n × p)
        Eigen::MatrixXd X(n, p);
        for (Eigen::Index j = 0; j < p; ++j) {
            auto col = column_to_eigen(ft, (*cols)[static_cast<std::size_t>(j)], heap, "stats/cov");
            if (!col) return std::unexpected(col.error());
            X.col(j) = *col;
        }

        // Center columns
        Eigen::VectorXd means = X.colwise().mean();
        Eigen::MatrixXd centered = X.rowwise() - means.transpose();

        // Sample covariance: (X-μ)ᵀ(X-μ) / (n-1)
        Eigen::MatrixXd cov = (centered.transpose() * centered) / static_cast<double>(n - 1);
        return matrix_to_list(cov, heap);
    });

    // ────────────────────────────────────────────────────────────────
    // stats/cor : fact-table col-index-list → list-of-lists
    //   Pearson correlation matrix for the given columns.
    // ────────────────────────────────────────────────────────────────
    env.register_builtin("stats/cor", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(heap, args[0], "stats/cor");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto cols = list_to_col_indices(heap, args[1], "stats/cor");
        if (!cols) return std::unexpected(cols.error());
        if (cols->empty())
            return std::unexpected(stats_error("stats/cor: need at least one column index"));

        auto& ft = **ft_res;
        auto n = static_cast<Eigen::Index>(ft.row_count);
        auto p = static_cast<Eigen::Index>(cols->size());
        if (n < 2)
            return std::unexpected(stats_error("stats/cor: need at least 2 rows"));

        Eigen::MatrixXd X(n, p);
        for (Eigen::Index j = 0; j < p; ++j) {
            auto col = column_to_eigen(ft, (*cols)[static_cast<std::size_t>(j)], heap, "stats/cor");
            if (!col) return std::unexpected(col.error());
            X.col(j) = *col;
        }

        Eigen::VectorXd means = X.colwise().mean();
        Eigen::MatrixXd centered = X.rowwise() - means.transpose();
        Eigen::MatrixXd cov = (centered.transpose() * centered) / static_cast<double>(n - 1);

        // Normalize: cor[i][j] = cov[i][j] / (σ_i * σ_j)
        Eigen::VectorXd stddevs = cov.diagonal().array().sqrt();
        Eigen::MatrixXd cor(p, p);
        for (Eigen::Index i = 0; i < p; ++i) {
            for (Eigen::Index j = 0; j < p; ++j) {
                if (stddevs(i) == 0.0 || stddevs(j) == 0.0)
                    cor(i, j) = (i == j) ? 1.0 : 0.0;
                else
                    cor(i, j) = cov(i, j) / (stddevs(i) * stddevs(j));
            }
        }
        return matrix_to_list(cor, heap);
    });

    // ────────────────────────────────────────────────────────────────
    // stats/quantile-vec : fact-table col-index p → list
    //   p-th quantile (0 ≤ p ≤ 1) for each specified column.
    // ────────────────────────────────────────────────────────────────
    env.register_builtin("stats/quantile-vec", 3, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(heap, args[0], "stats/quantile-vec");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto cols = list_to_col_indices(heap, args[1], "stats/quantile-vec");
        if (!cols) return std::unexpected(cols.error());
        auto pv = classify_numeric(args[2], heap);
        if (!pv.is_valid())
            return std::unexpected(stats_error("stats/quantile-vec: p must be a number"));
        double p = pv.as_double();

        auto& ft = **ft_res;
        Eigen::VectorXd result(static_cast<Eigen::Index>(cols->size()));
        for (std::size_t i = 0; i < cols->size(); ++i) {
            auto col = column_to_eigen(ft, (*cols)[i], heap, "stats/quantile-vec");
            if (!col) return std::unexpected(col.error());
            // Sort and interpolate
            std::vector<double> sorted(col->data(), col->data() + col->size());
            result(static_cast<Eigen::Index>(i)) = stats::percentile(std::move(sorted), p);
        }
        return eigen_to_list(result, heap);
    });

    // ────────────────────────────────────────────────────────────────
    // stats/ols-multi : fact-table y-col x-col-index-list → alist
    //   Multi-variate OLS:  y = Xβ + ε
    //   Uses Eigen ColPivHouseholderQR for numeric stability.
    //   Returns an alist:
    //     ((coefficients . (β₀ β₁ ... βₖ))
    //      (std-errors   . (se₀ se₁ ... seₖ))
    //      (t-stats      . (t₀ t₁ ... tₖ))
    //      (p-values     . (p₀ p₁ ... pₖ))
    //      (r-squared    . R²)
    //      (adj-r-squared . R²ₐ)
    //      (residual-se  . σ̂))
    // ────────────────────────────────────────────────────────────────
    env.register_builtin("stats/ols-multi", 3, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(heap, args[0], "stats/ols-multi");
        if (!ft_res) return std::unexpected(ft_res.error());

        // y column index
        auto yv = classify_numeric(args[1], heap);
        if (!yv.is_valid() || !yv.is_fixnum() || yv.int_val < 0)
            return std::unexpected(stats_error("stats/ols-multi: y-col must be a non-negative integer"));
        auto y_col = static_cast<std::size_t>(yv.int_val);

        // x column indices
        auto x_cols = list_to_col_indices(heap, args[2], "stats/ols-multi");
        if (!x_cols) return std::unexpected(x_cols.error());
        if (x_cols->empty())
            return std::unexpected(stats_error("stats/ols-multi: need at least one predictor column"));

        auto& ft = **ft_res;
        auto n = static_cast<Eigen::Index>(ft.row_count);
        auto k = static_cast<Eigen::Index>(x_cols->size());

        if (n < k + 2)
            return std::unexpected(stats_error("stats/ols-multi: need at least (k+2) rows for k predictors"));

        // Extract y vector
        auto y_res = column_to_eigen(ft, y_col, heap, "stats/ols-multi");
        if (!y_res) return std::unexpected(y_res.error());
        Eigen::VectorXd y = *y_res;

        // Build design matrix X = [1 | x₁ | x₂ | ... | xₖ]  (n × (k+1))
        Eigen::MatrixXd X(n, k + 1);
        X.col(0).setOnes();  // intercept
        for (Eigen::Index j = 0; j < k; ++j) {
            auto col = column_to_eigen(ft, (*x_cols)[static_cast<std::size_t>(j)], heap, "stats/ols-multi");
            if (!col) return std::unexpected(col.error());
            X.col(j + 1) = *col;
        }

        // Solve via QR decomposition:  β = (XᵀX)⁻¹ Xᵀy
        Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(X);
        if (qr.rank() < k + 1)
            return std::unexpected(stats_error("stats/ols-multi: design matrix is rank-deficient"));

        Eigen::VectorXd beta = qr.solve(y);

        // Residuals
        Eigen::VectorXd residuals = y - X * beta;
        double sse = residuals.squaredNorm();
        double df_resid = static_cast<double>(n - k - 1);
        double mse = sse / df_resid;
        double residual_se = std::sqrt(mse);

        // R² and adjusted R²
        double y_mean = y.mean();
        double sst = (y.array() - y_mean).square().sum();
        double r_squared = (sst > 0.0) ? 1.0 - sse / sst : 0.0;
        double adj_r_squared = 1.0 - (1.0 - r_squared) * (static_cast<double>(n) - 1.0) / df_resid;

        // Standard errors of coefficients: sqrt(diag((XᵀX)⁻¹) * MSE)
        Eigen::MatrixXd XtX = X.transpose() * X;
        Eigen::MatrixXd XtX_inv = XtX.inverse();
        Eigen::VectorXd se_beta(k + 1);
        for (Eigen::Index i = 0; i <= k; ++i)
            se_beta(i) = std::sqrt(XtX_inv(i, i) * mse);

        // t-statistics and p-values
        Eigen::VectorXd t_stats(k + 1);
        Eigen::VectorXd p_values(k + 1);
        for (Eigen::Index i = 0; i <= k; ++i) {
            t_stats(i) = (se_beta(i) > 0.0) ? beta(i) / se_beta(i) : 0.0;
            p_values(i) = (se_beta(i) > 0.0) ? stats::t_pvalue_two_tailed(t_stats(i), df_resid) : 1.0;
        }

        // Build the result alist
        // We build from the bottom up (last pair first).
        auto coeff_list = eigen_to_list(beta, heap);
        if (!coeff_list) return std::unexpected(coeff_list.error());
        auto se_list = eigen_to_list(se_beta, heap);
        if (!se_list) return std::unexpected(se_list.error());
        auto t_list = eigen_to_list(t_stats, heap);
        if (!t_list) return std::unexpected(t_list.error());
        auto p_list = eigen_to_list(p_values, heap);
        if (!p_list) return std::unexpected(p_list.error());

        auto r2_enc = ops::encode(r_squared);
        if (!r2_enc) return std::unexpected(stats_error("stats/ols-multi: encoding error"));
        auto ar2_enc = ops::encode(adj_r_squared);
        if (!ar2_enc) return std::unexpected(stats_error("stats/ols-multi: encoding error"));
        auto rse_enc = ops::encode(residual_se);
        if (!rse_enc) return std::unexpected(stats_error("stats/ols-multi: encoding error"));

        // Build alist pairs
        auto p7 = make_alist_pair(heap, intern_table, "residual-se",   *rse_enc);
        if (!p7) return std::unexpected(p7.error());
        auto p6 = make_alist_pair(heap, intern_table, "adj-r-squared", *ar2_enc);
        if (!p6) return std::unexpected(p6.error());
        auto p5 = make_alist_pair(heap, intern_table, "r-squared",     *r2_enc);
        if (!p5) return std::unexpected(p5.error());
        auto p4 = make_alist_pair(heap, intern_table, "p-values",      *p_list);
        if (!p4) return std::unexpected(p4.error());
        auto p3 = make_alist_pair(heap, intern_table, "t-stats",       *t_list);
        if (!p3) return std::unexpected(p3.error());
        auto p2 = make_alist_pair(heap, intern_table, "std-errors",    *se_list);
        if (!p2) return std::unexpected(p2.error());
        auto p1 = make_alist_pair(heap, intern_table, "coefficients",  *coeff_list);
        if (!p1) return std::unexpected(p1.error());

        // Chain into alist: ((coefficients . ...) (std-errors . ...) ... (residual-se . ...))
        auto l7 = make_cons(heap, *p7, Nil);    if (!l7) return std::unexpected(l7.error());
        auto l6 = make_cons(heap, *p6, *l7);    if (!l6) return std::unexpected(l6.error());
        auto l5 = make_cons(heap, *p5, *l6);    if (!l5) return std::unexpected(l5.error());
        auto l4 = make_cons(heap, *p4, *l5);    if (!l4) return std::unexpected(l4.error());
        auto l3 = make_cons(heap, *p3, *l4);    if (!l3) return std::unexpected(l3.error());
        auto l2 = make_cons(heap, *p2, *l3);    if (!l2) return std::unexpected(l2.error());
        auto l1 = make_cons(heap, *p1, *l2);    if (!l1) return std::unexpected(l1.error());
        return *l1;
    });
}

/// Analysis-only name registration (no Eigen dependency required at link time).
/// Must be kept in sync with register_stats_primitives above.
inline void register_stats_builtin_names(BuiltinEnvironment& env) {
    auto r = [&env](const char* name, uint32_t arity, bool has_rest) {
        env.register_builtin(name, arity, has_rest, PrimitiveFunc{});
    };

    r("stats/mean-vec",     2, false);
    r("stats/var-vec",      2, false);
    r("stats/cov",          2, false);
    r("stats/cor",          2, false);
    r("stats/quantile-vec", 3, false);
    r("stats/ols-multi",    3, false);
}

} // namespace eta::stats_bindings

