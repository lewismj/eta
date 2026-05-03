# std.causal.crossfit

Cross-fitting and double machine-learning (DML) estimators.

```scheme
(import std.causal.crossfit)
```

| Symbol | Description |
| --- | --- |
| `(crossfit:k-folds data k seed)` | Partition `data` into `k` folds. |
| `(crossfit:nuisance data folds spec target cols)` | Cross-fit a single nuisance regression. |
| `(crossfit:nuisance-arm data folds spec y x arm cols)` | Cross-fit per treatment arm. |
| `(crossfit:dml-plr data y x cols . opts)` | Partially-linear DML estimator. |
| `(crossfit:dml-irm data y x cols . opts)` | Interactive regression DML estimator. |
| `(crossfit:influence-se psi)` | Standard error from influence values. |
| `(crossfit:dml-ci estimate se [alpha])` | Confidence interval from DML output. |

Options include `'folds`, `'seed`, `'outcome-spec`, `'propensity-spec`.

