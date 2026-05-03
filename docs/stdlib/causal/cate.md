# std.causal.cate

Meta-learners for conditional average treatment effects (S/T/X/R/DR).

A learner spec is an alist:

```scheme
'((fit . (lambda (X y) ... model))
  (predict . (lambda (model X) ... y-hat))
  (kind . regressor)              ; or 'classifier
  (fit-weighted . (lambda (X y w) ... model)))   ; optional
```

Rows are alists; matrices are row-major lists.

```scheme
(import std.causal.cate)
```

## Fit

| Symbol | Description |
| --- | --- |
| `(cate:fit-s-learner data y x cols spec)` | S-learner. |
| `(cate:fit-t-learner data y x cols spec)` | T-learner. |
| `(cate:fit-x-learner data y x cols spec)` | X-learner. |
| `(cate:fit-r-learner data y x cols spec)` | R-learner. |
| `(cate:fit-dr-learner data y x cols spec)` | DR-learner. |

## Use

| Symbol | Description |
| --- | --- |
| `(cate:predict model rows cols)` | Predict CATE for new rows. |
| `(cate:ate model rows cols)` | Average of predicted CATE. |
| `(cate:rank model rows cols)` | Rank rows by predicted CATE descending. |

## Diagnostics

| Symbol | Description |
| --- | --- |
| `(cate:residual-r2 model data y x cols)` | Residualised R-squared. |
| `(cate:propensity-overlap data x cols)` | Propensity-score overlap diagnostic. |

