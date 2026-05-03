# Networking — nng Socket Primitives

[← Back to README](../../../README.md) · [Message Passing & Actors](message-passing.md) ·
[Network & Message Passing Design](network-message-passing.md) ·
[Modules & Stdlib](modules.md)

---

> nng (nanomsg-next-generation) socket primitives for Eta.
> nng is a required dependency, fetched automatically at build time.

---

## Overview

Eta's networking layer is built on **nng** — a lightweight, MIT-licensed
messaging library that implements the SP (Scalability Protocols)
specification.  All socket operations are exposed as Eta builtins and
wrapped in the `(std net)` standard library module.

Every nng socket is a first-class heap object.  It is reference-counted by
the GC and closed automatically when it is no longer reachable.  The
`with-socket` helper (from `std.net`) is the recommended way to manage
socket lifetimes explicitly via `dynamic-wind`.

```scheme
(import std.net)

;; Low-level: open, communicate, close manually
(define sock (nng-socket 'req))
(nng-dial sock "tcp://localhost:5555")
(send! sock '(hello))
(define reply (recv! sock))
(nng-close sock)

;; High-level: with-socket handles cleanup automatically
(with-socket 'req
  (lambda (sock)
    (nng-dial sock "tcp://localhost:5555")
    (send! sock '(hello))
    (recv! sock)))
```

---

## Quick Reference

