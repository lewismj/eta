# std.hashset

Helpers built over the runtime hash-set primitives.

```scheme
(import std.hashset)
```

| Symbol | Description |
| --- | --- |
| `(hash-set-empty? s)` | True when `s` has no elements. |
| `(hash-set-size s)` | Number of elements. |
| `(hash-set-subset? a b)` | True when every element of `a` is in `b`. |
| `(hash-set-equal? a b)` | True when `a` and `b` contain the same elements. |

