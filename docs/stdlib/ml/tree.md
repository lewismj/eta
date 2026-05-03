# std.ml.tree

Regression CART with optional honest leaf estimation.

```scheme
(import std.ml.tree)
```

| Symbol | Description |
| --- | --- |
| `(tree:fit x-matrix y-vector . opts)` | Fit a regression tree. Options: `'min-leaf`, `'max-depth`, `'mtry`, `'honest?`, `'seed`. |
| `(tree:predict model x-matrix)` | Predict for new rows. |
| `(tree:leaves model)` | Leaf records of the fitted tree. |
| `(tree:leaf-membership model x-matrix)` | Leaf id assigned to each input row. |

