# std.actor.supervisor

[<- Stdlib Reference](./README.md) | [Language Guide](../../language_guide.md) | [Detailed reference](../../stdlib/actor-supervisor.md)

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

Supported options:

- Child `'restart` policy: `'permanent`, `'transient`, `'temporary`.
- Supervisor `'strategy`: `'one-for-one`, `'one-for-all`, `'rest-for-one`.
- Restart intensity controls: `'max-restarts`, `'max-seconds`.

`std.supervisor` is a compatibility shim that re-exports this module.
