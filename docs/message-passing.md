# Message Passing & Actors

[← Back to README](../README.md) · [Networking Primitives](networking.md) ·
[Network & Message Passing Design](network-message-passing.md) ·
[Modules & Stdlib](modules.md) · [Examples](examples.md)

---

> Erlang-style actor model for Eta: independent actors communicating
> through message passing over nng sockets.

---

## Overview

Eta's actor model is built on a simple principle: **share nothing, communicate
through messages**.  Each actor has its own VM, heap, and GC, and exchanges
data exclusively through serialized messages over nng sockets.

Actors come in two flavours:

| Primitive | Isolation | Transport | Use case |
|-----------|-----------|-----------|----------|
| `spawn` | Separate **OS process** | `ipc://` or `tcp://` | Heavy work, fault isolation, cross-host distribution |
| `spawn-thread` | In-process **thread** | `inproc://` | Low-latency, no fork/exec overhead, closures as workers |

Both return a PAIR socket and use the same `send!` / `recv!` /
`current-mailbox` API — code written for one works unchanged with the other.

> [!NOTE]
> `spawn` launches a child **process** (separate executable) connected over
> IPC or TCP, while `spawn-thread` runs a thunk in a new **in-process VM
> thread** over an `inproc://` socket.  The messaging API is identical —
> choose `spawn` for fault isolation and network distribution, or
> `spawn-thread` for minimal overhead when actors share a machine.

This gives you:

- **True parallelism** — actors run on separate cores simultaneously.
- **Fault isolation** — a crash in one actor cannot corrupt another's heap.
- **Network transparency** — the same `send!` / `recv!` API works whether
  actors are in the same machine or on different hosts.
- **No data races** — the design eliminates shared mutable state entirely.

```
┌───────────────────────┐     PAIR socket      ┌────────────────────────┐
│     Parent Process    │◄──────────────────►  │     Child Process      │
│                       │  ipc:// or tcp://    │                        │
│  VM₁  Heap₁  GC₁      │                      │  VM₂  Heap₂  GC₂       │
│                       │                      │                        │
│  (define w            │                      │  (define mailbox       │
│    (spawn "w.eta"))   │                      │    (current-mailbox))  │
│  (send! w '(task 42)) │                      │  (define msg           │
│  (recv! w 'wait)      │                      │    (recv! mailbox))    │
└───────────────────────┘                      └────────────────────────┘
```

---

## Primitive Reference

### `spawn`

```scheme
(spawn module-path)          → parent-side PAIR socket
(spawn module-path endpoint) → parent-side PAIR socket (custom endpoint)
```

Launches a child `etai` process that loads `module-path`.  The parent and
child are connected by a `PAIR` nng socket over an automatically-selected
IPC endpoint (Unix domain socket on Linux/macOS, named pipe on Windows).

Returns the **parent-side socket**, which is used for all subsequent
`send!` / `recv!` calls to the child.

```scheme
(define worker (spawn "examples/worker.eta"))
(send! worker '(compute 42))
(define result (recv! worker 'wait))
(nng-close worker)
```

**Child endpoint selection:**

| Platform | Default transport |
|----------|-------------------|
| Linux / macOS | `ipc:///tmp/eta-<pid>-<n>.sock` |
| Windows | `ipc://\\.\pipe\eta-<pid>-<n>` |

Override with the optional `endpoint` argument:
```scheme
(spawn "worker.eta" "tcp://*:6000")  ; parent listens on TCP port 6000
```

---

### `current-mailbox`

```scheme
(current-mailbox) → PAIR socket
```

Called **inside a spawned child module**, returns the PAIR socket
connected to the parent.  This is automatically bound when the child
is launched via `spawn`.

```scheme
;; worker.eta — runs inside the spawned child process
(module worker
  (import std.io)
  (begin
    (define mailbox (current-mailbox))
    (define task (recv! mailbox 'wait))  ; block until parent sends a task
    (send! mailbox (* task 2) 'wait)     ; send result back
    (println "worker: done")))
```

---

### `spawn-wait`

```scheme
(spawn-wait sock) → exit-code
```

Blocks until the child process associated with `sock` exits.  Returns
the process exit code (0 for clean exit, non-zero for error).

