# std.net

[← Stdlib Reference](./README.md) · [Language Guide](../../language_guide.md) · [Detailed reference](../reference/networking.md)

High-level networking patterns built over NNG transport primitives.

For local PID and mailbox messaging inside one Eta runtime, use `std.actor`.
For distributed actor nodes over the same NNG transport layer, use
`std.actor.node`.

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

`std.actor` / `std.actor.node` operate on actor PIDs and node handshakes.
`std.net` exposes raw socket workflows and explicit socket monitoring via
`nng-monitor` / `nng-demonitor`.

---

**Source:** [`stdlib/std/net.eta`](../../../stdlib/std/net.eta)

