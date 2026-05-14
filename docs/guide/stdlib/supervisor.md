# std.supervisor

[← Stdlib Reference](./README.md) · [Language Guide](../../language_guide.md) · [Detailed reference](../reference/supervisor.md)

Erlang-inspired actor supervision strategies.

> **Build flag:** Available only when Eta is built with `-DETA_BUILD_NNG=ON`.

```scheme
(import std.supervisor)
```

| Symbol | Description |
| --- | --- |
| `(one-for-one specs)` | Restart only the failed child. |
| `(one-for-all specs)` | Restart every child when any one fails. |

Each `spec` is a list `(id start-thunk . opts)` describing how to (re)start
a child.

---

**Source:** [`stdlib/std/supervisor.eta`](../../../stdlib/std/supervisor.eta)

