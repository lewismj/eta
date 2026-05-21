# std.supervisor

[<- Stdlib Reference](./README.md) | [Language Guide](../../language_guide.md)

Compatibility shim for `std.actor.supervisor`.

```scheme
(import std.supervisor)
```

| Symbol | Description |
| --- | --- |
| `(one-for-one specs . opts)` | Start a local one-for-one supervisor. |
| `(one-for-all specs . opts)` | Start a local one-for-all supervisor. |
| `(rest-for-one specs . opts)` | Start a local rest-for-one supervisor. |

This module keeps older imports working and re-exports entry points from
`std.actor.supervisor`.

Prefer `(import std.actor.supervisor)` for new code.

---

**Source:** [`stdlib/std/supervisor.eta`](../../../stdlib/std/supervisor.eta)
