# std.causal.estimate

Causal effect estimation backends. Input data is a list of observations,
each an alist such as `'((x . 1) (z . 0) (y . 2.3))`. Treatment `X` is
binary in `{0, 1}` unless noted.

```scheme
(import std.causal.estimate)
```

## ATE estimators

| Symbol | Description |
| --- | --- |
| `(do:ate data y x z)` | Default ATE estimator (AIPW). |
| `(do:ate-gformula data y x z)` | G-formula via stratified outcome means. |
| `(do:ate-ipw data y x z)` | IPW with stratified empirical propensity scores. |
| `(do:ate-aipw data y x z)` | Augmented IPW (doubly robust). |
| `(do:ate-tmle data y x z)` | Targeted maximum-likelihood estimator. |

## Inference

| Symbol | Description |
| --- | --- |
| `(do:bootstrap-ci estimator data y x z . opts)` | Non-parametric bootstrap CI. Options include `'reps`, `'alpha`, `'seed`. |

## Sub-routines

| Symbol | Description |
| --- | --- |
| `(do:propensity-score data x z)` | Stratified empirical propensity scores. |
| `(causal:design-matrix data cols)` | Build a row-major design matrix. |
| `(causal:response-vector data y)` | Build the outcome vector. |

## Sensitivity

| Symbol | Description |
| --- | --- |
| `(do:e-value rr)` | Compute the E-value for a risk ratio. |
| `(do:rosenbaum-bound . args)` | Rosenbaum-style bound for an unmeasured confounder. |

