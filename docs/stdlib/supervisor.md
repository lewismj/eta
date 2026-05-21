# std.supervisor

Compatibility shim for `std.actor.supervisor`.

```scheme
(import std.supervisor)
```

| Symbol | Description |
| --- | --- |
| `(one-for-one specs . opts)` | Start a local one-for-one supervisor. |
| `(one-for-all specs . opts)` | Start a local one-for-all supervisor. |
| `(rest-for-one specs . opts)` | Start a local rest-for-one supervisor. |

This module re-exports `std.actor.supervisor` entry points for existing code.
Prefer importing `std.actor.supervisor` directly for new code.

