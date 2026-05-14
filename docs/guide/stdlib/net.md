# std.net

[← Stdlib Reference](./README.md) · [Language Guide](../../language_guide.md) · [Detailed reference](../reference/networking.md)

High-level networking patterns built over the NNG actor primitives.

> **Build flag:** Available only when Eta is built with `-DETA_BUILD_NNG=ON`.

```scheme
(import std.net)
```

| Symbol | Description |
| --- | --- |
| `(with-socket spec thunk)` | Open a socket, run `thunk` with it bound, close on exit. |
| `(request-reply url request)` | Send `request` to a REQ/REP server, return the reply. |
| `(worker-pool url n handler)` | Start `n` workers that call `(handler request)` and reply. |
| `(pub-sub url topic handler)` | Subscribe to `topic` and dispatch each message to `handler`. |
| `(survey url request timeout-ms)` | Survey several responders, collect replies until timeout. |

---

**Source:** [`stdlib/std/net.eta`](../../../stdlib/std/net.eta)