```scheme
(define worker (spawn "worker.eta"))
(send! worker '(work))
(define result (recv! worker 'wait))
(spawn-wait worker)   ; wait for clean exit
(nng-close worker)
```

---

### `spawn-kill`

```scheme
(spawn-kill sock) → void
```

Forcibly terminates the child process associated with `sock`.  The
child receives `SIGTERM` (Unix) or `TerminateProcess` (Windows).
Use this only when `(send! worker '(exit))` is not feasible.

```scheme
(spawn-kill worker)   ; forcibly terminate
(nng-close worker)    ; close the socket
```

---

## Lifecycle Management

| Event | Behavior |
|-------|----------|
| Parent calls `(nng-close worker)` | Socket closes; child's next `recv!` returns `#f` → child should exit cleanly |
| Parent crashes | OS closes the socket; child's `recv!` returns `#f` |
| Child crashes | Parent's `recv!` raises `'nng-error`; parent can handle it |
| Child exits normally | Parent's `recv!` returns `#f` on next call |

**Recommended pattern — clean shutdown:**

```scheme
;; Parent: signal exit, wait for child to finish
(send! worker '(exit))
(spawn-wait worker)
(nng-close worker)
```

**Recommended pattern — safe socket management:**

```scheme
(define worker (spawn "worker.eta"))
(dynamic-wind
  (lambda () #f)
  (lambda ()
    (send! worker '(task 42))
    (recv! worker 'wait))
  (lambda ()
    (nng-close worker)))  ; always close, even on exception
```

---

## Message Serialization

All values passed over `send!` / `recv!` are serialized to a binary wire
format.  The serialization is transparent — you send and receive ordinary
Eta values.

**Serializable types:**

| Type | Example |
|------|---------|
| Booleans | `#t`, `#f` |
| Fixnums | `42`, `-7` |
| Flonums | `3.14`, `1.0e10` |
| Characters | `#\a`, `#\space` |
| Strings | `"hello"` |
| Symbols | `'compute` |
| Pairs & lists | `'(a b c)`, `(cons 1 2)` |
| Vectors | `#(1 2 3)` |
| Bytevectors | `#u8(0 255 128)` |
| `'()` (nil) | The empty list |

**Not serializable:** Closures, continuations, ports, nng sockets, and
tensors.  These carry OS-level references that are meaningless in another
process.  Attempting to send one raises:
```
'nng-error "cannot send non-serializable value"
```

**Wire format auto-detection:**

```scheme
(send! sock value)         ; binary (default) — fast, compact
(send! sock value 'text)   ; s-expression text — human-readable, slower

(recv! sock)               ; auto-detects binary vs text
```

---

## Common Patterns

### Pattern 1 — Parent / Child (basic)

The simplest actor pattern: spawn one worker, send it a task, get back
a result.

```scheme
;; parent.eta
(module parent
  (import std.net)
  (import std.io)
  (begin
    (define worker (spawn "worker.eta"))
    (send! worker '(square 7))
    (define result (recv! worker 'wait))   ; => 49
    (println result)
    (send! worker '(exit))
    (spawn-wait worker)
    (nng-close worker)))
```

```scheme
;; worker.eta
(module worker
  (begin
    (define mailbox (current-mailbox))
    (letrec ((loop (lambda ()
                     (define msg (recv! mailbox 'wait))
                     (cond
                       ((equal? (car msg) 'square)
                        (send! mailbox (* (cadr msg) (cadr msg)) 'wait)
                        (loop))
                       ((equal? (car msg) 'exit) #f)
                       (#t (loop))))))
      (loop))))
```

---

### Pattern 2 — Request / Reply (synchronous RPC)

`request-reply` from `std.net` encapsulates a single synchronous
round-trip over a REQ/REP socket pair:

```scheme
;; Client (any .eta file or REPL)
(import std.net)
(define answer
  (request-reply "tcp://localhost:5555" '(compute 42)))
```

```scheme
;; echo-server.eta — standalone REP server
(module echo-server
  (import std.io)
  (begin
    (define sock (nng-socket 'rep))
    (nng-listen sock "tcp://*:5555")
    (println "echo-server: listening on tcp://*:5555")
    (letrec ((loop (lambda ()
                     (define msg (recv! sock 'wait))
                     (send! sock msg 'wait)          ; echo back unchanged
                     (loop))))
      (loop))))
```

