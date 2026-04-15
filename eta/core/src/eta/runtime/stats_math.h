#pragma once

/// @file stats_math.h
/// @brief Pure C++ statistical math functions for the Eta runtime.
///
/// Provides:  mean, sample variance, std-dev, standard error,
///            percentile, covariance, correlation, OLS regression,
///            t-distribution CDF/quantile, and two-sample t-test.
///
/// No external dependency — uses only <cmath> and <algorithm>.

// M_PI is a POSIX extension; MSVC requires _USE_MATH_DEFINES before <cmath>.
#ifdef _WIN32
#define M_PI 3.14159265358979323846
#endif


#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace eta::runtime::stats {

// ─── Descriptive statistics ──────────────────────────────────────────

inline double mean(const std::vector<double>& xs) {
    if (xs.empty()) return 0.0;
    double sum = 0.0;
    for (double x : xs) sum += x;
    return sum / static_cast<double>(xs.size());
}

/// Sample variance (Bessel-corrected, N-1 denominator).
inline double variance(const std::vector<double>& xs) {
    if (xs.size() < 2) return 0.0;
    double m = mean(xs);
    double ss = 0.0;
    for (double x : xs) { double d = x - m; ss += d * d; }
    return ss / static_cast<double>(xs.size() - 1);
}

inline double stddev(const std::vector<double>& xs) {
    return std::sqrt(variance(xs));
}

/// Standard error of the mean  (s / √n).
inline double sem(const std::vector<double>& xs) {
    if (xs.size() < 2) return 0.0;
    return stddev(xs) / std::sqrt(static_cast<double>(xs.size()));
}

/// p-th percentile (0 ≤ p ≤ 1) using linear interpolation.
inline double percentile(std::vector<double> xs, double p) {
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    if (p <= 0.0) return xs.front();
    if (p >= 1.0) return xs.back();
    double idx = p * static_cast<double>(xs.size() - 1);
    auto lo = static_cast<std::size_t>(std::floor(idx));
    auto hi = static_cast<std::size_t>(std::ceil(idx));
    double frac = idx - static_cast<double>(lo);
    return xs[lo] * (1.0 - frac) + xs[hi] * frac;
}

/// Sample covariance (N-1 denominator).
inline std::optional<double> covariance(const std::vector<double>& xs,
                                         const std::vector<double>& ys) {
    if (xs.size() != ys.size() || xs.size() < 2) return std::nullopt;
    double mx = mean(xs), my = mean(ys);
    double sum = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i)
        sum += (xs[i] - mx) * (ys[i] - my);
    return sum / static_cast<double>(xs.size() - 1);
}

/// Pearson correlation coefficient.
inline std::optional<double> correlation(const std::vector<double>& xs,
                                          const std::vector<double>& ys) {
    auto cov = covariance(xs, ys);
    if (!cov) return std::nullopt;
    double sx = stddev(xs), sy = stddev(ys);
    if (sx == 0.0 || sy == 0.0) return std::nullopt;
    return *cov / (sx * sy);
}

// ─── Special functions: Regularized Incomplete Beta ──────────────────

/// Log-Gamma via Lanczos approximation (g=7, n=9 coefficients).
inline double lgamma_approx(double x) {
    static const double c[9] = {
         0.99999999999980993,
       676.5203681218851,
     -1259.1392167224028,
       771.32342877765313,
      -176.61502916214059,
        12.507343278686905,
        -0.13857109526572012,
         9.9843695780195716e-6,
         1.5056327351493116e-7
    };
    if (x < 0.5) {
        // Reflection formula
        return std::log(M_PI / std::sin(M_PI * x)) - lgamma_approx(1.0 - x);
    }
    x -= 1.0;
    double a = c[0];
    double t = x + 7.5;   // g + 0.5
    for (int i = 1; i < 9; ++i)
        a += c[i] / (x + static_cast<double>(i));
    return 0.5 * std::log(2.0 * M_PI) + (x + 0.5) * std::log(t) - t + std::log(a);
}

