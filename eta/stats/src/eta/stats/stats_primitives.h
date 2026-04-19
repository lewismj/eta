#pragma once

/**
 * @file stats_primitives.h
 * @brief Eigen-backed multi-variate statistical primitives for Eta.
 *
 * Provides polymorphic multi-variate statistics that operate on any
 * combination of numeric sequences (lists, vectors, fact-table columns):
 *   - Column-wise means and variances
 *   - Covariance and correlation matrices
 *   - Column-wise quantiles
 *   - Multi-variate OLS regression (via QR decomposition)
 *
 * Every primitive accepts either:
 *   (a) a list of numeric sequences (each a list, vector, etc.), or
 *   (b) a fact-table + a list of column indices.
 *
 * (stats section).  All primitives capture Heap& by reference for allocation.
 */

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
#include <eta/runtime/stats_extract.h>
#include <eta/runtime/vm/vm.h>

namespace eta::stats_bindings {

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;
using Args = const std::vector<LispVal>&;


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

/**
 * Extract a FactTable column as an Eigen::VectorXd.
 * Delegates to the shared stats::column_to_eigen from stats_extract.h.
 */
inline std::expected<Eigen::VectorXd, RuntimeError>
column_to_eigen(types::FactTable& ft, std::size_t col, Heap& heap, const char* who) {
    return stats::column_to_eigen(ft, col, heap, who);
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

/**
 * Polymorphic column extraction.
 *   1-arg form: args[0] is a list of numeric sequences.
 *   2-arg form: args[0] is a fact-table, args[1] is a list of column indices.
 */
inline std::expected<std::vector<Eigen::VectorXd>, RuntimeError>
extract_columns(Heap& heap, Args args, const char* who) {
    std::vector<Eigen::VectorXd> columns;

    if (args.size() == 2) {
        auto ft_res = get_fact_table(heap, args[0], who);
        if (!ft_res)
            return std::unexpected(stats_error(
                std::string(who) + ": with two arguments, first must be a fact-table"));
        auto cols = list_to_col_indices(heap, args[1], who);
        if (!cols) return std::unexpected(cols.error());
        if (cols->empty())
            return std::unexpected(stats_error(std::string(who) + ": need at least one column index"));
        for (auto ci : *cols) {
            auto col = column_to_eigen(**ft_res, ci, heap, who);
            if (!col) return std::unexpected(col.error());
            columns.push_back(std::move(*col));
        }
        return columns;
    }

    LispVal cur = args[0];
    if (cur == Nil)
        return std::unexpected(stats_error(std::string(who) + ": need at least one data sequence"));
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
            return std::unexpected(stats_error(std::string(who) + ": expected a list of numeric sequences"));
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cons)
            return std::unexpected(stats_error(std::string(who) + ": expected a list of numeric sequences"));
        auto vec = stats::to_eigen(heap, cons->car, who);
        if (!vec) return std::unexpected(vec.error());
        columns.push_back(std::move(*vec));
        cur = cons->cdr;
    }
    return columns;
}


