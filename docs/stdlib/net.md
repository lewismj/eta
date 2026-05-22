# std.net

High-level networking patterns built over NNG transport primitives.
Available only when Eta is built with `-DETA_BUILD_NNG=ON`.

For local PID and mailbox messaging inside one Eta runtime, use `std.actor`.
For distributed actor nodes over the same NNG transport layer, use
`std.actor.node`.

```scheme
(import std.net)
```

| Symbol | Description |
| --- | --- |
| `(nng-monitor socket)` | Register socket disconnect monitoring for `recv!` down messages. |
| `(nng-demonitor socket)` | Remove socket disconnect monitoring. |
| `(with-socket spec thunk)` | Open a socket, run `thunk` with it bound, close on exit. |
| `(request-reply endpoint message)` | Open a REQ socket, send one message, receive one reply, close. |
| `(worker-pool module-path tasks)` | Spawn one child per task and collect one reply per child in submission order. |
| `(pub-sub endpoint topics handler)` | Connect a SUB socket, subscribe to each topic, and invoke `handler` per message. |
| `(survey endpoint question timeout-ms)` | Open a SURVEYOR socket, send one question, collect replies until timeout. |

`worker-pool` is a compatibility helper over socket-based child workers. Each
worker module is expected to use:

1. `(recv! (current-mailbox) 'wait)` to read one task
2. `(send! (current-mailbox) result 'wait)` to return one result

`std.actor` / `std.actor.node` operate on actor PIDs and node handshakes.
`std.net` exposes raw socket workflows and explicit socket monitoring via
`nng-monitor` / `nng-demonitor`.