| Primitive | Signature | Description |
|-----------|-----------|-------------|
| [`nng-socket`](#nng-socket) | `(nng-socket type-sym)` | Create a socket |
| [`nng-listen`](#nng-listen) | `(nng-listen sock endpoint)` | Bind and listen on an endpoint |
| [`nng-dial`](#nng-dial) | `(nng-dial sock endpoint)` | Connect to an endpoint |
| [`nng-close`](#nng-close) | `(nng-close sock)` | Close the socket |
| [`nng-socket?`](#nng-socket-1) | `(nng-socket? x)` | Predicate — is `x` an nng socket? |
| [`send!`](#send) | `(send! sock value [flag])` | Serialize and send a value |
| [`recv!`](#recv) | `(recv! sock [flag])` | Receive and deserialize a value |
| [`nng-poll`](#nng-poll) | `(nng-poll items timeout-ms)` | Poll multiple sockets |
| [`nng-subscribe`](#nng-subscribe) | `(nng-subscribe sock topic)` | Set SUB topic filter |
| [`nng-set-option`](#nng-set-option) | `(nng-set-option sock option value)` | Set socket option |
| [`spawn-thread`](#spawn-thread) | `(spawn-thread thunk)` | Launch a lambda as an in-process actor thread |
| [`spawn-thread-with`](#spawn-thread-with) | `(spawn-thread-with path fn-sym args…)` | Launch a named function from a source file as an in-process thread |
| [`thread-join`](#thread-join) | `(thread-join sock)` | Block until the thread completes |
| [`thread-alive?`](#thread-alive) | `(thread-alive? sock)` | Returns `#t` if the thread is still running |


---


## Socket Protocols

`nng-socket` takes a symbol identifying the protocol:

| Symbol | Protocol | Pattern | Notes |
|--------|----------|---------|-------|
| `'pair` | PAIR | 1-to-1 bidirectional | Default for `spawn`; guaranteed delivery, total ordering |
| `'req` | REQ | Client in request-reply | Strict lock-step: send one request, receive one reply |
| `'rep` | REP | Server in request-reply | Receives request, sends reply; one request in flight at a time |
| `'pub` | PUB | Publisher in pub-sub | Best-effort broadcast; slow subscribers may drop messages |
| `'sub` | SUB | Subscriber in pub-sub | Must call `nng-subscribe` with a topic prefix; default accepts nothing |
| `'push` | PUSH | Sender in pipeline | Fan-out: round-robin distribution to connected PULL sockets |
| `'pull` | PULL | Receiver in pipeline | Receives from PUSH; guaranteed delivery per sender, FIFO per sender |
| `'surveyor` | SURVEYOR | Scatter in scatter-gather | Sends a question; collects responses until a deadline |
| `'respondent` | RESPONDENT | Gather in scatter-gather | Receives question, sends reply; must reply before deadline |
| `'bus` | BUS | Many-to-many | Every peer sees every other peer's messages; best-effort |

**Rule of thumb:** Use `'pair` (via `spawn`) for actor-to-actor communication.
Use `'req`/`'rep` for synchronous RPC.  Use `'pub`/`'sub` only when
message loss is acceptable (telemetry, logging, broadcast).  Use
`'push`/`'pull` for work-distribution pipelines.  Use
`'surveyor`/`'respondent` for scatter-gather (e.g. "which workers are
ready?").  Use `'bus` for peer coordination and gossip protocols.

---

## Endpoints

An **endpoint** is a URL string specifying the transport and address:

| Transport | Format | Description |
|-----------|--------|-------------|
| `tcp://` | `tcp://<host>:<port>` | TCP/IP — works across machines |
| `ipc://` | `ipc:///tmp/<name>.sock` (Unix) or `ipc://\\.\pipe\<name>` (Windows) | IPC — fastest local communication; named pipes on Windows |
| `inproc://` | `inproc://<name>` | In-process — zero-copy between threads |

When **listening**, use `*` as the host to bind all interfaces:
```scheme
(nng-listen sock "tcp://*:5555")    ; bind to port 5555 on all interfaces
```

When **dialing**, specify the actual host:
```scheme
(nng-dial sock "tcp://localhost:5555")   ; connect to localhost
(nng-dial sock "tcp://10.0.0.5:5555")   ; connect to a remote host
```

---

## Primitive Reference

### `nng-socket`

```scheme
(nng-socket type-symbol) → socket
```

Creates and returns a new nng socket of the given protocol type.  The
socket is opened via the appropriate nng protocol function (e.g.
`nng_pair0_open`, `nng_req0_open`).

A default **receive timeout of 1 000 ms** is set on every newly created
socket.  This prevents `recv!` from blocking the VM indefinitely.  Use
`nng-set-option` or the `'wait` flag to override.

```scheme
(define sock (nng-socket 'pair))
(define req  (nng-socket 'req))
(define sub  (nng-socket 'sub))
```

**Errors:** Raises `'nng-error` if the socket cannot be created.

---

### `nng-listen`

```scheme
(nng-listen sock endpoint) → void
```

Binds the socket to `endpoint` and begins listening for incoming
connections.  The call returns immediately; the socket accepts connections
in the background.

```scheme
(define sock (nng-socket 'rep))
(nng-listen sock "tcp://*:5555")       ; listen on all interfaces, port 5555
(nng-listen sock "ipc:///tmp/eta.sock")  ; Unix domain socket
```

**Note:** A socket can only listen on one endpoint at a time with most
protocols.  Call `nng-listen` before calling `recv!`.

**Errors:** Raises `'nng-error` (e.g. `NNG_EADDRINUSE`) if the port is
already in use.

---

### `nng-dial`

```scheme
(nng-dial sock endpoint) → void
```

Connects the socket to `endpoint`.  If the peer is not yet listening,
nng will retry the connection automatically (exponential back-off).  The
call returns once the connection is established.

```scheme
(define sock (nng-socket 'req))
(nng-dial sock "tcp://localhost:5555")
```

**Errors:** Raises `'nng-error` on a connection failure that is not
retryable (e.g. invalid endpoint format).

---

### `nng-close`

```scheme
(nng-close sock) → void
```

Closes the socket and releases all associated OS resources.  Idempotent:
calling `nng-close` on an already-closed socket is a no-op.

The socket is also closed automatically when it is GC'd, but explicit
closure is recommended for prompt resource release:

```scheme
(nng-close sock)
```

Use `with-socket` from `std.net` for automatic cleanup via `dynamic-wind`:

```scheme
(with-socket 'req
  (lambda (sock)
    (nng-dial sock "tcp://localhost:5555")
    (send! sock msg)
    (recv! sock)))
;; sock is closed here even if an exception is raised
```

---

### `nng-socket?`

```scheme
(nng-socket? x) → boolean
```

Returns `#t` if `x` is an nng socket object, `#f` otherwise.

```scheme
(nng-socket? (nng-socket 'pair))  ; => #t
(nng-socket? "not a socket")      ; => #f
```

---

### `send!`

```scheme
(send! sock value)         ; binary format (default), 1 s timeout
(send! sock value 'text)   ; s-expression (human-readable) format
(send! sock value 'noblock) ; non-blocking — returns #f if EAGAIN
(send! sock value 'wait)   ; block indefinitely (no timeout)
```

Serializes `value` to a wire-format byte buffer and sends it as a single
nng message frame.

**Wire formats:**
- **Binary (default):** Compact encoding derived from the `.etac` bytecode
  constant format.  Fast and space-efficient for all serializable types.
  Binary messages begin with the magic byte `0xEA`.
- **Text (`'text` flag):** S-expression string.  Human-readable and
  debuggable, but slower for large data.

**Serializable types:** `#f`, `#t`, `'()`, fixnums, flonums, characters,
strings, symbols, pairs, lists, vectors, and bytevectors.

**Non-serializable types:** Closures, continuations, ports, tensors, `Tape`,
and `TapeRef`.

`Tape` and `TapeRef` are VM-local AAD values. Sending them raises
`:ad/cross-vm-ref` so callers can catch and handle the AD boundary violation.
Other non-serializable values raise the standard nng serialization error.

```scheme
(send! sock 42)                     ; send a fixnum
(send! sock '(hello world))        ; send a list
(send! sock "binary data" 'text)   ; send as s-expression text
```

**Returns:** `#t` on success.  In `'noblock` mode, returns `#f` if the
send buffer is full.

**Errors:** Raises `'nng-error` on socket errors.

---

### `recv!`

```scheme
(recv! sock)             ; block up to 1 s (default timeout), return value or #f
(recv! sock 'noblock)    ; non-blocking — return value or #f immediately
(recv! sock 'wait)       ; block indefinitely until a message arrives
```

Receives a single nng message frame, auto-detects the wire format (binary
vs. text), deserializes it, and returns the Eta value.

Returns `#f` on **timeout** (default 1 000 ms) or when no message is
available in `'noblock` mode.  This allows polling without blocking the VM:

```scheme
(define msg (recv! sock))
(when msg
  (process msg))
```

**Auto-detection:** Binary messages start with `0xEA`; text messages start
with a printable character.

**Errors:** Raises `'nng-error` on socket errors.  Returns `#f` on timeout
(does not raise — check return value).

> **Warning:** Blocking `recv!` freezes the entire VM.  Use the default
> 1 s timeout, `'noblock`, or `nng-poll` for non-blocking idioms.

---

### `nng-poll`

```scheme
(nng-poll items timeout-ms) → list of ready sockets
```

Polls multiple sockets simultaneously and returns the list of sockets that
have messages available.  `items` is a list of `(socket . events)` pairs
where `events` is a symbol or list of symbols (e.g. `'recv`, `'send`).
`timeout-ms` is the maximum wait in milliseconds (`0` = check immediately,
`-1` = wait forever).

```scheme
(define ready
  (nng-poll (list (cons sock-a 'recv)
                  (cons sock-b 'recv))
            100))  ; wait up to 100 ms

(for-each (lambda (sock)
            (define msg (recv! sock 'noblock))
            (when msg (handle msg)))
          ready)
```

`nng-poll` is the preferred way to drive an **event loop** that monitors
multiple sockets without blocking the VM on any single socket.

---

### `nng-subscribe`

```scheme
(nng-subscribe sock topic-string) → void
```

Sets a topic filter on a `'sub` socket.  Only messages whose payload
begins with `topic-string` (as a byte prefix) will be delivered.  Call
with an empty string `""` to receive all messages:

```scheme
(define sub (nng-socket 'sub))
(nng-dial sub "tcp://localhost:5556")
(nng-subscribe sub "prices.")   ; receive only messages starting with "prices."
(nng-subscribe sub "alerts.")   ; also receive "alerts." messages
;; (nng-subscribe sub "")       ; receive everything
```

A freshly created `'sub` socket has **no subscriptions** — it discards
all messages until at least one subscription is added.

---

### `nng-set-option`

```scheme
(nng-set-option sock option value) → void
```

Sets a socket-level option.  Common options:

| Option symbol | Type | Default | Description |
|---------------|------|---------|-------------|
| `'recv-timeout` | integer (ms) | `1000` | Receive timeout. `-1` = infinite. |
| `'send-timeout` | integer (ms) | unlimited | Send timeout. `-1` = infinite. |
| `'recv-buf-size` | integer | protocol-default | Receive buffer depth (messages) |
| `'send-buf-size` | integer | protocol-default | Send buffer depth (messages) |
| `'survey-time` | integer (ms) | 1000 | SURVEYOR deadline for collecting responses |

```scheme
;; Increase receive timeout to 5 s
(nng-set-option sock 'recv-timeout 5000)

;; Disable timeout — block until a message arrives (use with care)
(nng-set-option sock 'recv-timeout -1)

;; Set SURVEYOR deadline to 2 s
(nng-set-option surveyor 'survey-time 2000)
```

---

## In-Process Actor Threads

`spawn-thread` and `spawn-thread-with` launch independent **in-process threads**
— each with its own VM, heap, and GC — connected to the parent via an
`inproc://` PAIR socket.  They are cheaper than OS-process `spawn` (no
fork/exec) and support upvalue capture, but upvalues must be serializable.

### `spawn-thread`

```scheme
(spawn-thread thunk) → socket
```

Serializes the zero-argument closure `thunk` (its bytecode and all captured
upvalues) and launches it in a fresh thread VM.  Returns the **parent-side
PAIR socket**, which is used for all subsequent `send!` / `recv!` calls.

Inside the thunk, `(current-mailbox)` returns the thread's end of the socket.

```scheme
;; Capture an offset as an upvalue — runs in its own thread VM
(define (make-worker offset)
  (spawn-thread
    (lambda ()
      (let ((mb (current-mailbox)))
        (let ((n (recv! mb 'wait)))
          (send! mb (+ n offset) 'wait))))))

(define t (make-worker 7))
(send! t 35 'wait)
(recv! t 'wait)         ; => 42
(thread-join t)
(nng-close t)
```

`spawn-thread` transfers a full capture set for the thunk: direct upvalues,
nested closures, and referenced module-global values used by the closure
bytecode.  This allows helper lambdas and module-level immutable data to run
in the child VM without moving worker code to a separate file.

Supported captured values include numbers, strings, symbols, booleans, pairs,
lists, vectors, bytevectors, and closures. Runtime-only handles such as ports,
sockets, tensors, and continuations remain non-transferable. Nested values are
validated recursively, and failures report the specific capture root
(`upvalue[...]` or `global[...]`) plus the unsupported heap-object kind.

**Errors:**
- `'runtime-error "spawn-thread: argument must be a 0-argument closure (thunk)"` —
  argument is not a thunk.
- `'runtime-error "spawn-thread: process manager not configured"` — built
  without nng support.

---

### `spawn-thread-with`

```scheme
(spawn-thread-with module-path func-sym args...) → socket
```

Loads `module-path` in a fresh thread VM (the same way `spawn` loads a child
process), then calls the named function `func-sym` with any additional `args`.
Returns the **parent-side PAIR socket**.

Use this when you prefer to keep worker logic in a separate source file, or
when the thunk intentionally depends on non-transferable runtime handles.

```scheme
;; inproc-worker.eta defines a (noop) stub; real work is in module body.
(define t (spawn-thread-with "cookbook/concurrency/inproc-worker.eta" 'noop))
(send! t 42 'wait)
(recv! t 'wait)
(thread-join t)
(nng-close t)
```

---

### `thread-join`

```scheme
(thread-join sock) → 0
```

Blocks until the thread associated with `sock` has finished.  Returns `0`
on success.  Raises a type error if `sock` is not a thread socket.

---

### `thread-alive?`

```scheme
(thread-alive? sock) → boolean
```

Returns `#t` if the thread is still running, `#f` once it has exited.

```scheme
(define t (spawn-thread (lambda () (thread-sleep! 200))))
(thread-alive? t)   ; => #t  (while sleeping)
(thread-join  t)
(thread-alive? t)   ; => #f
(nng-close t)
```

**Tip:** prefer `thread-join` over a polling loop on `thread-alive?` — joining
is O(1) and does not spin.

---

## Error Handling

All nng errors are mapped to Eta exceptions with the tag `'nng-error`.
The raised value is a string from `nng_strerror()`.

```scheme
(catch 'nng-error
  (let ((sock (nng-socket 'req)))
    (nng-dial sock "tcp://localhost:5555")
    (send! sock '(hello))
    (recv! sock 'wait)))
;; => "connection refused" (or the reply value on success)
```

Use `guard` (or `catch`) around socket operations that may fail:

```scheme
(define (safe-request endpoint msg)
  (catch 'nng-error
    (request-reply endpoint msg)))
```

---

## Transport Comparison

| Transport | Bandwidth | Latency | Scope | Notes |
|-----------|-----------|---------|-------|-------|
| `inproc://` | Highest | ~0 µs | Same process | Zero-copy (no serialization needed for same-process thread actors) |
| `ipc://` | High | ~1–10 µs | Same machine | Unix domain sockets (Linux/macOS) or named pipes (Windows) |
| `tcp://` | Medium | ~50–500 µs | Any network | Works across machines; requires explicit IP/port routing |

---

## `std.net` — High-Level Patterns

The `std.net` module wraps the raw primitives in ergonomic helpers:

```scheme
(import std.net)
```

| Function | Description |
|----------|-------------|
| `with-socket` | Create a socket with `dynamic-wind` cleanup |
| `request-reply` | One-shot synchronous RPC: open REQ, send, receive, close |
| `worker-pool` | Spawn N workers, distribute tasks, collect results |
| `pub-sub` | Subscribe and drive an event loop |
| `survey` | Scatter-gather: broadcast a question, collect all replies |

See [Message Passing & Actors](message-passing.md) for usage examples and
the full actor model guide.

---

## See Also

- [Message Passing & Actors](message-passing.md) — Actor model, `spawn`, worker patterns
- [Network & Message Passing Design](network-message-passing.md) — Full design document
- [Modules & Stdlib](modules.md) — `std.net` module reference
- [Examples](../examples-tour.md#concurrency) — Runnable code samples

