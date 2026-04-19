#pragma once

/**
 * @file stats_math.h
 * @brief Eigen-backed statistical math functions for the Eta runtime.
 *
 * Provides:  mean, sample variance, std-dev, standard error,
 *            percentile, covariance, correlation, OLS regression,
 *            t-distribution CDF/quantile, and two-sample t-test.
 *
 * All vector operations delegate to Eigen; scalar special functions
 * (incomplete beta, t-CDF/quantile) use std::lgamma from <cmath>.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <vector>

#include <Eigen/Dense>

namespace eta::runtime::stats {


/// Zero-copy const view of a std::vector<double> as an Eigen column vector.
inline Eigen::Map<const Eigen::VectorXd> as_eigen(const std::vector<double>& v) {
    return {v.data(), static_cast<Eigen::Index>(v.size())};
}


inline double mean(const Eigen::Ref<const Eigen::VectorXd>& xs) {
    if (xs.size() == 0) return 0.0;
    return xs.mean();
}

/// Sample variance (Bessel-corrected, N-1 denominator).
inline double variance(const Eigen::Ref<const Eigen::VectorXd>& xs) {
    if (xs.size() < 2) return 0.0;
    double m = xs.mean();
    return (xs.array() - m).square().sum()
         / static_cast<double>(xs.size() - 1);
}

inline double stddev(const Eigen::Ref<const Eigen::VectorXd>& xs) {
    return std::sqrt(variance(xs));
}

inline double sem(const Eigen::Ref<const Eigen::VectorXd>& xs) {
    if (xs.size() < 2) return 0.0;
    return stddev(xs) / std::sqrt(static_cast<double>(xs.size()));
}

/// Takes a copy because we need to sort.
inline double percentile(Eigen::VectorXd xs, double p) {
    if (xs.size() == 0) return 0.0;
    std::sort(xs.data(), xs.data() + xs.size());
    if (p <= 0.0) return xs(0);
    if (p >= 1.0) return xs(xs.size() - 1);
    double idx = p * static_cast<double>(xs.size() - 1);
    auto lo = static_cast<Eigen::Index>(std::floor(idx));
    auto hi = static_cast<Eigen::Index>(std::ceil(idx));
    double frac = idx - static_cast<double>(lo);
    return xs(lo) * (1.0 - frac) + xs(hi) * frac;
}

/// Sample covariance (N-1 denominator).
inline std::optional<double> covariance(const Eigen::Ref<const Eigen::VectorXd>& xs,
                                         const Eigen::Ref<const Eigen::VectorXd>& ys) {
    if (xs.size() != ys.size() || xs.size() < 2) return std::nullopt;
    double mx = xs.mean(), my = ys.mean();
    return ((xs.array() - mx) * (ys.array() - my)).sum()
         / static_cast<double>(xs.size() - 1);
}

/// Pearson correlation coefficient.
inline std::optional<double> correlation(const Eigen::Ref<const Eigen::VectorXd>& xs,
                                          const Eigen::Ref<const Eigen::VectorXd>& ys) {
    auto cov = covariance(xs, ys);
    if (!cov) return std::nullopt;
    double sx = stddev(xs), sy = stddev(ys);
    if (sx == 0.0 || sy == 0.0) return std::nullopt;
    return *cov / (sx * sy);
}


inline double mean(const std::vector<double>& v) { return mean(as_eigen(v)); }
inline double variance(const std::vector<double>& v) { return variance(as_eigen(v)); }
inline double stddev(const std::vector<double>& v) { return stddev(as_eigen(v)); }
inline double sem(const std::vector<double>& v) { return sem(as_eigen(v)); }

inline double percentile(std::vector<double> v, double p) {
    Eigen::VectorXd ev = Eigen::Map<Eigen::VectorXd>(v.data(), static_cast<Eigen::Index>(v.size()));
    return percentile(std::move(ev), p);
}

inline std::optional<double> covariance(const std::vector<double>& xs,
                                         const std::vector<double>& ys) {
    return covariance(as_eigen(xs), as_eigen(ys));
}

inline std::optional<double> correlation(const std::vector<double>& xs,
                                          const std::vector<double>& ys) {
    return correlation(as_eigen(xs), as_eigen(ys));
}


/**
 * Regularized incomplete beta function I_x(a, b) via continued fraction
 * (modified Lentz algorithm).  Convergence is fast for most inputs.
 */
