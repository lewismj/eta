# eta-nng

Networking and actor-model concurrency for Eta, backed by
[nng](https://github.com/nanomsg/nng) (Nanomsg Next Generation).

`eta-nng` is a native sidecar that provides the low-level `nng-*`, `send!`,
`recv!`, and `spawn*` runtime primitives used by the `std.net` module and the
concurrency cookbook.  The high-level Eta API lives in `std/net.eta`.

Protocols supported: `pair` · `req` · `rep` · `pub` · `sub` · `push` · `pull` ·
`surveyor` · `respondent` · `bus`.

## Quick start

```eta
(import std.net)

;; One-shot request/reply
(let ((reply (request-reply "tcp://localhost:5555" '(ping))))
  (display reply))
```

## Module

```eta
(import std.net)   ; high-level patterns (with-socket, request-reply, etc.)
```

The low-level primitives (`nng-socket`, `send!`, `recv!`, `spawn`, …) are
builtins available globally when the `eta-nng` sidecar is loaded — no import
needed for the primitives themselves.

## Low-level primitives

### Sockets

| Primitive | Signature | Description |
|---|---|---|
| `nng-socket` | `(nng-socket proto)` | Open a socket for protocol symbol |
| `nng-listen` | `(nng-listen sock endpoint)` | Bind and listen on a URL |
| `nng-dial` | `(nng-dial sock endpoint)` | Connect to a remote endpoint |
| `nng-close` | `(nng-close sock)` | Close a socket |
| `nng-socket?` | `(nng-socket? x)` | Predicate |
| `nng-subscribe` | `(nng-subscribe sub-sock topic)` | Set topic filter on a `sub` socket |
| `nng-set-option` | `(nng-set-option sock opt val)` | Set a socket option |
| `nng-poll` | `(nng-poll items timeout-ms)` | Non-blocking readiness check |

Protocol symbols: `'pair` `'req` `'rep` `'pub` `'sub` `'push` `'pull`
`'surveyor` `'respondent` `'bus`.

Option symbols for `nng-set-option`: `'recv-timeout` `'send-timeout`
`'recv-buf-size` `'survey-time` (all values in milliseconds or buffer count).

```eta
;; Echo server skeleton
(let ((sock (nng-socket 'rep)))
  (nng-listen sock "tcp://*:5555")
  (let ((msg (recv! sock 'wait)))
    (send! sock msg 'wait))
  (nng-close sock))
```

### Sending and receiving

| Primitive | Signature | Description |
|---|---|---|
| `send!` | `(send! sock val . flag)` | Send an Eta value (binary serialisation) |
| `recv!` | `(recv! sock . flag)` | Receive and deserialise a value, or `#f` on timeout |

Optional flag symbols:
- `'wait` — block indefinitely (no timeout).
- `'noblock` — return `#f` immediately if no message is ready.
- `'text` (send only) — use s-expression text format instead of binary.

Binary and text formats are auto-detected on receive.

```eta
;; Pair socket (inproc) example
(let ((server (nng-socket 'pair))
      (client (nng-socket 'pair)))
  (nng-listen server "inproc://demo")
  (nng-dial   client "inproc://demo")
  (send! client '(hello world))
  (display (recv! server))   ; => (hello world)
  (nng-close server)
  (nng-close client))
```

## High-level patterns (`std.net`)

### `with-socket`

```
(with-socket proto thunk)  =>  result of thunk
```

Creates a socket, runs `thunk` with it, and closes it via `dynamic-wind`
even on error or continuation escape.

```eta
(import std.net)

(with-socket 'req
  (lambda (sock)
    (nng-dial sock "tcp://localhost:5555")
    (send! sock '(compute 42))
    (recv! sock 'wait)))
```

### `request-reply`

```
(request-reply endpoint message)  =>  reply value
```

Opens a fresh REQ socket, dials `endpoint`, sends `message`, waits for one
reply, and closes the socket.

```eta
(import std.net)
(request-reply "tcp://localhost:5555" '(add 3 4))
; => 7
```

### `worker-pool`

```
(worker-pool module-path tasks)  =>  list of results
```

Spawns one child interpreter per task, sends each worker its task value,
collects one reply per worker in order.

The worker module must:
1. `(recv! (current-mailbox) 'wait)` — receive its task.
2. `(send! (current-mailbox) result 'wait)` — send its result back.

```eta
(import std.net)
(worker-pool "cookbook/concurrency/parallel-fib-worker.eta" '(30 32 34 36))
; => '(832040 2178309 5702887 14930352)
```

### `pub-sub`

```
(pub-sub listen-endpoint topics handler)  =>  does not return
```

Connects a SUB socket to `listen-endpoint`, subscribes to each topic in
`topics`, and calls `handler` on every received message.

```eta
(import std.net)
(pub-sub "tcp://localhost:5556"
         '("prices" "alerts")
         (lambda (msg) (display msg) (newline)))
```

### `survey`

```
(survey endpoint question timeout-ms)  =>  list of responses
```

Opens a SURVEYOR socket, listens on `endpoint`, sets a survey deadline of
`timeout-ms` ms, sends `question`, and collects all responses that arrive
before the deadline.

```eta
(import std.net)
(survey "tcp://*:5557" '(status?) 1000)
; => '((ok worker-1) (ok worker-2))   ; order may vary
```

## Actor model

| Primitive | Signature | Description |
|---|---|---|
| `spawn` | `(spawn module-path . args)` | Start a child interpreter process; returns a mailbox socket |
| `spawn-kill` | `(spawn-kill mailbox)` | Terminate a spawned child |
| `spawn-wait` | `(spawn-wait mailbox)` | Block until a spawned child exits; returns exit code |
| `current-mailbox` | `(current-mailbox)` | Return the child's own mailbox socket (in a worker) |
| `spawn-thread` | `(spawn-thread thunk)` | Run a zero-argument closure in a new in-process thread |
| `spawn-thread-with` | `(spawn-thread-with module func . args)` | In-process thread running a named function from a module |
| `thread-join` | `(thread-join mailbox)` | Join an in-process thread; returns exit code |
| `thread-alive?` | `(thread-alive? mailbox)` | Test whether an in-process thread is still running |

```eta
;; Spawn a worker and exchange one message
(let ((worker (spawn "my-worker.eta")))
  (send! worker '(task 42) 'wait)
  (let ((result (recv! worker 'wait)))
    (display result)
    (nng-close worker)))
```

## Supervision primitives

| Primitive | Signature | Description |
|---|---|---|
| `monitor` | `(monitor sock)` | Watch for peer disconnect; `recv!` returns `(down endpoint "disconnected")` |
| `demonitor` | `(demonitor sock)` | Cancel monitoring |
| `enable-heartbeat` | `(enable-heartbeat sock interval-ms)` | Periodic ping/pong liveness; timeout delivers `(down endpoint "heartbeat-timeout")` |

```eta
;; Monitor a connected pair socket
(let ((sock (nng-socket 'pair)))
  (nng-dial sock "tcp://localhost:6000")
  (monitor sock)
  (enable-heartbeat sock 2000)   ; ping every 2 s
  (let ((msg (recv! sock 'wait)))
    ;; msg may be (down "tcp://localhost:6000" "heartbeat-timeout")
    (display msg)))
```

### `nng-poll`

Checks a list of `(socket . _)` pairs non-blocking; buffers any available
messages and returns a list of ready sockets.

```eta
(let ((ready (nng-poll (list (cons s1 #t) (cons s2 #t)) 100)))
  (for-each (lambda (sock) (display (recv! sock))) ready))
```

## Cookbook examples

See the `cookbook/concurrency/` directory for complete runnable examples:

| File | Pattern |
|---|---|
| `echo-server.eta` / `echo-client.eta` | REQ/REP echo |
| `message-passing.eta` / `message-passing-worker.eta` | Actor message passing |
| `worker-pool.eta` / `worker-pool-worker.eta` | Parallel worker pool |
| `pub-sub.eta` / `pub-sub-publisher.eta` | Publish/subscribe |
| `scatter-gather.eta` / `scatter-gather-worker.eta` | Scatter-gather |
| `parallel-fib.eta` / `parallel-fib-worker.eta` | Parallel Fibonacci |
| `monte-carlo.eta` / `monte-carlo-worker.eta` | Monte Carlo Pi estimation |
| `inproc.eta` / `inproc-worker.eta` | In-process threads |
