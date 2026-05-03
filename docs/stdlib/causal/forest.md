# std.causal.forest

Causal forest built on doubly-robust pseudo-outcomes, layered on
`std.ml.forest`.

```scheme
(import std.causal.forest)
```

| Symbol | Description |
| --- | --- |
| `(forest:fit-causal-forest data y x cols . opts)` | Fit a causal forest. Options: `'n-trees`, `'min-leaf`, `'max-depth`, `'mtry`, `'seed`. |
| `(forest:predict-cate model rows cols)` | Predicted CATE per row. |
| `(forest:variable-importance model)` | Per-feature importance scores. |
| `(forest:local-aipw model row cols)` | Local AIPW estimate at a single row. |