inline void register_stats_primitives(BuiltinEnvironment& env, Heap& heap,
                                       InternTable& intern_table,
                                       [[maybe_unused]] vm::VM* vm = nullptr) {

    ///   Column-wise means.
    env.register_builtin("%stats-mean-vec", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto cols = extract_columns(heap, args, "%stats-mean-vec");
        if (!cols) return std::unexpected(cols.error());

        Eigen::VectorXd means(static_cast<Eigen::Index>(cols->size()));
        for (std::size_t i = 0; i < cols->size(); ++i)
            means(static_cast<Eigen::Index>(i)) = (*cols)[i].mean();
        return eigen_to_list(means, heap);
    });

    ///   Column-wise sample variances (N-1).
    env.register_builtin("%stats-var-vec", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto cols = extract_columns(heap, args, "%stats-var-vec");
        if (!cols) return std::unexpected(cols.error());

        Eigen::VectorXd vars(static_cast<Eigen::Index>(cols->size()));
        for (std::size_t i = 0; i < cols->size(); ++i) {
            if ((*cols)[i].size() < 2)
                return std::unexpected(stats_error("%stats-var-vec: each sequence needs at least 2 elements"));
            vars(static_cast<Eigen::Index>(i)) = stats::variance((*cols)[i]);
        }
        return eigen_to_list(vars, heap);
    });

    ///   Sample covariance matrix (N-1).
    env.register_builtin("%stats-cov-matrix", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto cols = extract_columns(heap, args, "%stats-cov-matrix");
        if (!cols) return std::unexpected(cols.error());

        auto p = static_cast<Eigen::Index>(cols->size());
        if (p == 0)
            return std::unexpected(stats_error("%stats-cov-matrix: need at least one column"));
        auto n = (*cols)[0].size();
        for (auto& c : *cols)
            if (c.size() != n)
                return std::unexpected(stats_error("%stats-cov-matrix: all sequences must have the same length"));
        if (n < 2)
            return std::unexpected(stats_error("%stats-cov-matrix: need at least 2 observations"));

        Eigen::MatrixXd X(n, p);
        for (Eigen::Index j = 0; j < p; ++j)
            X.col(j) = (*cols)[static_cast<std::size_t>(j)];

        Eigen::VectorXd means = X.colwise().mean();
        Eigen::MatrixXd centered = X.rowwise() - means.transpose();
        Eigen::MatrixXd cov = (centered.transpose() * centered) / static_cast<double>(n - 1);
        return matrix_to_list(cov, heap);
    });

    ///   Pearson correlation matrix.
    env.register_builtin("%stats-cor-matrix", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto cols = extract_columns(heap, args, "%stats-cor-matrix");
        if (!cols) return std::unexpected(cols.error());

        auto p = static_cast<Eigen::Index>(cols->size());
        if (p == 0)
            return std::unexpected(stats_error("%stats-cor-matrix: need at least one column"));
        auto n = (*cols)[0].size();
        for (auto& c : *cols)
            if (c.size() != n)
                return std::unexpected(stats_error("%stats-cor-matrix: all sequences must have the same length"));
        if (n < 2)
            return std::unexpected(stats_error("%stats-cor-matrix: need at least 2 observations"));

        Eigen::MatrixXd X(n, p);
        for (Eigen::Index j = 0; j < p; ++j)
            X.col(j) = (*cols)[static_cast<std::size_t>(j)];

        Eigen::VectorXd means = X.colwise().mean();
        Eigen::MatrixXd centered = X.rowwise() - means.transpose();
        Eigen::MatrixXd cov = (centered.transpose() * centered) / static_cast<double>(n - 1);

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

    env.register_builtin("%stats-quantile-vec", 2, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        double p;
        std::vector<Eigen::VectorXd> columns;

        if (args.size() == 3) {
            /// fact-table + col-indices + p
            auto ft_res = get_fact_table(heap, args[0], "%stats-quantile-vec");
            if (!ft_res) return std::unexpected(ft_res.error());
            auto col_indices = list_to_col_indices(heap, args[1], "%stats-quantile-vec");
            if (!col_indices) return std::unexpected(col_indices.error());
            if (col_indices->empty())
                return std::unexpected(stats_error("%stats-quantile-vec: need at least one column index"));
            for (auto ci : *col_indices) {
                auto col = column_to_eigen(**ft_res, ci, heap, "%stats-quantile-vec");
                if (!col) return std::unexpected(col.error());
                columns.push_back(std::move(*col));
            }
            auto pv = classify_numeric(args[2], heap);
            if (!pv.is_valid())
                return std::unexpected(stats_error("%stats-quantile-vec: p must be a number"));
            p = pv.as_double();
        } else {
            /// seq-list + p
            LispVal cur = args[0];
            if (cur == Nil)
                return std::unexpected(stats_error("%stats-quantile-vec: need at least one data sequence"));
            while (cur != Nil) {
                if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                    return std::unexpected(stats_error("%stats-quantile-vec: expected a list of numeric sequences"));
                auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
                if (!cons)
                    return std::unexpected(stats_error("%stats-quantile-vec: expected a list of numeric sequences"));
                auto vec = stats::to_eigen(heap, cons->car, "%stats-quantile-vec");
                if (!vec) return std::unexpected(vec.error());
                columns.push_back(std::move(*vec));
                cur = cons->cdr;
            }
            auto pv = classify_numeric(args[1], heap);
            if (!pv.is_valid())
                return std::unexpected(stats_error("%stats-quantile-vec: p must be a number"));
            p = pv.as_double();
        }

        Eigen::VectorXd result(static_cast<Eigen::Index>(columns.size()));
        for (std::size_t i = 0; i < columns.size(); ++i)
            result(static_cast<Eigen::Index>(i)) = stats::percentile(std::move(columns[i]), p);
        return eigen_to_list(result, heap);
    });

    /**
     *   Uses Eigen ColPivHouseholderQR for numeric stability.
     *   Returns an alist:
     */
    env.register_builtin("%stats-ols-multi", 2, true, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        Eigen::VectorXd y;
        std::vector<Eigen::VectorXd> x_cols;

        if (args.size() == 3) {
            /// fact-table + y-col + x-col-indices
            auto ft_res = get_fact_table(heap, args[0], "%stats-ols-multi");
            if (!ft_res) return std::unexpected(ft_res.error());

            auto yv = classify_numeric(args[1], heap);
            if (!yv.is_valid() || !yv.is_fixnum() || yv.int_val < 0)
                return std::unexpected(stats_error("%stats-ols-multi: y-col must be a non-negative integer"));
            auto y_col_idx = static_cast<std::size_t>(yv.int_val);

            auto x_col_indices = list_to_col_indices(heap, args[2], "%stats-ols-multi");
            if (!x_col_indices) return std::unexpected(x_col_indices.error());
            if (x_col_indices->empty())
                return std::unexpected(stats_error("%stats-ols-multi: need at least one predictor column"));

            auto y_res = column_to_eigen(**ft_res, y_col_idx, heap, "%stats-ols-multi");
            if (!y_res) return std::unexpected(y_res.error());
            y = std::move(*y_res);

            for (auto ci : *x_col_indices) {
                auto col = column_to_eigen(**ft_res, ci, heap, "%stats-ols-multi");
                if (!col) return std::unexpected(col.error());
                x_cols.push_back(std::move(*col));
            }
        } else {
            /// y-sequence + list-of-x-sequences
            auto y_res = stats::to_eigen(heap, args[0], "%stats-ols-multi");
            if (!y_res) return std::unexpected(y_res.error());
            y = std::move(*y_res);

            LispVal cur = args[1];
            if (cur == Nil)
                return std::unexpected(stats_error("%stats-ols-multi: need at least one predictor sequence"));
            while (cur != Nil) {
                if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                    return std::unexpected(stats_error("%stats-ols-multi: expected a list of predictor sequences"));
                auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
                if (!cons)
                    return std::unexpected(stats_error("%stats-ols-multi: expected a list of predictor sequences"));
                auto vec = stats::to_eigen(heap, cons->car, "%stats-ols-multi");
                if (!vec) return std::unexpected(vec.error());
                x_cols.push_back(std::move(*vec));
                cur = cons->cdr;
            }
        }

        auto n = y.size();
        auto k = static_cast<Eigen::Index>(x_cols.size());

        if (n < k + 2)
            return std::unexpected(stats_error("%stats-ols-multi: need at least (k+2) observations for k predictors"));

        Eigen::MatrixXd X(n, k + 1);
        X.col(0).setOnes();  ///< intercept
        for (Eigen::Index j = 0; j < k; ++j) {
            if (x_cols[static_cast<std::size_t>(j)].size() != n)
                return std::unexpected(stats_error("%stats-ols-multi: all sequences must have the same length as y"));
            X.col(j + 1) = x_cols[static_cast<std::size_t>(j)];
        }

        Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(X);
        if (qr.rank() < k + 1)
            return std::unexpected(stats_error("%stats-ols-multi: design matrix is rank-deficient"));

        Eigen::VectorXd beta = qr.solve(y);

        /// Residuals
        Eigen::VectorXd residuals = y - X * beta;
        double sse = residuals.squaredNorm();
        double df_resid = static_cast<double>(n - k - 1);
        double mse = sse / df_resid;
        double residual_se = std::sqrt(mse);

        double y_mean = y.mean();
        double sst = (y.array() - y_mean).square().sum();
        double r_squared = (sst > 0.0) ? 1.0 - sse / sst : 0.0;
        double adj_r_squared = 1.0 - (1.0 - r_squared) * (static_cast<double>(n) - 1.0) / df_resid;

        Eigen::MatrixXd XtX = X.transpose() * X;
        Eigen::MatrixXd XtX_inv = XtX.inverse();
        Eigen::VectorXd se_beta(k + 1);
        for (Eigen::Index i = 0; i <= k; ++i)
            se_beta(i) = std::sqrt(XtX_inv(i, i) * mse);

        /// t-statistics and p-values
        Eigen::VectorXd t_stats(k + 1);
        Eigen::VectorXd p_values(k + 1);
        for (Eigen::Index i = 0; i <= k; ++i) {
            t_stats(i) = (se_beta(i) > 0.0) ? beta(i) / se_beta(i) : 0.0;
            p_values(i) = (se_beta(i) > 0.0) ? stats::t_pvalue_two_tailed(t_stats(i), df_resid) : 1.0;
        }

        /// Build the result alist
        auto coeff_list = eigen_to_list(beta, heap);
        if (!coeff_list) return std::unexpected(coeff_list.error());
        auto se_list = eigen_to_list(se_beta, heap);
        if (!se_list) return std::unexpected(se_list.error());
        auto t_list = eigen_to_list(t_stats, heap);
        if (!t_list) return std::unexpected(t_list.error());
        auto p_list = eigen_to_list(p_values, heap);
        if (!p_list) return std::unexpected(p_list.error());

        auto r2_enc = ops::encode(r_squared);
        if (!r2_enc) return std::unexpected(stats_error("%stats-ols-multi: encoding error"));
        auto ar2_enc = ops::encode(adj_r_squared);
        if (!ar2_enc) return std::unexpected(stats_error("%stats-ols-multi: encoding error"));
        auto rse_enc = ops::encode(residual_se);
        if (!rse_enc) return std::unexpected(stats_error("%stats-ols-multi: encoding error"));

        /// Build alist pairs
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

        /// Chain into alist
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


} ///< namespace eta::stats_bindings