inline double betainc(double x, double a, double b) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;

    /// For better convergence, use the symmetry relation when x > (a+1)/(a+b+2)
    if (x > (a + 1.0) / (a + b + 2.0))
        return 1.0 - betainc(1.0 - x, b, a);

    /// Compute the log of the front factor:  x^a (1-x)^b / (a * Beta(a,b))
    double lbeta = std::lgamma(a) + std::lgamma(b) - std::lgamma(a + b);
    double front = std::exp(a * std::log(x) + b * std::log(1.0 - x) - lbeta) / a;

    /// Continued fraction (Lentz's method)
    constexpr int max_iter = 200;
    constexpr double eps = 1.0e-14;
    constexpr double tiny = 1.0e-30;

    double f = 1.0, C = 1.0, D = 0.0;
    for (int m = 0; m <= max_iter; ++m) {
        double d;
        if (m == 0) {
            d = 1.0;
        } else {
            int k = m;
            if (k % 2 == 0) {
                int half = k / 2;
                d = (static_cast<double>(half) * (b - static_cast<double>(half)) * x)
                    / ((a + static_cast<double>(k) - 1.0) * (a + static_cast<double>(k)));
            } else {
                int half = (k - 1) / 2;
                d = -((a + static_cast<double>(half)) * (a + b + static_cast<double>(half)) * x)
                    / ((a + static_cast<double>(k) - 1.0) * (a + static_cast<double>(k)));
            }
        }

        D = 1.0 + d * D;
        if (std::abs(D) < tiny) D = tiny;
        D = 1.0 / D;

        C = 1.0 + d / C;
        if (std::abs(C) < tiny) C = tiny;

        f *= C * D;
        if (std::abs(C * D - 1.0) < eps) break;
    }

    return front * (f - 1.0);
}


/// CDF of Student's t-distribution.
inline double t_cdf(double t, double df) {
    double x = df / (df + t * t);
    double ib = betainc(x, df / 2.0, 0.5);
    if (t >= 0.0)
        return 1.0 - 0.5 * ib;
    else
        return 0.5 * ib;
}

/// Two-tailed p-value for Student's t-test.
inline double t_pvalue_two_tailed(double t, double df) {
    return 2.0 * (1.0 - t_cdf(std::abs(t), df));
}