Run the server:
```bash
etai examples/echo-server.eta
```

From another terminal or the REPL:
```scheme
(import std.net)
(request-reply "tcp://localhost:5555" '(hello world))
; => (hello world)
```

---

### Pattern 3 — Worker Pool (parallel map)

`worker-pool` spawns one child per task and collects results in parallel:

```scheme
(import std.net)

;; Square each number in parallel using 5 worker processes
(define results
  (worker-pool "worker.eta" '(1 2 3 4 5)))
; => (1 4 9 16 25)  (if worker squares its input)
```

See [`examples/worker-pool.eta`](../examples/worker-pool.eta) and
[`examples/parallel-map.eta`](../examples/parallel-map.eta) for
complete runnable demos.

---

### Pattern 4 — Publish / Subscribe

```scheme
;; publisher.eta — sends price updates every second
(module publisher
  (begin
    (define pub (nng-socket 'pub))
    (nng-listen pub "tcp://*:5556")
    (letrec ((broadcast (lambda (n)
                          (send! pub (list 'prices.eur n))
                          (send! pub (list 'prices.usd (* n 1.1)))
                          (broadcast (+ n 1)))))
      (broadcast 100))))
```

```scheme
;; subscriber.eta — receives price updates
(module subscriber
  (import std.net)
  (import std.io)
  (begin
    (define sub (nng-socket 'sub))
    (nng-dial sub "tcp://localhost:5556")
    (nng-subscribe sub "prices.")          ; filter: only price messages
    (letrec ((loop (lambda ()
                     (define msg (recv! sub 'wait))
                     (when msg (println msg) (loop)))))
      (loop))))
```

Or use `pub-sub` from `std.net`:

```scheme
(pub-sub "tcp://localhost:5556"
         '("prices.")
         (lambda (msg) (println msg)))
```

---

### Pattern 5 — Scatter / Gather (Survey)

Ask multiple workers a question and collect all their answers before a
deadline:

```scheme
;; respondent-worker.eta — answers status queries
(module respondent
  (begin
    (define mailbox (current-mailbox))
    ;; A respondent must connect to the surveyor
    (define surveyor (nng-socket 'respondent))
    (nng-dial surveyor "tcp://localhost:5557")
    (letrec ((loop (lambda ()
                     (define question (recv! surveyor 'wait))
                     (when question
                       (send! surveyor (list 'ok (hostname)) 'wait)
                       (loop)))))
      (loop))))
```

```scheme
;; scatter-gather.eta — collect status from all respondents
(import std.net)
(define responses
  (survey "tcp://*:5557" '(status?) 1000))
; => ((ok worker-1) (ok worker-2) ...)
```

See [`examples/scatter-gather.eta`](../examples/scatter-gather.eta) for a
complete runnable demo.

---

### Pattern 6 — Event Loop with `nng-poll`

When a process manages multiple sockets simultaneously, `nng-poll`
prevents blocking on any single socket:

```scheme
(define commands (nng-socket 'pull))
(define events   (nng-socket 'sub))
(nng-listen commands "ipc:///tmp/cmds.sock")
(nng-dial   events   "tcp://localhost:5556")
(nng-subscribe events "")

(letrec ((loop (lambda ()
                 ;; Wait up to 100 ms for any socket to become readable
                 (define ready
                   (nng-poll (list (cons commands 'recv)
                                   (cons events 'recv))
                             100))
                 ;; Process all ready sockets
                 (for-each
                   (lambda (sock)
                     (define msg (recv! sock 'noblock))
                     (when msg (dispatch sock msg)))
                   ready)
                 (loop))))
  (loop))
```

---

### Pattern 7 — In-Process Threads (`spawn-thread`)


`spawn-thread` is a lightweight alternative to `spawn` for actors that live
inside the same OS process.  There is no fork/exec overhead; the thunk's
bytecode and upvalues are serialized and executed in a fresh in-process VM
thread connected over an `inproc://` PAIR socket.  The API is identical to
`spawn` / `current-mailbox`.

