# std.causal.identify

ID and IDC algorithms over ADMGs.

```scheme
(import std.causal.identify)
```

## Estimand AST

| Node | Meaning |
| --- | --- |
| `(P vars)` | Observational factor. |
| `(sum vars expr)` | Marginalisation over `vars`. |
| `(prod e1 e2 ...)` | Product of factors. |
| `(cond-on expr cond)` | Conditional expression. |
| `(fail (hedge ...))` | Non-identifiable witness. |

## Procedures

| Symbol | Description |
| --- | --- |
| `(id g y x)` | Estimand for `P(Y | do(X))` over ADMG `g`. |
| `(id-rec g y x)` | Recursive helper used internally; exposed for testing. |
| `(idc g y x z)` | Estimand for `P(Y | do(X), Z)`. |
| `(do:simplify estimand)` | Algebraic simplification of an estimand AST. |

