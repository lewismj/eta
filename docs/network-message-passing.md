# Network & Message-Passing Parallelism

> Erlang-style message passing for Eta, powered by ZeroMQ.

## Motivation

Eta's VM is single-threaded: the interpreter loop, GC, stack, and winding stack
are all owned by one thread.  Making the VM thread-safe would require
synchronizing the heap (sharded `concurrent_flat_map`), intern table, GC
mark-sweep, the value stack, the call-frame stack, the winding stack, the
catch stack, and the trail stack — a massive undertaking that would add
latency to every instruction and likely introduce subtle bugs around
continuations and `dynamic-wind`.

Instead we adopt the **process model**: each Eta "actor" is an OS-level
process (or an OS thread running its own independent VM instance) with its
own heap, GC, and stack.  Actors communicate exclusively through message
passing over ZeroMQ sockets.  This gives us:

* **True multi-core parallelism** without any shared mutable state.
* **Fault isolation** — a crash in one actor doesn't corrupt another's heap.
* **Network transparency** — the same `send!` / `recv!` API works whether
  the peer is in-process, on another core, or on another machine.
* **Lightweight dependency** — ZeroMQ is ~400 KB, no runtime deps, and is
  battle-tested in production systems (CERN, finance, gaming).

Bundling networking and parallelism into one module is intentional: both
rely on the same socket primitives, serialization logic, and lifecycle
management.  A "raw TCP port" layer would duplicate most of this work and
offer strictly less functionality than ZeroMQ already provides (framing,
reconnection, fan-out patterns, etc.).

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│                      Eta Source Code                      │
│                                                          │
│  (import (std net))                                      │
│                                                          │
│  (define sock (zmq-socket 'pair))                        │
│  (zmq-connect sock "tcp://localhost:5555")               │
│  (send! sock '(hello world))                             │
│  (define msg (recv! sock))                               │
│                                                          │
│  ;; Spawn a parallel worker                              │
│  (define worker (spawn "worker.eta"))                    │
│  (send! worker '(compute 42))                            │
│  (define result (recv! worker))                          │
└──────────────────────┬───────────────────────────────────┘
                       │  Eta primitives (std/net.eta)
                       ▼
┌──────────────────────────────────────────────────────────┐
│              eta/zmq/ — C++ Primitive Layer               │
│                                                          │
│  ZmqContextPtr    — global zmq_ctx_t (one per Driver)    │
│  ZmqSocketPtr     — heap object wrapping zmq_socket_t    │
│  Serializer       — LispVal ↔ wire bytes                 │
│  zmq_primitives.h — register_zmq_primitives(env,...)     │
│  process_mgr.h    — spawn / monitor child Eta processes  │
└──────────────────────┬───────────────────────────────────┘
                       │  links against
                       ▼
┌──────────────────────────────────────────────────────────┐
│              libzmq (fetched via CMake)                   │
└──────────────────────────────────────────────────────────┘
```

---

## Interaction with `dynamic-wind` and Continuations

This is a critical design constraint.  Eta supports first-class continuations
(`call/cc`) and `dynamic-wind` — both of which capture and restore the winding
stack when control jumps non-locally.

### Why message passing is safe

Continuations capture the **value stack**, **call frames**, and **winding
stack** of a single VM.  ZeroMQ sockets are heap objects (like ports or
tensors) — they are referenced by a `LispVal` in the heap but their
internal state (file descriptors, OS buffers) is **not** captured by
continuations.  This is identical to how file ports behave today: you can
`call/cc` while a file is open, and re-invoking the continuation doesn't
"rewind" the file position.

**Rules:**

1. **Sockets are not rewound.**  Invoking a captured continuation does not
   replay or undo any messages already sent/received.  This matches Erlang
   semantics (and common sense for I/O).

2. **`dynamic-wind` guards socket cleanup.**  Users should wrap socket
   operations in `dynamic-wind` to ensure sockets are closed when control
   leaves a region (just as they would with file ports):

   ```scheme
   (let ((sock #f))
     (dynamic-wind
       (lambda () (set! sock (zmq-socket 'pair)))
       (lambda () (zmq-connect sock addr) (send! sock msg))
       (lambda () (zmq-close sock))))
   ```

3. **Blocking `recv!` and continuations.**  If a continuation is invoked
   while a `recv!` is blocked, the `recv!` call in the abandoned
   continuation simply never returns (the OS thread is not blocked — only
   the VM instruction pointer moved).  We recommend `zmq-poll` for
   cooperative waiting to avoid confusion.

4. **Spawned processes are independent.**  A `spawn`ed Eta process has its
   own VM, heap, and continuation space.  The parent's continuations cannot
   reach into a child process.  Communication is strictly via messages.

### No conflicts

Because actors share nothing and sockets are opaque heap objects (like
tensors and ports), there are **no conflicts** with `dynamic-wind` or
continuations.  The same design that makes file ports safe makes ZMQ
sockets safe.

---

## Phase 1 — CMake Integration & ZeroMQ Fetch

**Goal:** Add `libzmq` as an optional dependency using the same pattern as
`eta/torch/`.  No Eta-visible primitives yet — just build infrastructure.

### Tasks

1. **Create `cmake/FetchLibzmq.cmake`**
   - Mirror `FetchLibtorch.cmake` structure.
   - Use CMake `FetchContent` to download `libzmq` from the official GitHub
     releases (or build from source via `FetchContent_Declare`).
   - Set `ZeroMQ_DIR` so subsequent `find_package(ZeroMQ)` succeeds.
   - Also fetch `cppzmq` (the header-only C++ binding) which provides
     `zmq.hpp` — a RAII wrapper around raw `zmq_*` C calls.

2. **Add `ETA_BUILD_ZMQ` option to `eta/CMakeLists.txt`**
   ```cmake
   option(ETA_BUILD_ZMQ "Enable ZeroMQ networking and message-passing" ON)
   ```
   - Try `find_package(ZeroMQ QUIET)` first.
   - If not found, include `FetchLibzmq.cmake`.
   - If still not found, set `ETA_BUILD_ZMQ OFF` with a warning.

3. **Create `eta/zmq/` subdirectory**
   ```
   eta/zmq/
     CMakeLists.txt
     src/
       eta/
         zmq/
           zmq_context.h       # ZMQ context singleton
   ```
   - `eta_zmq` is an `INTERFACE` library (header-only, like `eta_torch`).
   - Links against `eta_core` and `libzmq` / `cppzmq`.

4. **Wire into the build**
   - `eta/CMakeLists.txt`: `if(ETA_BUILD_ZMQ) add_subdirectory(zmq) endif()`
   - Interpreter / REPL / LSP link `eta_zmq` when the option is on.
   - Add a `eta_copy_zmq_dlls()` helper for Windows (mirrors
     `eta_copy_torch_dlls()`).

### Acceptance Criteria

- [ ] `cmake -DETA_BUILD_ZMQ=ON ..` succeeds on Windows, Linux, macOS.
- [ ] `cmake -DETA_BUILD_ZMQ=OFF ..` builds Eta without ZeroMQ (no link errors).
- [ ] A trivial C++ unit test creates a ZMQ context and destroys it.

---

## Phase 2 — Value Serialization (Wire Format)

**Goal:** Implement bidirectional serialization of `LispVal` values to a
byte buffer suitable for ZMQ message frames.

### Design

Two formats, selectable at call site:

| Format | Use Case | Implementation |
|--------|----------|----------------|
| **S-expression (text)** | Debugging, logging, human-readable | `value_formatter.h` (FormatMode::Write) for serialize; new `datum_reader` for deserialize |
| **Binary (etac-derived)** | Performance, large data, tensors | Reuse `BytecodeSerializer` constant encoding (CT_Nil, CT_Fixnum, CT_String, etc.) |

Phase 2 implements the **s-expression format** first (simpler, debuggable).
The binary format is deferred to Phase 6.

### Tasks

1. **Implement `datum_reader.h` — string → LispVal parser**
   - Reuse `eta/reader/parser.h` to parse an s-expression string into
     `SExpr` AST nodes.
   - Implement `sexpr_to_value()`: walks the `SExpr` tree and produces
     heap-allocated `LispVal` objects:
     - `SExpr::Number` → `make_fixnum` / `make_flonum`
     - `SExpr::String` → `make_string`
     - `SExpr::Symbol` → `make_symbol` (boxed as `Tag::Symbol`)
     - `SExpr::List`   → chain of `make_cons`
     - `SExpr::Bool`   → `True` / `False`
     - `SExpr::Char`   → `ops::encode<char32_t>`
     - `SExpr::Vector` → `make_vector`
   - Place in `eta/core/src/eta/runtime/datum_reader.h`.

2. **Implement `wire_format.h` — serialize/deserialize entry points**
   ```cpp
   namespace eta::zmq {
     // Serialize a LispVal to an s-expression string (uses format_value Write mode).
     std::string serialize_value(LispVal v, Heap& heap, InternTable& intern);

     // Deserialize an s-expression string back to a LispVal.
     std::expected<LispVal, RuntimeError>
     deserialize_value(std::string_view data, Heap& heap, InternTable& intern);
   }
   ```

3. **Round-trip unit tests**
   - For every value type: nil, booleans, fixnums (small and heap-allocated),
     flonums, characters, strings, symbols, pairs, lists, vectors,
     bytevectors, nested structures.
   - Verify `deserialize(serialize(v)) == v` (structural equality).

### Limitations (Phase 2)

- Closures, continuations, ports, and tensors are **not serializable**.
  Attempting to serialize one produces `#<closure>` etc., which cannot be
  deserialized.  This is intentional — closures capture code pointers and
  upvalue references that are meaningless in another process.
- The error message will clearly state: "cannot send non-serializable value".

### Acceptance Criteria

- [ ] Round-trip tests pass for all data types.
- [ ] `deserialize_value` rejects malformed input with a clear error.
- [ ] Performance: serialize + deserialize of a 10,000-element list completes
      in under 10 ms on a modern machine.

---

## Phase 3 — ZMQ Socket Primitives

**Goal:** Expose ZeroMQ sockets to Eta code as first-class heap objects
with a small, orthogonal set of primitives.

### Heap Object

```cpp
// eta/zmq/src/eta/zmq/zmq_socket_ptr.h

struct ZmqSocketPtr {
    std::unique_ptr<zmq::socket_t> socket;
    zmq::socket_type type;
    bool bound{false};
    bool connected{false};
};
```

Registered as a new `ObjectKind::ZmqSocket` in `heap.h`.  The heap's
type-erased destructor calls `socket->close()` automatically when the
object is GC'd.

### Primitives

All registered via `register_zmq_primitives()` in `zmq_primitives.h`,
following the `register_torch_primitives()` / `register_port_primitives()`
pattern.

| Primitive | Signature | Description |
|-----------|-----------|-------------|
| `zmq-socket` | `(zmq-socket type-symbol)` | Create a socket. Types: `'pair`, `'pub`, `'sub`, `'push`, `'pull`, `'req`, `'rep`, `'dealer`, `'router` |
| `zmq-bind` | `(zmq-bind sock endpoint)` | Bind to an endpoint (e.g. `"tcp://*:5555"`, `"ipc:///tmp/eta.sock"`, `"inproc://workers"`) |
| `zmq-connect` | `(zmq-connect sock endpoint)` | Connect to an endpoint |
| `zmq-close` | `(zmq-close sock)` | Close the socket |
| `zmq-socket?` | `(zmq-socket? x)` | Predicate |
| `send!` | `(send! sock value)` | Serialize `value` and send as a ZMQ message frame |
| `recv!` | `(recv! sock)` | Receive a ZMQ message frame and deserialize to a `LispVal` |
| `zmq-poll` | `(zmq-poll items timeout-ms)` | Poll multiple sockets; returns list of ready sockets. `items` is a list of `(socket . events)` pairs. |
| `zmq-subscribe` | `(zmq-subscribe sock topic)` | Set SUB filter (topic is a string prefix) |
| `zmq-set-option` | `(zmq-set-option sock option value)` | Set socket options (e.g. `'linger`, `'rcvtimeo`, `'sndhwm`) |

### Non-blocking mode

`send!` and `recv!` accept an optional `'noblock` flag:

```scheme
(send! sock value 'noblock)   ; returns #t on success, #f if EAGAIN
(recv! sock 'noblock)         ; returns value or #f if nothing available
```

### Error handling

ZMQ errors are mapped to Eta exceptions via the existing `Throw` /
`SetupCatch` mechanism:

```scheme
(guard (exn (#t (display (error-message exn))))
  (send! sock value))
```

The exception tag is `'zmq-error`, and the value is a string describing
the error (from `zmq_strerror(errno)`).

### Acceptance Criteria

- [ ] Can create, bind, connect, send, receive, and close a PAIR socket.
- [ ] Can pub/sub with topic filtering.
- [ ] Can push/pull fan-out to multiple workers.
- [ ] `zmq-poll` correctly reports ready sockets.
- [ ] `zmq-close` is idempotent and safe to call multiple times.
- [ ] GC of an unreferenced socket calls `zmq_close` automatically.
- [ ] Non-blocking send/recv works correctly.

---

## Phase 4 — Process Spawning & Actor Model

**Goal:** Implement `spawn` — create a child Eta process running its own
VM, connected to the parent via a ZMQ `PAIR` socket.  This is the core of
Erlang-style message passing.

### Design

```
┌─────────────────────┐     PAIR socket      ┌─────────────────────┐
│   Parent Process     │◄──────────────────►│   Child Process      │
│                      │  ipc:// or tcp://   │                      │
│  VM₁, Heap₁, GC₁    │                     │  VM₂, Heap₂, GC₂    │
│                      │                     │                      │
│  worker = (spawn     │                     │  (define parent      │
│    "worker.eta")     │                     │    (current-mailbox))│
│  (send! worker msg)  │                     │  (define msg         │
│  (recv! worker)      │                     │    (recv! parent))   │
└─────────────────────┘                     └─────────────────────┘
```

### `spawn` semantics

```scheme
(spawn module-path)           ; → returns a zmq-socket connected to child
(spawn module-path endpoint)  ; → custom endpoint (default: auto-assigned ipc/tcp)
```

`spawn` does the following:

1. **Selects a transport.**
   - On Unix: `ipc:///tmp/eta-<pid>-<counter>.sock` (fastest, no TCP overhead).
   - On Windows: `tcp://127.0.0.1:<ephemeral-port>` (Windows lacks Unix
     domain sockets in older versions; ZMQ handles port selection).
   - Override with explicit endpoint argument.

2. **Creates a PAIR socket** in the parent and binds it.

3. **Launches a child process** running `etai <module-path> --mailbox <endpoint>`.
   - Uses `boost::process::child` (Boost is already a dependency) for
     cross-platform process creation.  This avoids relying on C++23
     `std::process` which is not yet stable across compilers.
   - The child's stdout/stderr are optionally captured or forwarded.

4. **The child connects** its mailbox socket to the parent's endpoint on
   startup.  The `current-mailbox` variable is automatically bound.

5. **Returns the parent-side socket** as the handle for `send!` / `recv!`.

### Child-side API

Inside a spawned module, the following are available:

```scheme
(current-mailbox)   ; → the PAIR socket connected to the parent
(send! (current-mailbox) result)
(define msg (recv! (current-mailbox)))
```

### Module path assumption

Spawned processes share the same module path as the parent (same stdlib
directory, same `ETA_MODULE_PATH`).  The child is a fresh `etai`
invocation — it re-compiles or loads cached `.etac` files as normal.

### Lifecycle management

| Event | Behavior |
|-------|----------|
| Parent calls `(zmq-close worker)` | Socket closes; child's next `recv!` gets an error → child exits. |
| Parent crashes | OS closes the socket fd; child's next `recv!` gets ETERM → child exits. |
| Child crashes | Parent's next `recv!` gets an error; parent can handle it. |
| Child exits normally | Parent's next `recv!` gets ETERM or empty → parent can detect. |

For robustness, an optional **heartbeat** protocol can be layered on top
in Phase 7 (a periodic ping/pong on the PAIR socket).

### Process manager

```cpp
// eta/zmq/src/eta/zmq/process_mgr.h

class ProcessManager {
public:
    struct ChildHandle {
        boost::process::child process;
        std::string endpoint;
        LispVal socket;  // parent-side PAIR socket (heap object)
    };

    // Spawn a child Eta process.
    std::expected<LispVal, RuntimeError>
    spawn(const std::string& module_path,
          Heap& heap, InternTable& intern,
          const std::string& etai_path);

    // Wait for a child to exit.
    void wait(ChildHandle& child);

    // Kill a child process.
    void kill(ChildHandle& child);

    // Destructor kills all remaining children.
    ~ProcessManager();

private:
    std::vector<ChildHandle> children_;
    zmq::context_t& ctx_;
    int spawn_counter_{0};
};
```

### Primitives

| Primitive | Signature | Description |
|-----------|-----------|-------------|
| `spawn` | `(spawn module-path)` | Spawn a child process, return parent-side socket |
| `spawn-kill` | `(spawn-kill sock)` | Forcibly terminate the child associated with this socket |
| `spawn-wait` | `(spawn-wait sock)` | Block until the child process exits; return exit code |
| `current-mailbox` | `(current-mailbox)` | In a spawned child, returns the socket to the parent |

### Acceptance Criteria

- [ ] `(spawn "examples/hello.eta")` launches a child process.
- [ ] Parent can send a message, child receives it, sends a reply, parent
      receives the reply.
- [ ] Child exits cleanly when the parent closes the socket.
- [ ] Parent detects child crash and gets an error from `recv!`.
- [ ] Works on Windows (tcp loopback) and Linux/macOS (ipc).
- [ ] Multiple children can be spawned concurrently.

---

## Phase 5 — Standard Library (`std/net.eta`)

**Goal:** Provide a high-level Eta module that wraps the raw ZMQ primitives
into ergonomic, Erlang-inspired patterns.

### Module structure

```scheme
;; stdlib/std/net.eta
(define-module (std net)
  (export
    ;; Low-level ZMQ
    zmq-socket zmq-bind zmq-connect zmq-close zmq-socket?
    send! recv! zmq-poll zmq-subscribe zmq-set-option

    ;; Actor model
    spawn spawn-kill spawn-wait current-mailbox

    ;; High-level patterns
    with-socket        ; dynamic-wind wrapper
    request-reply      ; synchronous request/reply
    worker-pool        ; fan-out to N workers
    pub-sub            ; publish/subscribe setup
    ))
```

### High-level helpers

```scheme
;; Safe socket management via dynamic-wind
(define (with-socket type thunk)
  (let ((sock #f))
    (dynamic-wind
      (lambda () (set! sock (zmq-socket type)))
      (lambda () (thunk sock))
      (lambda () (when sock (zmq-close sock))))))

;; Synchronous request-reply
(define (request-reply endpoint message)
  (with-socket 'req
    (lambda (sock)
      (zmq-connect sock endpoint)
      (send! sock message)
      (recv! sock))))

;; Spawn N workers, distribute work, collect results
(define (worker-pool module-path tasks)
  (let* ((n (length tasks))
         (workers (map (lambda (_) (spawn module-path)) tasks)))
    ;; Send one task to each worker
    (for-each (lambda (w t) (send! w t)) workers tasks)
    ;; Collect results
    (let ((results (map (lambda (w) (recv! w)) workers)))
      ;; Clean up
      (for-each zmq-close workers)
      results)))

;; Pub/sub convenience
(define (pub-sub bind-endpoint topics handler)
  (with-socket 'sub
    (lambda (sock)
      (zmq-connect sock bind-endpoint)
      (for-each (lambda (t) (zmq-subscribe sock t)) topics)
      (let loop ()
        (handler (recv! sock))
        (loop)))))
```

### Acceptance Criteria

- [ ] `(import (std net))` works in Eta programs.
- [ ] `with-socket` cleans up on normal exit and on continuation escape.
- [ ] `request-reply` performs a synchronous round-trip.
- [ ] `worker-pool` distributes tasks to N workers and collects results.
- [ ] Examples compile and run: `examples/message-passing.eta`,
      `examples/worker-pool.eta`.

---

## Phase 6 — Binary Wire Format (Performance)

**Goal:** Add a compact binary serialization format for high-throughput
message passing, reusing the `.etac` constant encoding infrastructure.

### Design

Reuse the `BytecodeSerializer` constant tag scheme (`CT_Nil`, `CT_Fixnum`,
`CT_String`, `CT_HeapCons`, `CT_HeapVec`, etc.) but write to/from a
`std::vector<uint8_t>` rather than a file stream.

```cpp
namespace eta::zmq {
  // Binary wire format (compact, fast)
  std::vector<uint8_t>
  serialize_binary(LispVal v, Heap& heap, InternTable& intern);

  std::expected<LispVal, RuntimeError>
  deserialize_binary(std::span<const uint8_t> data, Heap& heap, InternTable& intern);
}
```

### Format selection

`send!` defaults to binary.  A `'text` flag uses the s-expression format:

```scheme
(send! sock value)          ; binary (fast)
(send! sock value 'text)    ; s-expression (debuggable)
```

`recv!` auto-detects: binary messages start with a version byte (`0xEA`);
s-expression messages start with `(` or a printable character.

### Tensor serialization (when `ETA_BUILD_TORCH` is ON)

For `ObjectKind::Tensor`, serialize the tensor metadata (dtype, shape) plus
raw contiguous bytes.  This enables passing tensors between processes
without going through Python/NumPy.

```
[CT_Tensor] [dtype:u8] [ndim:u32] [dim₀:i64]...[dimₙ:i64] [nbytes:u64] [raw data...]
```

### Acceptance Criteria

- [ ] Binary round-trip tests pass for all serializable types.
- [ ] Binary format is ≥5× faster than s-expression for large lists.
- [ ] Tensor serialization works for float32 and float64 tensors.
- [ ] `recv!` auto-detects binary vs text.

---

## Phase 7 — In-Process Threads (Lightweight Actors)

**Goal:** Allow spawning actors as **OS threads** within the same process,
each running an independent VM instance with its own heap.  Uses
`inproc://` transport for zero-copy message passing.

### Motivation

Spawning a full child process (Phase 4) has overhead: process creation,
module re-compilation, and IPC serialization.  For fine-grained parallelism
(e.g., parallel map over a list), in-process threads are more appropriate.

### Design

```scheme
(spawn-thread thunk)   ; → returns a zmq-socket (PAIR, inproc://)
```

Under the hood:

1. Create a `PAIR` socket pair on `inproc://eta-thread-<N>`.
2. Launch an `std::thread` that:
   - Creates a new `Heap`, `InternTable`, and `VM`.
   - Registers builtins (including ZMQ primitives).
   - Executes `thunk` (a lambda/closure that has been **serialized** and
     deserialized into the new VM's heap).
3. The parent gets the parent-side socket; the child thread gets the
   child-side socket as its `current-mailbox`.

### Closure serialization challenge

Closures contain code pointers and upvalue references.  To send a closure
to another VM instance:

- **Code** is identified by function name + module.  Both VMs load the
  same module, so function pointers can be resolved by name.
- **Upvalues** are serialized as values (only serializable values —
  numbers, strings, symbols, lists, vectors).
- A closure referencing another closure or a continuation **cannot** be
  sent to a thread.

This is enforced at runtime: `spawn-thread` attempts to serialize the
closure's upvalues and raises an error if any are non-serializable.

### Alternative: code-only dispatch

A simpler model avoids closure serialization entirely:

```scheme
(spawn-thread-with "module.eta" 'function-name arg1 arg2 ...)
```

The child thread loads the named module, looks up the function by name,
and calls it with the serialized arguments.  This is closer to Erlang's
`spawn(Module, Function, Args)`.

**Recommendation:** Implement `spawn-thread-with` first (Phase 7a), then
`spawn-thread` with closure serialization as Phase 7b.

### Primitives

| Primitive | Signature | Description |
|-----------|-----------|-------------|
| `spawn-thread-with` | `(spawn-thread-with module func-name args...)` | Spawn an in-process actor |
| `spawn-thread` | `(spawn-thread thunk)` | Spawn with closure (Phase 7b) |
| `thread-join` | `(thread-join sock)` | Wait for thread to complete |
| `thread-alive?` | `(thread-alive? sock)` | Check if thread is still running |

### Acceptance Criteria

- [ ] `spawn-thread-with` creates a thread with an independent VM.
- [ ] Parent and child communicate via `send!` / `recv!` over `inproc://`.
- [ ] No shared mutable state between parent and child heaps.
- [ ] Thread cleanup on normal exit and on parent socket close.
- [ ] Stress test: spawn 100 threads, each computes a value and returns it.

---

## Phase 8 — Monitoring, Heartbeats & Supervision

**Goal:** Add Erlang-style monitoring and supervision primitives for
fault-tolerant actor systems.

### Monitoring

```scheme
(monitor child)         ; → parent receives '(down child reason) when child dies
(demonitor child)       ; → stop monitoring
```

Implementation: a dedicated monitoring thread per parent that polls all
monitored sockets and detects disconnection.

### Heartbeats

```scheme
(enable-heartbeat sock interval-ms)   ; enable ping/pong on a socket
```

Heartbeat is implemented as a ZMQ `PAIR` socket option or as an
application-level periodic message (`'__eta_ping__` / `'__eta_pong__`).

### Supervision trees (library-level)

```scheme
;; stdlib/std/supervisor.eta
(define (one-for-one children-specs)
  ;; Restart any child that crashes
  ...)

(define (one-for-all children-specs)
  ;; Restart all children if any crashes
  ...)
```

These are pure Eta code built on top of `spawn`, `monitor`, and `recv!`.

### Acceptance Criteria

- [ ] Parent receives a `(down ...)` message when a child crashes.
- [ ] Heartbeat detects a hung child within 2× the interval.
- [ ] `one-for-one` supervisor restarts a crashed child automatically.

---

## Phase 9 — ZMQ Port Adapter (Optional)

**Goal:** Bridge ZMQ sockets into the Eta port system so that standard
`read` / `write` / `display` work on sockets.

### Design

```cpp
class ZmqPort : public Port {
public:
    ZmqPort(std::shared_ptr<zmq::socket_t> socket, Mode mode);

    std::optional<char32_t> read_char() override;
    std::expected<void, RuntimeError> write_string(const std::string& str) override;
    std::expected<void, RuntimeError> close() override;
    bool is_open() const override;
    bool is_input() const override;
    bool is_output() const override;

private:
    std::shared_ptr<zmq::socket_t> socket_;
    Mode mode_;
    std::string recv_buffer_;
    size_t recv_pos_{0};
};
```

This port buffers received messages and exposes them character-by-character
via `read-char`.  `write-string` accumulates data and sends it as a ZMQ
frame on `flush` or `close`.

### Primitives

```scheme
(zmq-socket->input-port sock)    ; → input port reading from socket
(zmq-socket->output-port sock)   ; → output port writing to socket
```

### Acceptance Criteria

- [ ] `(read (zmq-socket->input-port sock))` reads an s-expression sent
      from the other end.
- [ ] `(write value (zmq-socket->output-port sock))` sends the value.
- [ ] Port operations work with `with-input-from-port` /
      `with-output-to-port` parameter objects.

---

## Phase 10 — Documentation, Examples & Polish

### Tasks

1. **Documentation**
   - `docs/networking.md` — ZMQ socket primitives reference.
   - `docs/message-passing.md` — Actor model guide with diagrams.
   - `docs/examples.md` — Update with networking examples.
   - Update `docs/modules.md` with `(std net)`.

2. **Examples**
   ```
   examples/
     echo-server.eta          ; simple req/rep echo
     message-passing.eta      ; parent-child communication
     worker-pool.eta          ; fan-out parallelism
     pub-sub.eta              ; publish-subscribe
     parallel-map.eta         ; in-process thread pool
     monte-carlo.eta          ; parallel Monte Carlo simulation
     distributed-compute.eta  ; multi-machine example
   ```

3. **VS Code extension**
   - Add ZMQ-related snippets (`zmq-socket`, `spawn`, `with-socket`).
   - LSP completions for `(std net)` exports.

4. **DAP integration**
   - Show ZMQ sockets in the heap view (display endpoint, socket type,
     bound/connected state).
   - Show spawned child processes in a tree view.

5. **Performance benchmarks**
   - Message throughput: messages/sec for binary vs text format.
   - Latency: round-trip time for request-reply.
   - Scalability: throughput vs number of worker threads.

### Acceptance Criteria

- [ ] All examples compile and run.
- [ ] Documentation is complete and reviewed.
- [ ] VS Code extension includes ZMQ snippets and completions.
- [ ] Benchmarks are documented with reproducible methodology.

---

## Summary of Files Changed/Added per Phase

| Phase | New Files | Modified Files |
|-------|-----------|----------------|
| 1 | `cmake/FetchLibzmq.cmake`, `eta/zmq/CMakeLists.txt`, `eta/zmq/src/eta/zmq/zmq_context.h` | `eta/CMakeLists.txt`, `CMakeLists.txt` |
| 2 | `eta/core/src/eta/runtime/datum_reader.h`, `eta/zmq/src/eta/zmq/wire_format.h` | — |
| 3 | `eta/zmq/src/eta/zmq/zmq_socket_ptr.h`, `eta/zmq/src/eta/zmq/zmq_primitives.h`, `eta/zmq/src/eta/zmq/zmq_factory.h` | `eta/core/src/eta/runtime/memory/heap.h` (new ObjectKind), `eta/core/src/eta/runtime/builtin_names.h`, `eta/interpreter/src/eta/interpreter/driver.h` |
| 4 | `eta/zmq/src/eta/zmq/process_mgr.h` | `eta/zmq/src/eta/zmq/zmq_primitives.h` |
| 5 | `stdlib/std/net.eta` | `stdlib/prelude.eta` (optional auto-import) |
| 6 | — | `eta/zmq/src/eta/zmq/wire_format.h` (add binary format) |
| 7 | — | `eta/zmq/src/eta/zmq/zmq_primitives.h`, `eta/zmq/src/eta/zmq/process_mgr.h` |
| 8 | `stdlib/std/supervisor.eta` | `eta/zmq/src/eta/zmq/zmq_primitives.h` |
| 9 | `eta/zmq/src/eta/zmq/zmq_port.h` | `eta/zmq/src/eta/zmq/zmq_primitives.h` |
| 10 | `docs/networking.md`, `docs/message-passing.md`, `examples/*.eta` | `docs/examples.md`, `docs/modules.md`, VS Code extension files |

---

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| ZMQ build complexity on Windows | Medium | Pre-built binaries via FetchContent; fallback to vcpkg |
| Blocking `recv!` freezes VM | High | Default to `zmq-poll`; document `'noblock` flag prominently |
| Closure serialization is complex | Medium | Phase 7a uses name-based dispatch; defer full closure serialization |
| Child process zombies | Medium | Heartbeat (Phase 8); `ProcessManager` destructor kills all children |
| Large message serialization overhead | Low | Binary format (Phase 6) minimizes overhead; tensor zero-copy path |
| `dynamic-wind` confusion with sockets | Low | Document clearly that sockets are not rewound; recommend `with-socket` |
| Cross-platform `ipc://` differences | Low | Windows uses `tcp://127.0.0.1`; abstracted in `spawn` |

---

## Dependencies

| Dependency | Version | License | Size | Notes |
|------------|---------|---------|------|-------|
| libzmq | ≥ 4.3 | MPL-2.0 | ~400 KB | Core C library |
| cppzmq | ≥ 4.10 | MIT | Header-only | C++ RAII wrapper |
| Boost.Process | (already in Boost 1.88) | BSL-1.0 | (already linked) | Cross-platform process spawning |

All dependencies are compatible with Eta's existing license and build system.