```scheme
;; A worker that captures an offset in its closure.
;; No separate worker file needed.
(define (make-worker offset)
  (spawn-thread
    (lambda ()
      (let ((mb (current-mailbox)))          ; thread-side PAIR socket
        (let ((n (recv! mb 'wait)))
          (send! mb (+ n offset) 'wait))))))

(define t1 (make-worker 10))
(define t2 (make-worker 20))
(define t3 (make-worker 30))

(send! t1 5 'wait)  (send! t2 5 'wait)  (send! t3 5 'wait)
(recv! t1 'wait)    ; => 15
(recv! t2 'wait)    ; => 25
(recv! t3 'wait)    ; => 35

(thread-join t1) (thread-join t2) (thread-join t3)
(nng-close t1)   (nng-close t2)   (nng-close t3)
```

See [`examples/inproc.eta`](../examples/inproc.eta) for the runnable demo.

#### Thread lifecycle

| Event | Effect |
|-------|--------|
| Parent calls `(nng-close sock)` | Socket closes; thread's next `recv!` returns `#f` → thread should exit |
| Thread thunk returns normally | Thread exits; parent's `recv!` returns `#f` on next call |
| Thread raises an unhandled exception | Thread exits; parent's next `recv!` raises `'nng-error` |
| `(thread-alive? sock)` | `#t` while running, `#f` after exit |
| `(thread-join sock)` | Blocks until thread exits; returns `0` |

#### Constraints

- The thunk must take **0 arguments**.
- All captured **upvalues must be serializable** (numbers, strings, symbols,
  booleans, pairs, lists, vectors).  Closures, ports, sockets, and tensors
  are not serializable — keep those in a separate worker file and use
  `spawn-thread-with` instead.

---

## Cross-Host Messaging

The same `send!` / `recv!` API works across machines — just use `tcp://`
endpoints:

```scheme
;; ── Machine A (server) ────────────────────────────────────────────────
(define server (nng-socket 'rep))
(nng-listen server "tcp://*:6000")
(define msg (recv! server 'wait))
(send! server (process msg) 'wait)

;; ── Machine B (client) ────────────────────────────────────────────────
(define client (nng-socket 'req))
(nng-dial client "tcp://10.0.0.1:6000")   ; replace with actual server IP
(send! client '(compute 42))
(define result (recv! client 'wait))
```

See [`examples/distributed-compute.eta`](../examples/distributed-compute.eta)
for a complete two-process demonstration.

---

## Interaction with `dynamic-wind` and Continuations

nng sockets are heap objects — like file ports.  They are **not** captured
or rewound by continuations.  `call/cc` in the middle of a send/receive
sequence does not replay or undo I/O.

**Socket cleanup with `dynamic-wind`:**

```scheme
;; The after-thunk always runs — even if an exception escapes the body
;; or a continuation is invoked.
(let ((sock #f))
  (dynamic-wind
    (lambda () (set! sock (nng-socket 'req)))
    (lambda ()
      (nng-dial sock "tcp://localhost:5555")
      (send! sock msg)
      (recv! sock))
    (lambda ()
      (when sock (nng-close sock)))))
```

The `with-socket` helper from `std.net` encapsulates this pattern.

---

## Timeouts and Blocking

> **Critical:** Eta's VM is single-threaded.  A blocking `recv!` freezes
> the REPL, LSP, and all `dynamic-wind` cleanup.

| Strategy | Code | When to Use |
|----------|------|-------------|
| Default timeout (1 s) | `(recv! sock)` | Most cases — returns `#f` on timeout |
| Non-blocking | `(recv! sock 'noblock)` | In event loops, polling |
| Indefinite block | `(recv! sock 'wait)` | Only when a reply is guaranteed |
| Multi-socket poll | `(nng-poll items 100)` | Monitoring multiple sockets |

Change the default timeout per socket:
```scheme
(nng-set-option sock 'recv-timeout 5000)  ; 5 s
(nng-set-option sock 'recv-timeout -1)    ; infinite (same as 'wait)
```

---

## See Also

- [Networking Primitives](networking.md) — Complete nng primitive reference
- [Network & Message Passing Design](network-message-passing.md) — Architecture and implementation roadmap
- [`std.net` module](modules.md#stdnet--networking--message-passing) — High-level helpers
- [Examples](examples.md#networking--message-passing) — Runnable demos

