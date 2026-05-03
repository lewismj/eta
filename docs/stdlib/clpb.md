# std.clpb

CLP(B) Boolean propagation-only solver layered on `std.clp`.

```scheme
(import std.clpb)
```

| Symbol | Description |
| --- | --- |
| `(clp:boolean var)` | Constrain `var` to `{0, 1}`. |
| `(clp:and a b z)` | `z = a AND b`. |
| `(clp:or a b z)` | `z = a OR b`. |
| `(clp:xor a b z)` | `z = a XOR b`. |
| `(clp:imp a b z)` | `z = (a -> b)`. |
| `(clp:eq a b z)` | `z = (a == b)`. |
| `(clp:not a z)` | `z = NOT a`. |
| `(clp:card lo hi xs)` | Cardinality: number of true vars in `xs` is in `[lo, hi]`. |
| `(clp:labeling-b vars)` | Enumerate Boolean assignments. |
| `(clp:sat? formula)` | True when the formula is satisfiable. |
| `(clp:taut? formula)` | True when the formula is a tautology. |

