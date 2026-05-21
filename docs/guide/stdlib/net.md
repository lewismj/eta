# std.net

[← Stdlib Reference](./README.md) · [Language Guide](../../language_guide.md) · [Detailed reference](../reference/networking.md)

High-level networking patterns built over NNG transport primitives.

For local PID and mailbox messaging inside one Eta runtime, use `std.actor`.

> **Build flag:** Available only when Eta is built with `-DETA_BUILD_NNG=ON`.

```scheme
(import std.net)
```

| Symbol | Description |
| --- | --- |
| `(nng-monitor socket)` | Register socket disconnect monitoring for `recv!` down messages. |
| `(nng-demonitor socket)` | Remove socket disconnect monitoring. |
| `(with-socket spec thunk)` | Open a socket, run `thunk` with it bound, close on exit. |
| `(request-reply url request)` | Send `request` to a REQ/REP server, return the reply. |
| `(worker-pool url n handler)` | Start `n` workers that call `(handler request)` and reply. |
| `(pub-sub url topic handler)` | Subscribe to `topic` and dispatch each message to `handler`. |
| `(survey url request timeout-ms)` | Survey several responders, collect replies until timeout. |

`std.actor` uses `monitor`/`demonitor` for PID monitoring. `std.net` exposes
`nng-monitor`/`nng-demonitor` to keep socket monitoring explicit.

---

**Source:** [`stdlib/std/net.eta`](../../../stdlib/std/net.eta)

