# std.actor.gen_server

[<- Stdlib Reference](./README.md) | [Language Guide](../../language_guide.md) | [Detailed reference](../../stdlib/actor-gen-server.md)

OTP-style server behaviour built on `std.actor`.

```scheme
(import std.actor.gen_server)
```

| Symbol | Description |
| --- | --- |
| `(gen-server-start callbacks init-arg opts)` | Start one server process and return its pid. |
| `(gen-server-start-link callbacks init-arg opts)` | Start one server process and link it to the caller. |
| `(gen-server-call server request timeout-ms)` | Send one synchronous request; return reply value or `#f` on timeout/server-down. |
| `(gen-server-cast server message)` | Send one asynchronous message; returns `'ok` or `#f`. |
| `(gen-server-stop server reason timeout-ms)` | Stop one server with `reason`; waits for stop acknowledgement. |

Callback contract and message protocol:

```scheme
;; callbacks alist keys:
'init
'handle-call
'handle-cast
'handle-info   ; optional
'terminate     ; optional

'(gen-call ref from request)
'(gen-cast message)
'(gen-reply ref value)
```

Supported options:

- `'name` registers the server for name-based call/cast/stop.
- `'trap-exit` enables `trap-exit!` when the server starts.

See the runnable cookbook example:
[`cookbook/concurrency/gen-server-counter.eta`](../../../cookbook/concurrency/gen-server-counter.eta)

---

**Source:** [`stdlib/std/actor/gen_server.eta`](../../../stdlib/std/actor/gen_server.eta)
