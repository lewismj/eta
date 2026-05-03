# std.stats

Descriptive statistics, t-tests, ordinary least squares (univariate and
multivariate), and learner specs used by `std.causal.*`.

```scheme
(import std.stats)
```

## Descriptive (scalar inputs are lists of numbers)

| Symbol | Description |
| --- | --- |
| `(stats:mean xs)` | Arithmetic mean. |
| `(stats:variance xs)` | Sample variance. |
| `(stats:stddev xs)` | Sample standard deviation. |
| `(stats:sem xs)` | Standard error of the mean. |
| `(stats:percentile xs p)` | Percentile (`p` in `[0, 100]`). |
| `(stats:median xs)` | Median. |
| `(stats:covariance xs ys)` | Sample covariance. |
| `(stats:correlation xs ys)` | Pearson correlation. |
| `(stats:summary xs)` | Alist with mean, variance, stddev, min, max, n. |

## Confidence intervals

| Symbol | Description |
| --- | --- |
| `(stats:ci xs alpha)` | Two-sided CI for the mean at level `1-alpha`. |
| `(stats:ci-lower xs alpha)` | Lower bound. |
| `(stats:ci-upper xs alpha)` | Upper bound. |

## Distributions

| Symbol | Description |
| --- | --- |
| `(stats:t-cdf t df)` | Student-t CDF. |
| `(stats:t-quantile p df)` | Student-t quantile. |
| `(stats:normal-quantile p)` | Standard normal quantile. |

## t-test (two-sample, equal variances)

| Symbol | Description |
| --- | --- |
| `(stats:t-test xs ys)` | Full t-test result alist. |
| `(stats:t-test-stat xs ys)` | t statistic. |
| `(stats:t-test-pvalue xs ys)` | Two-sided p-value. |
| `(stats:t-test-df xs ys)` | Degrees of freedom. |
| `(stats:t-test-mean-diff xs ys)` | Mean difference. |

## Simple OLS (`y = a + b*x`)

| Symbol | Description |
| --- | --- |
| `(stats:ols xs ys)` | Fit and return result alist. |
| `(stats:ols-slope xs ys)` | Slope `b`. |
| `(stats:ols-intercept xs ys)` | Intercept `a`. |
| `(stats:ols-r2 xs ys)` | R-squared. |
| `(stats:ols-se-slope xs ys)` | Slope standard error. |
| `(stats:ols-se-intercept xs ys)` | Intercept standard error. |
| `(stats:ols-t-slope xs ys)` | Slope t-statistic. |
| `(stats:ols-t-intercept xs ys)` | Intercept t-statistic. |
| `(stats:ols-p-slope xs ys)` | Slope p-value. |
| `(stats:ols-p-intercept xs ys)` | Intercept p-value. |

## Vector statistics

| Symbol | Description |
| --- | --- |
| `(stats:mean-vec rows)` | Column means. |
| `(stats:var-vec rows)` | Column variances. |
| `(stats:cov-matrix rows)` | Sample covariance matrix. |
| `(stats:cor-matrix rows)` | Sample correlation matrix. |
| `(stats:quantile-vec rows p)` | Column quantiles. |

## Multivariate OLS

| Symbol | Description |
| --- | --- |
| `(stats:ols-multi x-matrix y-vector)` | Fit; returns a record. |
| `(stats:ols-multi-coefficients fit)` | Coefficient vector. |
| `(stats:ols-multi-std-errors fit)` | Standard errors. |
| `(stats:ols-multi-t-stats fit)` | t statistics. |
| `(stats:ols-multi-p-values fit)` | p-values. |
| `(stats:ols-multi-r-squared fit)` | R-squared. |
| `(stats:ols-multi-adj-r-squared fit)` | Adjusted R-squared. |
| `(stats:ols-multi-residual-se fit)` | Residual standard error. |

## Learner specs (used by causal modules)

| Symbol | Description |
| --- | --- |
| `(stats:make-ols-regressor)` | Learner-spec record wrapping multivariate OLS. |
| `(stats:make-logistic)` | Learner-spec record for binary logistic regression. |