/// Inverse CDF (quantile) of Student's t-distribution via bisection.
inline double t_quantile(double p, double df) {
    if (p <= 0.0) return -1e30;
    if (p >= 1.0) return  1e30;
    if (std::abs(p - 0.5) < 1e-15) return 0.0;

    double lo = -1000.0, hi = 1000.0;
    for (int i = 0; i < 100; ++i) {
        double mid = 0.5 * (lo + hi);
        if (t_cdf(mid, df) < p)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}


struct ConfidenceInterval {
    double lower;
    double upper;
    double point_estimate;
    double margin;
};

/// Confidence interval for the mean at the given level (e.g. 0.95).
inline std::optional<ConfidenceInterval> ci_mean(const Eigen::Ref<const Eigen::VectorXd>& xs, double level) {
    if (xs.size() < 2 || level <= 0.0 || level >= 1.0) return std::nullopt;
    double m = mean(xs);
    double se = sem(xs);
    double df = static_cast<double>(xs.size() - 1);
    double alpha = 1.0 - level;
    double t_crit = t_quantile(1.0 - alpha / 2.0, df);
    double margin = t_crit * se;
    return ConfidenceInterval{m - margin, m + margin, m, margin};
}

inline std::optional<ConfidenceInterval> ci_mean(const std::vector<double>& v, double level) {
    return ci_mean(as_eigen(v), level);
}


struct TTestResult {
    double t_stat;
    double p_value;
    double df;
    double mean_diff;
};

/// Welch's two-sample t-test (unequal variances).
inline std::optional<TTestResult> t_test_2(const Eigen::Ref<const Eigen::VectorXd>& xs,
                                            const Eigen::Ref<const Eigen::VectorXd>& ys) {
    if (xs.size() < 2 || ys.size() < 2) return std::nullopt;
    double mx = mean(xs), my = mean(ys);
    double vx = variance(xs), vy = variance(ys);
    double nx = static_cast<double>(xs.size());
    double ny = static_cast<double>(ys.size());
    double se = std::sqrt(vx / nx + vy / ny);
    if (se == 0.0) return std::nullopt;

    double t = (mx - my) / se;

    /// Welch-Satterthwaite degrees of freedom
    double num = (vx / nx + vy / ny) * (vx / nx + vy / ny);
    double den = (vx / nx) * (vx / nx) / (nx - 1.0)
               + (vy / ny) * (vy / ny) / (ny - 1.0);
    double df = num / den;

    double p = t_pvalue_two_tailed(t, df);
    return TTestResult{t, p, df, mx - my};
}

inline std::optional<TTestResult> t_test_2(const std::vector<double>& xs,
                                            const std::vector<double>& ys) {
    return t_test_2(as_eigen(xs), as_eigen(ys));
}


struct OLSResult {
    double slope;
    double intercept;
    double r_squared;
    double se_slope;
    double se_intercept;
    double t_slope;
    double t_intercept;
    double p_slope;
    double p_intercept;
};

inline std::optional<OLSResult> ols(const Eigen::Ref<const Eigen::VectorXd>& xs,
                                     const Eigen::Ref<const Eigen::VectorXd>& ys) {
    if (xs.size() != ys.size() || xs.size() < 3) return std::nullopt;

    Eigen::Index n = xs.size();

    /// Design matrix [1 | x]
    Eigen::MatrixXd X(n, 2);
    X.col(0).setOnes();
    X.col(1) = xs;

    /// Solve via QR decomposition
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(X);
    if (qr.rank() < 2) return std::nullopt;
    Eigen::VectorXd beta = qr.solve(ys);

    double intercept = beta(0);
    double slope     = beta(1);

    /// Residuals
    Eigen::VectorXd residuals = ys - X * beta;
    double sse = residuals.squaredNorm();
    double df  = static_cast<double>(n - 2);
    double mse = sse / df;

    double y_mean = ys.mean();
    double sst = (ys.array() - y_mean).square().sum();
    double r2 = (sst > 0.0) ? 1.0 - sse / sst : 0.0;

    Eigen::MatrixXd XtX_inv = (X.transpose() * X).inverse();
    double se_intercept = std::sqrt(XtX_inv(0, 0) * mse);
    double se_slope     = std::sqrt(XtX_inv(1, 1) * mse);

    double t_slope     = (se_slope     > 0.0) ? slope     / se_slope     : 0.0;
    double t_intercept = (se_intercept > 0.0) ? intercept / se_intercept : 0.0;
    double p_slope     = (se_slope     > 0.0) ? t_pvalue_two_tailed(t_slope,     df) : 1.0;
    double p_intercept = (se_intercept > 0.0) ? t_pvalue_two_tailed(t_intercept, df) : 1.0;

    return OLSResult{slope, intercept, r2,
                     se_slope, se_intercept,
                     t_slope, t_intercept,
                     p_slope, p_intercept};
}

inline std::optional<OLSResult> ols(const std::vector<double>& xs,
                                     const std::vector<double>& ys) {
    return ols(as_eigen(xs), as_eigen(ys));
}

} ///< namespace eta::runtime::stats