/// Regularized incomplete beta function I_x(a, b) via continued fraction
/// (modified Lentz algorithm).  Convergence is fast for most inputs.
inline double betainc(double x, double a, double b) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;

    // For better convergence, use the symmetry relation when x > (a+1)/(a+b+2)
    if (x > (a + 1.0) / (a + b + 2.0))
        return 1.0 - betainc(1.0 - x, b, a);

    // Compute the log of the front factor:  x^a (1-x)^b / (a * Beta(a,b))
    double lbeta = lgamma_approx(a) + lgamma_approx(b) - lgamma_approx(a + b);
    double front = std::exp(a * std::log(x) + b * std::log(1.0 - x) - lbeta) / a;

    // Continued fraction (Lentz's method)
    constexpr int max_iter = 200;
    constexpr double eps = 1.0e-14;
    constexpr double tiny = 1.0e-30;

    double f = 1.0, C = 1.0, D = 0.0;
    // The continued fraction for I_x(a,b) is 1/(1+ d1/(1+ d2/(1+ ...)))
    // with d_{2m+1} = -(a+m)(a+b+m)x / ((a+2m)(a+2m+1))
    //      d_{2m}   =  m(b-m)x / ((a+2m-1)(a+2m))
    for (int m = 0; m <= max_iter; ++m) {
        double d;
        if (m == 0) {
            d = 1.0; // first term
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

// ─── t-Distribution CDF / Quantile ──────────────────────────────────

/// CDF of Student's t-distribution (two-tailed p-value is 2*(1 - cdf(|t|,df))).
inline double t_cdf(double t, double df) {
    // F(t; ν) = I(ν/(ν+t²); ν/2, 1/2) * 0.5        if t < 0
    //         = 1 - I(ν/(ν+t²); ν/2, 1/2) * 0.5     if t ≥ 0
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
/// Returns t such that P(T ≤ t) = p.
inline double t_quantile(double p, double df) {
    if (p <= 0.0) return -1e30;
    if (p >= 1.0) return  1e30;
    if (std::abs(p - 0.5) < 1e-15) return 0.0;

    // Bisection on [-1000, 1000] — more than enough for any practical df.
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

// ─── Confidence Intervals ────────────────────────────────────────────

struct ConfidenceInterval {
    double lower;
    double upper;
    double point_estimate;
    double margin;
};

/// Confidence interval for the mean at the given level (e.g. 0.95).
inline std::optional<ConfidenceInterval> ci_mean(const std::vector<double>& xs, double level) {
    if (xs.size() < 2 || level <= 0.0 || level >= 1.0) return std::nullopt;
    double m = mean(xs);
    double se = sem(xs);
    double df = static_cast<double>(xs.size() - 1);
    double alpha = 1.0 - level;
    double t_crit = t_quantile(1.0 - alpha / 2.0, df);
    double margin = t_crit * se;
    return ConfidenceInterval{m - margin, m + margin, m, margin};
}

// ─── Two-Sample t-Test ───────────────────────────────────────────────

struct TTestResult {
    double t_stat;
    double p_value;
    double df;
    double mean_diff;
};

/// Welch's two-sample t-test (unequal variances).
inline std::optional<TTestResult> t_test_2(const std::vector<double>& xs,
                                            const std::vector<double>& ys) {
    if (xs.size() < 2 || ys.size() < 2) return std::nullopt;
    double mx = mean(xs), my = mean(ys);
    double vx = variance(xs), vy = variance(ys);
    double nx = static_cast<double>(xs.size());
    double ny = static_cast<double>(ys.size());
    double se = std::sqrt(vx / nx + vy / ny);
    if (se == 0.0) return std::nullopt;

    double t = (mx - my) / se;

    // Welch-Satterthwaite degrees of freedom
    double num = (vx / nx + vy / ny) * (vx / nx + vy / ny);
    double den = (vx / nx) * (vx / nx) / (nx - 1.0)
               + (vy / ny) * (vy / ny) / (ny - 1.0);
    double df = num / den;

    double p = t_pvalue_two_tailed(t, df);
    return TTestResult{t, p, df, mx - my};
}

// ─── Ordinary Least Squares (Simple Linear Regression) ───────────────

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

/// Simple OLS:  y = β₀ + β₁·x
inline std::optional<OLSResult> ols(const std::vector<double>& xs,
                                     const std::vector<double>& ys) {
    if (xs.size() != ys.size() || xs.size() < 3) return std::nullopt;

    double n = static_cast<double>(xs.size());
    double mx = mean(xs), my = mean(ys);
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        double dx = xs[i] - mx;
        double dy = ys[i] - my;
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }
    if (sxx == 0.0) return std::nullopt;

    double slope = sxy / sxx;
    double intercept = my - slope * mx;

    // Residual sum of squares
    double sse = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        double r = ys[i] - (intercept + slope * xs[i]);
        sse += r * r;
    }

    double r2 = (syy > 0.0) ? 1.0 - sse / syy : 0.0;
    double mse = sse / (n - 2.0);   // mean squared error (residual)

    // Standard errors
    double se_slope     = std::sqrt(mse / sxx);
    double se_intercept = std::sqrt(mse * (1.0 / n + mx * mx / sxx));

    double df = n - 2.0;
    double t_slope     = (se_slope     > 0.0) ? slope     / se_slope     : 0.0;
    double t_intercept = (se_intercept > 0.0) ? intercept / se_intercept : 0.0;
    double p_slope     = (se_slope     > 0.0) ? t_pvalue_two_tailed(t_slope,     df) : 1.0;
    double p_intercept = (se_intercept > 0.0) ? t_pvalue_two_tailed(t_intercept, df) : 1.0;

    return OLSResult{slope, intercept, r2,
                     se_slope, se_intercept,
                     t_slope, t_intercept,
                     p_slope, p_intercept};
}

} // namespace eta::runtime::stats

