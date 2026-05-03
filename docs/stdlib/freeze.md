# std.freeze

Attributed-variable combinators that delay or constrain logic variables.

```scheme
(import std.freeze)
```

| Symbol | Description |
| --- | --- |
| `(freeze var goal)` | Suspend `goal` until `var` is bound. |
| `(dif a b)` | Structural disequality constraint: `a` must never unify with `b`. |

