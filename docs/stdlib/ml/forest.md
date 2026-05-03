# std.ml.forest

Bagged regression forests built on `std.ml.tree`.

```scheme
(import std.ml.forest)
```

| Symbol | Description |
| --- | --- |
| `(forest:fit x-matrix y-vector . opts)` | Fit a regression forest. Options: `'n-trees`, `'min-leaf`, `'max-depth`, `'mtry`, `'bootstrap?`, `'seed`. |
| `(forest:predict model x-matrix)` | Predict for new rows. |
| `(forest:fit-parallel x-matrix y-vector . opts)` | Same as `forest:fit` but trains trees in parallel using `std.net` workers. |
| `(forest:make-rf-regressor . opts)` | Learner-spec record for use with `std.causal.cate`. |

