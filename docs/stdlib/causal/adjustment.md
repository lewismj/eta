# std.causal.adjustment

Adjustment criteria and structural detectors over mixed graphs (directed
`->` and bidirected `<->` edges).

```scheme
(import std.causal.adjustment)
```

| Symbol | Description |
| --- | --- |
| `(dag:gac? g x y z)` | Generalized Adjustment Criterion check on `g` for `(X, Y)` with adjustment set `Z`. |
| `(dag:minimal-adjustments g x y observed)` | Enumerate minimal valid adjustment sets drawn from `observed`. |
| `(front-door? g x y m)` | Front-door criterion check for mediator set `M`. |
| `(do:front-door-formula y x m)` | Canonical front-door estimand AST. |
| `(iv? g z x y)` | Instrumental-variable criterion check. |

