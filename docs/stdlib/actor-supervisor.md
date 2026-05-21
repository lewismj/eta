# std.actor.supervisor

Local actor supervisor helpers built on `std.actor`.

```scheme
(import std.actor.supervisor)
```

| Symbol | Description |
| --- | --- |
| `(make-child-spec id start . opts)` | Build one child specification alist. |
| `(child-spec id start . opts)` | Alias for `make-child-spec`. |
| `(supervisor-start key value . opts)` | Start one supervisor process. |
| `(supervisor-start-link key value . opts)` | Start one linked supervisor process. |
| `(supervisor-pid? value)` | Return `#t` when `value` is a supervisor pid. |
| `(supervisor-which-children supervisor)` | Return list of `(id pid)` pairs. |
| `(supervisor-count-children supervisor)` | Return count of running children. |
| `(supervisor-start-child supervisor spec)` | Start and append one child spec. |
| `(supervisor-terminate-child supervisor id)` | Stop one child by id. |
| `(supervisor-restart-child supervisor id)` | Restart one child by id. |
| `(supervisor-delete-child supervisor id)` | Stop and remove one child by id. |
| `(one-for-one specs . opts)` | Restart only the failed child. |
| `(one-for-all specs . opts)` | Restart every child when one fails. |
| `(rest-for-one specs . opts)` | Restart failed child and children started after it. |

Supported child spec keys:

- `'id` child identifier.
- `'start` zero-argument thunk for child startup.
- `'restart` one of `'permanent`, `'transient`, `'temporary`.
- `'shutdown` shutdown timeout metadata.
- `'type` child kind metadata.
- `'modules` module metadata.

Supported supervisor options:

- `'strategy` one of `'one-for-one`, `'one-for-all`, `'rest-for-one`.
- `'children` list of child specs.
- `'max-restarts` restart intensity threshold.
- `'max-seconds` restart intensity time window.
