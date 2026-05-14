# std.freeze

[← Stdlib Reference](./README.md) · [Language Guide](../../language_guide.md) · [Detailed reference](../reference/freeze.md)

Attributed-variable combinators that delay or constrain logic variables.

```scheme
(import std.freeze)
```

| Symbol | Description |
| --- | --- |
| `(freeze var goal)` | Suspend `goal` until `var` is bound. |
| `(dif a b)` | Structural disequality constraint: `a` must never unify with `b`. |

---

**Source:** [`stdlib/std/freeze.eta`](../../../stdlib/std/freeze.eta)

