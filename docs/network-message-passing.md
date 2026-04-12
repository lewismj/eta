# Network & Message-Passing Parallelism

[← Back to README](../README.md) · [Networking Primitives](networking.md) ·
[Message Passing & Actors](message-passing.md) · [Modules & Stdlib](modules.md) ·
[Next Steps](next-steps.md)

---

> Erlang-style message passing for Eta, powered by nng (nanomsg-next-generation).

## Quick Links

| Goal | Go to |
|------|-------|
| Primitive API reference (`nng-socket`, `send!`, `recv!`, …) | [Networking Primitives](networking.md) |
| Actor patterns and worked examples | [Message Passing & Actors](message-passing.md) |
| `std.net` module functions | [Modules & Stdlib — std.net](modules.md#stdnet--networking--message-passing) |
| Example programs | [Language Guide — Networking](examples.md#networking--message-passing) |

---

## Motivation

Eta's VM is intentionally **single-threaded**: the interpreter loop, GC,
value stack, call-frame stack, winding stack, catch stack, and trail stack
are all owned by one thread.  Making every data structure thread-safe
would add synchronisation overhead to every instruction and introduce
subtle bugs around continuations and `dynamic-wind`.

The alternative — the **process model** — gives true parallelism without
shared state:

- Each Eta actor is an independent OS process (or OS thread) with its own
  heap, GC, and stack.
- Actors communicate exclusively through **message passing** over nng sockets.
- Because nothing is shared, there are no data races, no lock contention,
  and no GC pauses in one actor due to another actor's allocation.

This is Erlang's model applied to a Scheme VM.

---

## Why nng?

nng (nanomsg-next-generation) was chosen over alternatives for several
concrete reasons:

| Criterion | nng | ZeroMQ |
|-----------|-----|--------|
| **License** | MIT | MPL-2.0 (libzmq) + MIT (cppzmq) |
| **Binary size** | ~200 KB static | ~400 KB + C++ wrapper |
| **Build** | Single `FetchContent` CMake target | Two repos; DLL copying on Windows |
| **Global context** | None — sockets are standalone | `zmq_ctx_t` per process |
| **Thread safety** | Sockets thread-safe by default | One socket per thread |
| **Windows IPC** | First-class (`ipc://` via named pipes) | Not supported; must use TCP loopback |
| **Async I/O** | Built-in `nng_aio` (completion-based) | `zmq_poll` (readiness-based) |

The absence of a global context object is particularly important: there
is nothing to create at startup or destroy at exit, which eliminates a
class of lifecycle bugs common in ZeroMQ applications.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                   Eta Source (std.net)                   │
│                                                          │
│  (spawn "worker.eta")   →  parent-side PAIR socket       │
│  (send! sock '(task 42))                                 │
│  (recv! sock 'wait)                                      │
│  (worker-pool "w.eta" '(1 2 3))                          │
└──────────────────────┬───────────────────────────────────┘
                       │  stdlib/std/net.eta
                       ▼
┌──────────────────────────────────────────────────────────┐
│              eta/nng/ — C++ Primitive Layer              │
│                                                          │
│  NngSocketPtr     — GC-managed heap object               │
│  Wire format      — LispVal ↔ binary / s-expression      │
│  nng_primitives   — register_nng_primitives(env, …)      │
│  ProcessManager   — spawn / wait / kill child processes  │
└──────────────────────┬───────────────────────────────────┘
                       │  links against
                       ▼
┌──────────────────────────────────────────────────────────┐
│         libnng (fetched via CMake FetchContent)          │
└──────────────────────────────────────────────────────────┘
```

**Three layers:**

1. **Eta stdlib** (`std/net.eta`) — high-level ergonomic patterns:
   `with-socket`, `request-reply`, `worker-pool`, `pub-sub`, `survey`.

2. **C++ primitive layer** (`eta/nng/`) — registers low-level builtins
   into the VM's global slot table: `nng-socket`, `nng-listen`, `nng-dial`,
   `send!`, `recv!`, `spawn`, `current-mailbox`, etc.  Manages the
   `NngSocketPtr` heap object, its GC destructor, and child process
   lifecycle via `ProcessManager`.

3. **libnng** — the OS-level socket library. Eta code never calls nng
   directly; everything goes through the primitive layer.

---

## Socket Protocols

nng implements the Scalability Protocols (SP) specification.  Each protocol
encodes a distinct communication pattern.  The protocol is chosen at socket
creation time with a symbol:

| Symbol | Pattern | Ordering | Delivery |
|--------|---------|----------|----------|
| `'pair` | 1-to-1 bidirectional | Total order | Guaranteed |
| `'req` / `'rep` | Request-reply (lock-step) | Per-sender FIFO | Guaranteed |
| `'push` / `'pull` | Pipeline fan-out | Per-sender FIFO | Guaranteed |
| `'pub` / `'sub` | Broadcast | Per-publisher FIFO | **Best-effort** |
| `'surveyor` / `'respondent` | Scatter-gather | Per-respondent | **Best-effort** (deadline) |
| `'bus` | Many-to-many gossip | Per-sender FIFO | **Best-effort** |

`'pair` is used internally by `spawn` — each spawned child gets a PAIR
socket as its mailbox, giving the actor model total ordering and guaranteed
delivery for free.

For the full primitive reference (`nng-socket`, `nng-listen`, `nng-dial`,
`nng-subscribe`, `nng-set-option`, endpoint formats, error handling) see
[Networking Primitives](networking.md).

---

## The Actor Model

### `spawn` and `current-mailbox`

```
 Parent Process                         Child Process
 ─────────────────────                 ──────────────────────
 (spawn "worker.eta")                  ;; child loads worker.eta
 → parent-side PAIR socket  ◄────────► (current-mailbox) → PAIR socket
 (send! worker '(task 42))            (recv! mailbox 'wait)
 (recv! worker 'wait)                 (send! mailbox result 'wait)
```

`spawn` does three things atomically:
1. Creates a PAIR socket in the parent and listens on an auto-assigned
   IPC endpoint (Unix domain socket on Linux/macOS, named pipe on Windows).
2. Launches a child `etai` process with the endpoint passed via `--mailbox`.
3. Returns the parent-side socket as the handle for all subsequent
   `send!` / `recv!` calls.

The child's `(current-mailbox)` returns its PAIR socket already dialled
and connected.  The two processes are immediately ready to exchange messages.

### Message Lifecycle

```
send! (parent)              recv! (child)
   │                            │
   │── serialize to bytes ──────►│── deserialize from bytes ──► LispVal
   │   binary wire frame         │                               on child's heap
```

Messages are **copied** — there is no shared memory.  The serialization
step is the only overhead compared to in-process function calls, but it
provides complete isolation: a bug in the child cannot corrupt the parent's
heap.

### Lifecycle and Error Model

| Event | Effect |
|-------|--------|
| Parent calls `(nng-close sock)` | Socket closes; child's next `recv!` returns `#f` |
| Parent crashes | OS closes socket; child's `recv!` returns `#f` |
| Child crashes | Parent's `recv!` raises `'nng-error` |
| Child exits cleanly | Parent's `recv!` returns `#f` on next call |

Clean shutdown pattern:
```scheme
(send! worker '(exit))
(spawn-wait worker)
(nng-close worker)
```

---

## Wire Format

All values exchanged over `send!` / `recv!` are serialized to a byte buffer.

**Binary (default):** Compact encoding derived from the `.etac` bytecode
constant scheme.  Messages begin with the magic byte `0xEA`.  Fast and
space-efficient for all data types.

**Text (`'text` flag):** S-expression string.  Human-readable for debugging.

`recv!` **auto-detects** the format — no configuration required on the
receiving end.

**Serializable types:** booleans, fixnums, flonums, characters, strings,
symbols, pairs, lists, vectors, bytevectors.

**Not serializable:** closures, continuations, ports, nng sockets, tensors.
These contain OS-level references or code pointers that are meaningless in
another process.  Attempting to send one raises
`'nng-error "cannot send non-serializable value"`.

---

## Interaction with Continuations and `dynamic-wind`

nng sockets are heap objects — exactly like file ports.  They are
**not captured or replayed** by `call/cc`:

- Re-invoking a continuation does not replay or undo messages already
  sent or received.  This matches Erlang semantics and common sense for I/O.

- `dynamic-wind` after-thunks run on any exit from a region — including
  exception escapes and continuation invocations — making them reliable
  for socket cleanup.  The `with-socket` helper in `std.net` packages
  this pattern.

- Spawned processes are fully independent — a parent's continuations
  cannot reach into a child's VM.  Communication is exclusively through
  messages.

---

## Default Timeout and Single-Threaded Safety

> **Critical constraint:** Eta's VM is single-threaded.  A `recv!` that
> blocks indefinitely freezes the entire VM — the REPL, the LSP server,
> and `dynamic-wind` cleanup code all stop.

Every newly created socket has a **1 000 ms receive timeout** by default.
`recv!` returns `#f` on timeout rather than blocking forever.

```scheme
(recv! sock)            ; up to 1 s → value or #f on timeout
(recv! sock 'wait)      ; indefinite block — use only when reply is guaranteed
(recv! sock 'noblock)   ; immediate → value or #f

(nng-set-option sock 'recv-timeout 5000)  ; change to 5 s
(nng-set-option sock 'recv-timeout -1)    ; infinite (same as 'wait)
```

For monitoring multiple sockets without blocking any one of them,
use `nng-poll` — it waits on a set of sockets and returns only those
with messages ready.

---

## Cross-Host Transparency

The same `send!` / `recv!` API works over any transport.  Changing from
local IPC to TCP is a one-line endpoint change:

```scheme
;; Local IPC (same machine, fastest)
(nng-listen sock "ipc:///tmp/eta-worker.sock")

;; TCP (any host on the network)
(nng-listen sock "tcp://*:6000")
(nng-dial   sock "tcp://10.0.0.5:6000")
```

`spawn` uses IPC automatically for local children.  For cross-host actors,
start `etai` independently on the remote machine and connect via TCP — the
Eta code on both sides is identical.

---

## Scope and Limitations

### In scope

- All ten nng socket protocols across IPC, TCP, and inproc transports
- Linux, macOS, and Windows (IPC via named pipes on Windows)
- `spawn` for local child processes; `current-mailbox` in the child
- Binary and text wire formats with auto-detection on receive
- `std.net` high-level helpers: `with-socket`, `request-reply`,
  `worker-pool`, `pub-sub`, `survey`
- VS Code extension: syntax highlighting, snippets, and DAP child process tree view

### Out of scope (post-v1)

| Feature | Rationale |
|---------|-----------|
| **Remote `spawn-remote`** | Requires SSH integration or a distributed node agent.  V1 supports cross-host messaging via raw `tcp://`; remote processes are started independently. |
| **Actor name registry** | Erlang's `register/2` and `whereis/1` provide process lookup by name.  V1 requires knowing the endpoint or holding the socket handle directly. |
| **In-process thread actors** (`spawn-thread`) | Closure serialization for anonymous lambdas is non-trivial. `spawn-thread-with` (name-based dispatch) is the planned first step. |
| **Monitoring & supervision trees** | `monitor`, `demonitor`, and OTP-style `one-for-one` supervisors are the next actor-model milestone — see [Next Steps](next-steps.md#5--actor-model-enhancements). |
| **Distributed GC** | Not planned. Lifecycle is managed explicitly via `nng-close` / `spawn-wait`. |
| **WebSocket / TLS transports** | nng supports these; they can be enabled post-v1. |

---

## See Also

- **[Networking Primitives](networking.md)** — Complete nng API and option reference
- **[Message Passing & Actors](message-passing.md)** — Actor patterns, worked examples, timeouts
- **[Modules & Stdlib — std.net](modules.md#stdnet--networking--message-passing)** — High-level helper reference
- **[Examples — Networking](examples.md#networking--message-passing)** — All runnable demos
- **[Next Steps](next-steps.md)** — Future FFI and actor model roadmap
