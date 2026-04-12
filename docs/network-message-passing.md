# Network & Message-Passing Parallelism

[← Back to README](../README.md) · [Next Steps](next-steps.md) ·
[Architecture](architecture.md) · [Runtime & GC](runtime.md) ·
[Modules & Stdlib](modules.md)

---

> Erlang-style message passing for Eta, powered by nng.

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
passing over nng sockets.  This gives us:

* **True multi-core parallelism** without any shared mutable state.
* **Fault isolation** — a crash in one actor doesn't corrupt another's heap.
* **Network transparency** — the same `send!` / `recv!` API works whether
  the peer is in-process, on another core, or on another machine.
* **Tiny, self-contained dependency** — nng is ~200 KB, has zero external
  runtime dependencies, builds as a static library via CMake `FetchContent`,
  and is MIT-licensed.  It descends from nanomsg and implements the SP
  (Scalability Protocols) specification.
* **First-class Windows support** — nng supports `ipc://` on Windows via
  named pipes, eliminating the Unix-only IPC limitation of some other
  messaging libraries.
* **Thread-safe sockets** — nng sockets can be used from any thread without
  the "one socket per thread" restriction, simplifying the in-process actor
  model (Phase 7).
* **No global context object** — unlike ZeroMQ, nng has no `zmq_ctx_t` to
  manage.  Sockets are opened directly via protocol-specific functions.

Bundling networking and parallelism into one module is intentional: both
rely on the same socket primitives, serialization logic, and lifecycle
management.  A "raw TCP port" layer would duplicate most of this work and
offer strictly less functionality than nng already provides (framing,
reconnection, fan-out patterns, etc.).

### Why nng over ZeroMQ?

| Criterion | nng | ZeroMQ |
|-----------|-----|--------|
| **License** | MIT | MPL-2.0 (libzmq) + MIT (cppzmq) |
| **Binary size** | ~200 KB (static) | ~400 KB + header-only C++ wrapper |
| **Build integration** | Single CMake target via `FetchContent` | Requires fetching both `libzmq` and `cppzmq`; complex DLL copying on Windows |
| **Context object** | None — sockets are standalone | Global `zmq_ctx_t` per process, must be managed and shut down |
| **Thread safety** | Sockets are thread-safe by default | One socket per thread; violating this causes undefined behavior |
| **Windows IPC** | `ipc://` via named pipes (first-class) | `ipc://` not supported on Windows; must fall back to TCP loopback |
| **Async I/O** | Built-in `nng_aio` (completion-based) | `zmq_poll` (readiness-based) |
| **Extra protocols** | SURVEYOR/RESPONDENT, BUS | DEALER/ROUTER (different pattern family) |

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│                      Eta Source Code                      │
│                                                          │
│  (import (std net))                                      │
│                                                          │
│  (define sock (nng-socket 'pair))                        │
│  (nng-dial sock "tcp://localhost:5555")                   │
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
│              eta/nng/ — C++ Primitive Layer               │
│                                                          │
│  NngSocketPtr     — heap object wrapping nng_socket      │
│  Serializer       — LispVal ↔ wire bytes                 │
│  nng_primitives.h — register_nng_primitives(env,...)     │
│  process_mgr.h    — spawn / monitor child Eta processes  │
└──────────────────────┬───────────────────────────────────┘
                       │  links against
                       ▼
┌──────────────────────────────────────────────────────────┐
│         libnng (fetched via CMake FetchContent)           │
└──────────────────────────────────────────────────────────┘
```

---

## Interaction with `dynamic-wind` and Continuations

This is a critical design constraint.  Eta supports first-class continuations
(`call/cc`) and `dynamic-wind` — both of which capture and restore the winding
stack when control jumps non-locally.

### Why message passing is safe

Continuations capture the **value stack**, **call frames**, and **winding
stack** of a single VM.  nng sockets are heap objects (like ports or
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
       (lambda () (set! sock (nng-socket 'pair)))
       (lambda () (nng-dial sock addr) (send! sock msg))
       (lambda () (nng-close sock))))
   ```

3. **Blocking `recv!` and continuations.**  If a continuation is invoked
   while a `recv!` is blocked, the `recv!` call in the abandoned
   continuation simply never returns (the OS thread is not blocked — only
   the VM instruction pointer moved).  We recommend `nng-poll` or
   `nng_aio`-based async I/O for cooperative waiting to avoid confusion.

4. **Spawned processes are independent.**  A `spawn`ed Eta process has its
   own VM, heap, and continuation space.  The parent's continuations cannot
   reach into a child process.  Communication is strictly via messages.

### No conflicts

Because actors share nothing and sockets are opaque heap objects (like
tensors and ports), there are **no conflicts** with `dynamic-wind` or
continuations.  The same design that makes file ports safe makes nng
sockets safe.

---

## Phase 1 — CMake Integration & nng Fetch

**Goal:** Add `libnng` as an optional dependency using the same pattern as
`eta/torch/`.  No Eta-visible primitives yet — just build infrastructure.

### Tasks

1. **Create `cmake/FetchNng.cmake`**
   - Mirror `FetchLibtorch.cmake` structure.
   - Use CMake `FetchContent` to download nng from GitHub:
     ```cmake
     include(FetchContent)
     FetchContent_Declare(
       nng
       GIT_REPOSITORY https://github.com/nanomsg/nng.git
       GIT_TAG        v1.9.0
     )
     set(NNG_TESTS OFF CACHE BOOL "" FORCE)
     set(NNG_TOOLS OFF CACHE BOOL "" FORCE)
     FetchContent_MakeAvailable(nng)
     ```
   - This is significantly simpler than ZeroMQ integration — nng is a
     single CMake project with a single `nng::nng` target.  No separate
     C++ wrapper library is needed (nng's C API is clean enough to use
     directly, and we wrap it in our own RAII types).
   - nng builds as a **static library** by default (`NNG_STATIC_LIB=ON`),
     so there is no DLL-copying step on Windows.

2. **Add `ETA_BUILD_NNG` option to `eta/CMakeLists.txt`**
   ```cmake
   option(ETA_BUILD_NNG "Enable nng networking and message-passing" ON)
   ```
   - Include `FetchNng.cmake` when the option is on.
   - If the fetch fails, set `ETA_BUILD_NNG OFF` with a warning.

3. **Create `eta/nng/` subdirectory**
   ```
   eta/nng/
     CMakeLists.txt
     src/
       eta/
         nng/
           nng_socket_ptr.h   # RAII wrapper around nng_socket
   ```
   - `eta_nng` is a library linking against `eta_core` and `nng::nng`.
   - No global context singleton is needed — nng sockets are opened
     directly via protocol-specific functions (`nng_pair0_open`, etc.).

4. **Wire into the build**
   - `eta/CMakeLists.txt`: `if(ETA_BUILD_NNG) add_subdirectory(nng) endif()`
   - Interpreter / REPL / LSP / DAP link `eta_nng` when the option is on:
     - **`eta/lsp/CMakeLists.txt`**: add conditional block (mirrors torch):
       ```cmake
       if(ETA_BUILD_NNG)
           target_link_libraries(eta_lsp PRIVATE eta_nng)
           target_compile_definitions(eta_lsp PRIVATE ETA_HAS_NNG=1)
       endif()
       ```
     - **`eta/dap/CMakeLists.txt`**: identical conditional block for `eta_dap`.
   - No `eta_copy_nng_dlls()` helper is needed — nng links statically.

### Acceptance Criteria

- [ ] `cmake -DETA_BUILD_NNG=ON ..` succeeds on Windows, Linux, macOS.
- [ ] `cmake -DETA_BUILD_NNG=OFF ..` builds Eta without nng (no link errors).
- [ ] A trivial C++ unit test opens and closes an nng pair socket.

---

## Phase 2 — Value Serialization (Wire Format)

**Goal:** Implement bidirectional serialization of `LispVal` values to a
byte buffer suitable for nng message frames.

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
   namespace eta::nng {
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

## Phase 3 — nng Socket Primitives

**Goal:** Expose nng sockets to Eta code as first-class heap objects
with a small, orthogonal set of primitives.

### Heap Object

```cpp
// eta/nng/src/eta/nng/nng_socket_ptr.h

#include <nng/nng.h>

enum class NngProtocol {
    Pair, Req, Rep, Pub, Sub, Push, Pull, Surveyor, Respondent, Bus
};

struct NngSocketPtr {
    nng_socket socket{NNG_SOCKET_INITIALIZER};
    NngProtocol protocol;
    bool listening{false};
    bool dialed{false};

    ~NngSocketPtr() {
        nng_close(socket);
    }
};
```

Registered as a new `ObjectKind::NngSocket` in `heap.h`.  The heap's
type-erased destructor calls `nng_close()` automatically when the
object is GC'd.

**Key difference from ZeroMQ:** nng has no global context object.  Each
socket is opened via a protocol-specific function (e.g. `nng_pair0_open()`,
`nng_req0_open()`) and is independently managed.  This simplifies the
implementation — there is no context singleton to create at startup or
tear down at exit.

#### DAP heap view support

The DAP server's `build_heap_snapshot()` already uses the generic
`to_string(ObjectKind)` function, so `NngSocket` objects will appear in
the per-kind statistics table automatically once the enum and `to_string`
are updated.

`expand_compound()` in `dap_server.cpp` should handle
`ObjectKind::NngSocket` to display useful fields when the user inspects
one in the Heap Inspector or Variables panel:

```cpp
#ifdef ETA_HAS_NNG
if (auto* ns = heap.try_get_as<ObjectKind::NngSocket, nng::NngSocketPtr>(...)) {
    children.push_back(json::object({
        {"name", "protocol"}, {"value", protocol_name(ns->protocol)}, {"variablesReference", 0}}));
    children.push_back(json::object({
        {"name", "listening"}, {"value", ns->listening ? "true" : "false"}, {"variablesReference", 0}}));
    children.push_back(json::object({
        {"name", "dialed"}, {"value", ns->dialed ? "true" : "false"}, {"variablesReference", 0}}));
}
#endif
```

Also update `is_compound_value()` to return `true` for `NngSocket` so the
Variables panel shows an expand arrow.

### Primitives

All registered via `register_nng_primitives()` in `nng_primitives.h`,
following the `register_torch_primitives()` / `register_port_primitives()`
pattern.

| Primitive | Signature | Description |
|-----------|-----------|-------------|
| `nng-socket` | `(nng-socket type-symbol)` | Create a socket. Types: `'pair`, `'pub`, `'sub`, `'push`, `'pull`, `'req`, `'rep`, `'surveyor`, `'respondent`, `'bus` |
| `nng-listen` | `(nng-listen sock endpoint)` | Listen on an endpoint (e.g. `"tcp://*:5555"`, `"ipc:///tmp/eta.sock"`, `"inproc://workers"`) |
| `nng-dial` | `(nng-dial sock endpoint)` | Dial (connect to) an endpoint |
| `nng-close` | `(nng-close sock)` | Close the socket |
| `nng-socket?` | `(nng-socket? x)` | Predicate |
| `send!` | `(send! sock value)` | Serialize `value` and send as an nng message |
| `recv!` | `(recv! sock)` | Receive an nng message and deserialize to a `LispVal` |
| `nng-poll` | `(nng-poll items timeout-ms)` | Poll multiple sockets; returns list of ready sockets. `items` is a list of `(socket . events)` pairs. Implemented via `nng_aio` internally. |
| `nng-subscribe` | `(nng-subscribe sock topic)` | Set SUB topic filter (topic is a string prefix) |
| `nng-set-option` | `(nng-set-option sock option value)` | Set socket options (e.g. `'recv-timeout`, `'send-timeout`, `'recv-buf-size`) |

### nng protocol-specific open functions

Under the hood, `nng-socket` maps the type symbol to the correct nng
open call:

```cpp
int rv;
switch (protocol) {
    case NngProtocol::Pair:       rv = nng_pair0_open(&sock);       break;
    case NngProtocol::Req:        rv = nng_req0_open(&sock);        break;
    case NngProtocol::Rep:        rv = nng_rep0_open(&sock);        break;
    case NngProtocol::Pub:        rv = nng_pub0_open(&sock);        break;
    case NngProtocol::Sub:        rv = nng_sub0_open(&sock);        break;
    case NngProtocol::Push:       rv = nng_push0_open(&sock);       break;
    case NngProtocol::Pull:       rv = nng_pull0_open(&sock);       break;
    case NngProtocol::Surveyor:   rv = nng_surveyor0_open(&sock);   break;
    case NngProtocol::Respondent: rv = nng_respondent0_open(&sock); break;
    case NngProtocol::Bus:        rv = nng_bus0_open(&sock);        break;
}
if (rv != 0) throw_nng_error(rv);
```

### Default receive timeout

> **Critical:** Eta's VM is single-threaded.  An indefinitely blocking
> `recv!` will freeze the entire VM — including the REPL, LSP, and any
> `dynamic-wind` cleanup.

To prevent this, **`recv!` defaults to a 1 000 ms timeout**.  If no message
arrives within the timeout, `recv!` returns `#f` rather than blocking
forever.  This is implemented by setting `NNG_OPT_RECVTIMEO` on every
newly created socket:

```cpp
nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 1000);
```

```scheme
(recv! sock)              ; blocks up to 1 s, returns value or #f on timeout
(recv! sock 'wait)        ; blocks indefinitely (opt-in, use with caution)
```

The default can be changed per-socket:

```scheme
(nng-set-option sock 'recv-timeout 5000)   ; 5 s timeout
(nng-set-option sock 'recv-timeout -1)     ; infinite (same as 'wait flag)
```

For event-loop style programming, prefer `nng-poll` which can wait on
multiple sockets simultaneously and integrates cleanly with timeouts.

### Non-blocking mode

`send!` and `recv!` accept an optional flag:

```scheme
(send! sock value 'noblock)   ; returns #t on success, #f if EAGAIN
(recv! sock 'noblock)         ; returns value or #f if nothing available
(recv! sock 'wait)            ; blocks indefinitely (override default timeout)
```

Under the hood, non-blocking mode passes `NNG_FLAG_NONBLOCK` to
`nng_send()` / `nng_recv()`.

### Async I/O with `nng_aio`

nng provides a powerful completion-based async I/O mechanism via `nng_aio`.
This is superior to ZeroMQ's readiness-based `zmq_poll` because it avoids
busy-waiting and integrates naturally with the OS event loop.

For Phase 3 we expose this through the `nng-poll` primitive (which uses
`nng_aio` internally).  A future phase could expose `nng_aio` directly for
more advanced use cases.

### Error handling

nng errors are mapped to Eta exceptions via the existing `Throw` /
`SetupCatch` mechanism:

```scheme
(guard (exn (#t (display (error-message exn))))
  (send! sock value))
```

The exception tag is `'nng-error`, and the value is a string describing
the error (from `nng_strerror(rv)`).

### Message ordering guarantees

Different nng socket protocols have different delivery semantics.  Users
must choose the right pattern for their use case:

| Socket Type | Ordering | Delivery | Notes |
|-------------|----------|----------|-------|
| `PAIR` | **Total order** | Guaranteed (1-to-1) | Used by `spawn` and `spawn-thread-with`.  Messages arrive in send order. |
| `REQ`/`REP` | **Lock-step** | Guaranteed (strict alternation) | One request, one reply — always in order.  Cannot pipeline. |
| `PUSH`/`PULL` | **Per-sender FIFO** | Guaranteed per sender | Fan-out: round-robin distribution.  Messages from a single sender arrive in order; messages from different senders may interleave. |
| `PUB`/`SUB` | **Per-publisher FIFO** | **Best-effort** (may drop) | Subscribers that connect late or are slow miss messages.  No back-pressure.  Use for telemetry, logging, broadcast. |
| `SURVEYOR`/`RESPONDENT` | **Per-respondent** | **Best-effort** (deadline-based) | One-to-many query: surveyor sends a question, all respondents reply within a deadline.  Responses may arrive in any order; late responses are discarded. |
| `BUS` | **Per-sender FIFO** | **Best-effort** | Many-to-many: every peer sees every other peer's messages.  Useful for coordination, gossip protocols, and peer discovery. |

**Rule of thumb:** For actor-to-actor communication use `PAIR` (via `spawn`)
or `REQ`/`REP`.  Use `PUB`/`SUB` only when message loss is acceptable.
Use `PUSH`/`PULL` for work distribution where per-sender ordering suffices.
Use `SURVEYOR`/`RESPONDENT` for scatter-gather patterns (e.g., "which
workers are idle?").  Use `BUS` for peer coordination.

### Acceptance Criteria

- [ ] Can create, listen, dial, send, receive, and close a PAIR socket.
- [ ] Can pub/sub with topic filtering.
- [ ] Can push/pull fan-out to multiple workers.
- [ ] `nng-poll` correctly reports ready sockets.
- [ ] `nng-close` is idempotent and safe to call multiple times.
- [ ] GC of an unreferenced socket calls `nng_close` automatically.
- [ ] Non-blocking send/recv works correctly.
- [ ] SURVEYOR/RESPONDENT scatter-gather works.
- [ ] BUS many-to-many messaging works.

### LSP integration (Phase 3)

The LSP server already discovers builtins via static tables in
`lsp_server.cpp`.  When `ETA_HAS_NNG` is defined, the following tables
must include nng primitives:

1. **`keyword_docs`** (hover documentation) — add entries for all ten
   primitives plus `send!` / `recv!`:
   ```cpp
   #ifdef ETA_HAS_NNG
   {"nng-socket",     "**nng-socket** — Create an nng socket.\n\n`(nng-socket type-sym)` ..."},
   {"nng-listen",     "**nng-listen** — Listen on an endpoint.\n\n`(nng-listen sock endpoint)`"},
   {"nng-dial",       "**nng-dial** — Dial (connect to) an endpoint.\n\n`(nng-dial sock endpoint)`"},
   {"nng-close",      "**nng-close** — Close the socket.\n\n`(nng-close sock)`"},
   {"nng-socket?",    "**nng-socket?** — Socket predicate.\n\n`(nng-socket? x)`"},
   {"send!",          "**send!** — Send a value over a socket.\n\n`(send! sock value [flag])`"},
   {"recv!",          "**recv!** — Receive a value from a socket.\n\n`(recv! sock [flag])`"},
   {"nng-poll",       "**nng-poll** — Poll multiple sockets.\n\n`(nng-poll items timeout-ms)`"},
   {"nng-subscribe",  "**nng-subscribe** — Set SUB topic filter.\n\n`(nng-subscribe sock topic)`"},
   {"nng-set-option", "**nng-set-option** — Set socket option.\n\n`(nng-set-option sock option value)`"},
   #endif
   ```

2. **`builtins`** (completion items) — add an `"NNG"` category:
   ```cpp
   #ifdef ETA_HAS_NNG
   {"nng-socket",     1, false, "NNG"}, {"nng-listen",     2, false, "NNG"},
   {"nng-dial",       2, false, "NNG"}, {"nng-close",      1, false, "NNG"},
   {"nng-socket?",    1, false, "NNG"}, {"send!",          2, true,  "NNG"},
   {"recv!",          1, true,  "NNG"}, {"nng-poll",       2, false, "NNG"},
   {"nng-subscribe",  2, false, "NNG"}, {"nng-set-option", 3, false, "NNG"},
   #endif
   ```

3. **`builtin_sigs`** (signature help) — add nng signatures:
   ```cpp
   #ifdef ETA_HAS_NNG
   {"nng-socket",    "(nng-socket type-symbol)"},
   {"nng-listen",    "(nng-listen sock endpoint)"},
   {"nng-dial",      "(nng-dial sock endpoint)"},
   {"send!",         "(send! sock value [flag])"},
   {"recv!",         "(recv! sock [flag])"},
   {"nng-poll",      "(nng-poll items timeout-ms)"},
   #endif
   ```

4. **`register_builtin_names`** in `eta/core/…/builtin_names.h` — add nng
   primitive names so the semantic analyzer doesn't flag them as undefined.
   Guard with `#ifdef ETA_HAS_NNG`.

### Syntax highlighting (Phase 3)

**`editors/vscode/syntaxes/eta.tmLanguage.json`** — add a new pattern
group for nng builtins so they receive syntax highlighting:

```json
{
    "comment": "nng / message-passing builtins",
    "match": "(?<=\\()\\s*(nng-socket|nng-listen|nng-dial|nng-close|nng-socket\\?|send!|recv!|nng-poll|nng-subscribe|nng-set-option)(?=[\\s()\\[\\]])",
    "captures": {
        "1": { "name": "support.function.nng.eta" }
    }
}
```

---

## Phase 4 — Process Spawning & Actor Model

**Goal:** Implement `spawn` — create a child Eta process running its own
VM, connected to the parent via an nng `PAIR` socket.  This is the core of
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
(spawn module-path)           ; → returns an nng-socket connected to child
(spawn module-path endpoint)  ; → custom endpoint (default: auto-assigned ipc)
```

`spawn` does the following:

1. **Selects a transport.**
   - On Unix: `ipc:///tmp/eta-<pid>-<counter>.sock` (fastest, no TCP overhead).
   - On Windows: `ipc://\\.\pipe\eta-<pid>-<counter>` (nng supports IPC on
     Windows via named pipes — no TCP loopback fallback needed).
   - Override with explicit endpoint argument.

2. **Creates a PAIR socket** in the parent and listens on it.

3. **Launches a child process** running `etai <module-path> --mailbox <endpoint>`.
   - Uses `boost::process::child` (Boost is already a dependency) for
     cross-platform process creation.  This avoids relying on C++23
     `std::process` which is not yet stable across compilers.
   - The child's stdout/stderr are optionally captured or forwarded.

4. **The child dials** the parent's endpoint on startup, connecting its
   mailbox socket.  The `current-mailbox` variable is automatically bound.

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
| Parent calls `(nng-close worker)` | Socket closes; child's next `recv!` gets `NNG_ECLOSED` → child exits. |
| Parent crashes | OS closes the socket fd; child's next `recv!` gets `NNG_ECLOSED` → child exits. |
| Child crashes | Parent's next `recv!` gets an error; parent can handle it. |
| Child exits normally | Parent's next `recv!` gets `NNG_ECLOSED` → parent can detect. |

For robustness, an optional **heartbeat** protocol can be layered on top
in Phase 8 (a periodic ping/pong on the PAIR socket).

### Remote / cross-host connectivity

`spawn` is designed for **local** child processes — it launches an `etai`
process on the same machine.  Transparent remote spawning (i.e. SSH-based
deployment or a node-discovery daemon like Erlang's EPMD) is explicitly
**out of scope for v1** to keep the implementation tractable.

Cross-host message passing is fully supported through the **raw socket
API** (Phase 3).  Two Eta processes on different machines communicate by
listening and dialing `tcp://` endpoints:

```scheme
;; ── Machine A (server) ──
(define sock (nng-socket 'pair))
(nng-listen sock "tcp://*:5555")
(define msg (recv! sock))          ; waits for remote message

;; ── Machine B (client) ──
(define sock (nng-socket 'pair))
(nng-dial sock "tcp://machine-a:5555")
(send! sock '(hello from machine-b))
```

The `spawn` endpoint override also enables connecting a local parent to a
pre-started remote worker:

```scheme
;; Parent knows the remote worker is already listening on tcp://10.0.0.5:9000
;; Create the PAIR socket manually and dial:
(define remote (nng-socket 'pair))
(nng-dial remote "tcp://10.0.0.5:9000")
(send! remote '(compute 42))
(define result (recv! remote))
```

In this model the remote `etai` process is started independently (e.g. via
SSH, systemd, or a container orchestrator) with `--mailbox tcp://*:9000`.
The Eta code on both sides is identical to the local case — only the
endpoint string changes.  This is nng's core "network transparency" benefit.

**Future work (post-v1):**  A `spawn-remote` primitive that SSHs into a
host and starts `etai` automatically, or an opt-in discovery service, could
be layered on top of this foundation.

### Process manager

```cpp
// eta/nng/src/eta/nng/process_mgr.h

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
    int spawn_counter_{0};
};
```

**Note:** Unlike ZeroMQ, there is no `zmq::context_t` member — nng
requires no global context.

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
- [ ] Works on Windows (IPC via named pipes) and Linux/macOS (IPC via Unix
      domain sockets).
- [ ] Multiple children can be spawned concurrently.

### LSP integration (Phase 4)

Add `spawn`, `spawn-kill`, `spawn-wait`, and `current-mailbox` to the
LSP's `keyword_docs`, `builtins`, and `builtin_sigs` tables (guarded by
`#ifdef ETA_HAS_NNG`), following the same pattern as Phase 3.

Add them to the tmLanguage grammar alongside the Phase 3 primitives:

```json
"match": "(?<=\\()\\s*(spawn|spawn-kill|spawn-wait|current-mailbox)(?=[\\s()\\[\\]])"
```

### DAP integration (Phase 4) — child process tree view

The existing DAP server reports a single `"main"` thread in
`handle_threads()`.  Spawned child processes are separate OS processes,
so they do **not** appear in the DAP threads list (each has its own VM
with its own potential debug adapter).

Instead, add a custom DAP request to enumerate spawned children:

```
eta/childProcesses  →  { children: [{ pid, endpoint, modulePath, alive }] }
```

**`dap_server.h`** — add:
```cpp
void handle_child_processes(const Value& id, const Value& args);
```

**`dap_server.cpp`** — dispatch `"eta/childProcesses"` and iterate
`ProcessManager::children_` to build the response.  Each entry includes
the child's PID, the nng endpoint, the module path, and whether the
process is still alive.

---

## Phase 5 — Standard Library (`std/net.eta`)

**Goal:** Provide a high-level Eta module that wraps the raw nng primitives
into ergonomic, Erlang-inspired patterns.

### Module structure

```scheme
;; stdlib/std/net.eta
(define-module (std net)
  (export
    ;; Low-level nng
    nng-socket nng-listen nng-dial nng-close nng-socket?
    send! recv! nng-poll nng-subscribe nng-set-option

    ;; Actor model
    spawn spawn-kill spawn-wait current-mailbox

    ;; High-level patterns
    with-socket        ; dynamic-wind wrapper
    request-reply      ; synchronous request/reply
    worker-pool        ; fan-out to N workers
    pub-sub            ; publish/subscribe setup
    survey             ; scatter-gather query
    ))
```

### High-level helpers

```scheme
;; Safe socket management via dynamic-wind
(define (with-socket type thunk)
  (let ((sock #f))
    (dynamic-wind
      (lambda () (set! sock (nng-socket type)))
      (lambda () (thunk sock))
      (lambda () (when sock (nng-close sock))))))

;; Synchronous request-reply
(define (request-reply endpoint message)
  (with-socket 'req
    (lambda (sock)
      (nng-dial sock endpoint)
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
      (for-each nng-close workers)
      results)))

;; Pub/sub convenience
(define (pub-sub listen-endpoint topics handler)
  (with-socket 'sub
    (lambda (sock)
      (nng-dial sock listen-endpoint)
      (for-each (lambda (t) (nng-subscribe sock t)) topics)
      (let loop ()
        (handler (recv! sock))
        (loop)))))

;; Scatter-gather via SURVEYOR/RESPONDENT
(define (survey endpoint question timeout-ms)
  (with-socket 'surveyor
    (lambda (sock)
      (nng-listen sock endpoint)
      (nng-set-option sock 'survey-time timeout-ms)
      (send! sock question)
      ;; Collect responses until deadline
      (let loop ((responses '()))
        (let ((r (recv! sock)))
          (if r
              (loop (cons r responses))
              (reverse responses)))))))
```

### Acceptance Criteria

- [ ] `(import (std net))` works in Eta programs.
- [ ] `with-socket` cleans up on normal exit and on continuation escape.
- [ ] `request-reply` performs a synchronous round-trip.
- [ ] `worker-pool` distributes tasks to N workers and collects results.
- [ ] `survey` collects responses from multiple respondents.
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
namespace eta::nng {
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

- [x] Binary round-trip tests pass for all serializable types.
- [x] Binary format is faster than s-expression for large lists (0 ms vs 3 ms for a
      10,000-element vector in release; threshold < 10 ms).
- [ ] Tensor serialization works for float32 and float64 tensors.
- [x] `recv!` auto-detects binary vs text.

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
(spawn-thread thunk)   ; → returns an nng-socket (PAIR, inproc://)
```

Under the hood:

1. Create a `PAIR` socket pair on `inproc://eta-thread-<N>`.
2. Launch an `std::thread` that:
   - Creates a new `Heap`, `InternTable`, and `VM`.
   - Registers builtins (including nng primitives).
   - Executes `thunk` (a lambda/closure that has been **serialized** and
     deserialized into the new VM's heap).
3. The parent gets the parent-side socket; the child thread gets the
   child-side socket as its `current-mailbox`.

**Thread safety advantage:** Unlike ZeroMQ (which requires "one socket per
thread"), nng sockets are thread-safe by default.  This means the parent
thread can safely send to / receive from in-process actor sockets without
any additional synchronization beyond what nng already provides.

### Closure serialization challenge

Closures contain code pointers and upvalue references.  To send a closure
to another VM instance:

- **Code** is identified by function name + module.  Both VMs load the
  same module, so function pointers can be resolved by name.
- **Upvalues** are serialized as values (only serializable values —
  numbers, strings, symbols, lists, vectors).
- A closure referencing another closure or a continuation **cannot** be
  sent to a thread.
- **Anonymous lambdas** (those without a top-level `define` / `defun` name)
  **cannot** be serialized because there is no stable name to resolve in
  the child VM.  `spawn-thread` will raise a clear error:
  `"cannot serialize anonymous closure — use spawn-thread-with instead"`.
  Only named, module-level functions are valid `spawn-thread` targets.

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

### DAP integration (Phase 7)

The DAP server's `handle_threads()` currently hardcodes a single
`{"id": 1, "name": "main"}` thread.  With in-process actor threads, this
must be updated to enumerate all live actor threads:

```cpp
void DapServer::handle_threads(const Value& id, const Value& /*args*/) {
    Array threads;
    threads.push_back(json::object({{"id", 1}, {"name", "main"}}));
#ifdef ETA_HAS_NNG
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (driver_ && driver_->process_manager()) {
        int tid = 2;
        for (const auto& th : driver_->process_manager()->thread_handles()) {
            threads.push_back(json::object({
                {"id",   Value(static_cast<int64_t>(tid))},
                {"name", Value("actor-" + std::to_string(tid - 1))},
            }));
            ++tid;
        }
    }
#endif
    send_response(id, json::object({{"threads", Value(std::move(threads))}}));
}
```

> **Note:** each in-process actor has its own VM instance, so stepping /
> pausing a child thread requires forwarding DAP commands to the correct
> VM.  For v1 only the main thread is debuggable; child actor threads
> appear in the list for visibility but cannot be individually
> stepped/paused.  Full multi-VM debugging is deferred to post-v1.

### LSP integration (Phase 7)

Add `spawn-thread-with`, `spawn-thread`, `thread-join`, and
`thread-alive?` to the LSP's hover docs, completion items, signature help,
builtin names, and the tmLanguage grammar (all guarded by
`#ifdef ETA_HAS_NNG`).

### `ProcessManager` thread safety

Phase 4's `ProcessManager` uses a plain `std::vector<ChildHandle>` which
is not thread-safe.  Once Phase 7 introduces in-process threads that may
themselves call `spawn` or `spawn-thread-with`, concurrent access becomes
possible.

**V1 default:** Guard `ProcessManager` mutations with a `std::mutex`.  The
lock is only held during the brief `push_back` / `erase` operations on
the children vector — not during socket I/O or process launch — so
contention is negligible.

```cpp
class ProcessManager {
    // ...existing members...
    mutable std::mutex mu_;  // guards children_
};
```

All public methods (`spawn`, `kill`, `wait`, destructor) acquire `mu_`
before modifying `children_`.  nng sockets are already thread-safe, so no
additional synchronization is needed for socket operations.

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
monitored sockets and detects disconnection via `nng_pipe_notify()` — nng
provides pipe event callbacks (`NNG_PIPE_EV_ADD_POST`, `NNG_PIPE_EV_REM_POST`)
that fire when a peer connects or disconnects, making monitoring more
natural than with ZeroMQ.

### Heartbeats

```scheme
(enable-heartbeat sock interval-ms)   ; enable ping/pong on a socket
```

Heartbeat is implemented as an application-level periodic message
(`'__eta_ping__` / `'__eta_pong__`), using `nng_aio` with a deadline for
non-blocking timeout detection.

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

## Phase 9 — nng Port Adapter (Optional)

**Goal:** Bridge nng sockets into the Eta port system so that standard
`read` / `write` / `display` work on sockets.

### Design

```cpp
class NngPort : public Port {
public:
    NngPort(nng_socket socket, Mode mode);

    std::optional<char32_t> read_char() override;
    std::expected<void, RuntimeError> write_string(const std::string& str) override;
    std::expected<void, RuntimeError> close() override;
    bool is_open() const override;
    bool is_input() const override;
    bool is_output() const override;

private:
    nng_socket socket_;
    Mode mode_;
    std::string recv_buffer_;
    size_t recv_pos_{0};
};
```

This port buffers received messages and exposes them character-by-character
via `read-char`.  `write-string` accumulates data and sends it as an nng
message on `flush` or `close`.

### Primitives

```scheme
(nng-socket->input-port sock)    ; → input port reading from socket
(nng-socket->output-port sock)   ; → output port writing to socket
```

### Acceptance Criteria

- [ ] `(read (nng-socket->input-port sock))` reads an s-expression sent
      from the other end.
- [ ] `(write value (nng-socket->output-port sock))` sends the value.
- [ ] Port operations work with `with-input-from-port` /
      `with-output-to-port` parameter objects.

---

## Phase 10 — Documentation, Examples & Polish

### Tasks

1. **Documentation**
   - `docs/networking.md` — nng socket primitives reference.
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
     scatter-gather.eta       ; surveyor/respondent pattern
     parallel-map.eta         ; in-process thread pool
     monte-carlo.eta          ; parallel Monte Carlo simulation
     distributed-compute.eta  ; multi-machine example
   ```

3. **VS Code extension — snippets**

   **`editors/vscode/snippets/eta.json`** — add nng / actor snippets:

   ```json
   "NNG Socket": {
       "prefix": "nng-socket",
       "body": [
           "(define ${1:sock} (nng-socket '${2|pair,pub,sub,push,pull,req,rep,surveyor,respondent,bus|}))",
           "(nng-${3|listen,dial|} ${1:sock} \"${4:tcp://localhost:5555}\")",
           "$0"
       ],
       "description": "Create and listen/dial an nng socket"
   },
   "With Socket": {
       "prefix": "with-socket",
       "body": [
           "(with-socket '${1|pair,req,rep,pub,sub,push,pull,surveyor,respondent,bus|}",
           "  (lambda (${2:sock})",
           "    (nng-dial ${2:sock} \"${3:endpoint}\")",
           "    $0))"
       ],
       "description": "Safe socket with dynamic-wind cleanup"
   },
   "Spawn Worker": {
       "prefix": "spawn",
       "body": [
           "(define ${1:worker} (spawn \"${2:worker.eta}\"))",
           "(send! ${1:worker} ${3:'(task)})",
           "(define ${4:result} (recv! ${1:worker}))",
           "$0"
       ],
       "description": "Spawn a child process and send/receive a message"
   },
   "Send/Recv": {
       "prefix": "send-recv",
       "body": [
           "(send! ${1:sock} ${2:value})",
           "(define ${3:reply} (recv! ${1:sock}))",
           "$0"
       ],
       "description": "Send a message and wait for a reply"
   },
   "Worker Pool": {
       "prefix": "worker-pool",
       "body": [
           "(define ${1:results} (worker-pool \"${2:worker.eta}\" ${3:tasks}))",
           "$0"
       ],
       "description": "Distribute tasks across a pool of workers"
   },
   "Import Net": {
       "prefix": "import-net",
       "body": "(import std.net)",
       "description": "Import the networking / message-passing module"
   }
   ```

4. **VS Code extension — syntax highlighting**

   By this phase the tmLanguage grammar should include all nng, actor, and
   thread primitives.  Verify that the following groups are present in
   `editors/vscode/syntaxes/eta.tmLanguage.json` (added incrementally in
   Phases 3, 4, 7):

   ```json
   {
       "comment": "nng / message-passing builtins",
       "match": "(?<=\\()\\s*(nng-socket|nng-listen|nng-dial|nng-close|nng-socket\\?|send!|recv!|nng-poll|nng-subscribe|nng-set-option|spawn|spawn-kill|spawn-wait|current-mailbox|spawn-thread-with|spawn-thread|thread-join|thread-alive\\?|with-socket|request-reply|worker-pool|pub-sub|survey)(?=[\\s()\\[\\]])",
       "captures": {
           "1": { "name": "support.function.nng.eta" }
       }
   }
   ```

5. **VS Code extension — child process tree view**

   Add a new sidebar tree view visible during debug sessions that lists
   spawned child processes (from Phase 4's `eta/childProcesses` custom
   DAP request).

   **New file: `editors/vscode/src/childProcessTreeView.ts`**

   ```typescript
   // Tree data provider for spawned child Eta processes.
   // Polls eta/childProcesses on each stop event.

   export interface ChildProcessInfo {
       pid: number;
       endpoint: string;
       modulePath: string;
       alive: boolean;
   }

   export class ChildProcessNode { /* pid, endpoint, modulePath, alive */ }

   export class ChildProcessTreeProvider implements TreeDataProvider<ChildProcessNode> {
       // On notifyStopped(), call session.customRequest('eta/childProcesses')
       // and rebuild the tree.
   }
   ```

   **`editors/vscode/src/dapTypes.ts`** — add:
   ```typescript
   export interface ChildProcessInfo {
       pid: number;
       endpoint: string;
       modulePath: string;
       alive: boolean;
   }
   ```

   **`editors/vscode/src/extension.ts`** — register the new tree view:
   ```typescript
   const childProcProvider = new ChildProcessTreeProvider();
   context.subscriptions.push(
       window.createTreeView('etaChildProcesses', {
           treeDataProvider: childProcProvider,
           showCollapseAll: false,
       }),
   );
   ```
   Wire `childProcProvider.notifyStopped()` into the `EtaDebugAdapterTracker`'s
   `onDidSendMessage` handler alongside the existing `gcRootsProvider` and
   `disasmTreeProvider` notifications.

   **`editors/vscode/package.json`** — add the view contribution:
   ```json
   {
       "id": "etaChildProcesses",
       "name": "Child Processes",
       "when": "inDebugMode && debugType == 'eta'",
       "icon": "$(server-process)"
   }
   ```
   Add to `activationEvents`:
   ```json
   "onView:etaChildProcesses"
   ```
   Add a refresh command:
   ```json
   {
       "command": "eta.refreshChildProcesses",
       "title": "Refresh Child Processes",
       "icon": "$(refresh)",
       "category": "Eta"
   }
   ```

6. **LSP — completions for `(std net)` exports**

   The LSP's `load_completion_cache()` already scans all `.eta` files in
   the module search path and indexes their symbols.  Once `stdlib/std/net.eta`
   exists (Phase 5), the LSP will automatically offer completion items for
   `with-socket`, `request-reply`, `worker-pool`, `pub-sub`, `survey`, and all
   re-exported nng primitives — no LSP code changes needed beyond the
   builtin tables updated in Phases 3/4/7.

   Verify that `(import std.net)` resolves correctly in the validation
   pipeline (it will, because `preload_module_deps` resolves transitive
   imports from the module search path).

7. **DAP — heap view polish**

   Verify that nng socket objects display cleanly in the existing Heap
   Inspector webview.  The `renderSnapshot()` JavaScript already renders
   any `ObjectKind` string returned by the backend.  The `renderInspect()`
   function shows children — with the Phase 3 `expand_compound` changes,
   nng socket fields (protocol, listening, dialed) will appear naturally.

   No webview HTML/JS changes are needed — the existing code is generic
   enough.

8. **Performance benchmarks**
   - Message throughput: messages/sec for binary vs text format.
   - Latency: round-trip time for request-reply.
   - Scalability: throughput vs number of worker threads.

### Acceptance Criteria

- [ ] All examples compile and run.
- [ ] Documentation is complete and reviewed.
- [ ] VS Code extension includes nng snippets (`nng-socket`, `spawn`,
      `with-socket`, `send-recv`, `worker-pool`, `import-net`).
- [ ] tmLanguage grammar highlights all nng/actor/thread primitives.
- [ ] LSP provides hover docs, completions, and signature help for all
      nng/actor/thread primitives when built with `ETA_HAS_NNG`.
- [ ] LSP auto-discovers `(std net)` exports via module-path scanning.
- [ ] DAP Heap Inspector displays `NngSocket` objects with protocol/listening/dialed fields.
- [ ] DAP child process tree view lists spawned children with PID, endpoint, status.
- [ ] DAP `threads` response includes in-process actor threads (Phase 7).
- [ ] Benchmarks are documented with reproducible methodology.

---

## Summary of Files Changed/Added per Phase

| Phase | New Files | Modified Files |
|-------|-----------|----------------|
| 1 | `cmake/FetchNng.cmake`, `eta/nng/CMakeLists.txt`, `eta/nng/src/eta/nng/nng_socket_ptr.h` | `eta/CMakeLists.txt`, `CMakeLists.txt`, `eta/lsp/CMakeLists.txt` (conditional link), `eta/dap/CMakeLists.txt` (conditional link) |
| 2 | `eta/core/src/eta/runtime/datum_reader.h`, `eta/nng/src/eta/nng/wire_format.h` | — |
| 3 | `eta/nng/src/eta/nng/nng_primitives.h`, `eta/nng/src/eta/nng/nng_factory.h` | `eta/core/src/eta/runtime/memory/heap.h` (new ObjectKind), `eta/core/src/eta/runtime/builtin_names.h` (nng builtins), `eta/interpreter/src/eta/interpreter/driver.h`, `eta/lsp/src/eta/lsp/lsp_server.cpp` (hover/completion/signatureHelp tables), `eta/dap/src/eta/dap/dap_server.cpp` (`expand_compound` + `is_compound_value` for NngSocket), `editors/vscode/syntaxes/eta.tmLanguage.json` (nng builtins) |
| 4 | `eta/nng/src/eta/nng/process_mgr.h` | `eta/nng/src/eta/nng/nng_primitives.h`, `eta/lsp/src/eta/lsp/lsp_server.cpp` (spawn/mailbox builtins), `eta/dap/src/eta/dap/dap_server.h` (`handle_child_processes`), `eta/dap/src/eta/dap/dap_server.cpp` (dispatch + handler), `editors/vscode/syntaxes/eta.tmLanguage.json` (spawn builtins) |
| 5 | `stdlib/std/net.eta` | `stdlib/prelude.eta` (optional auto-import) |
| 6 | — | `eta/nng/src/eta/nng/wire_format.h` (add binary format) |
| 7 | — | `eta/nng/src/eta/nng/nng_primitives.h`, `eta/nng/src/eta/nng/process_mgr.h`, `eta/dap/src/eta/dap/dap_server.cpp` (`handle_threads` multi-VM), `eta/lsp/src/eta/lsp/lsp_server.cpp` (thread builtins), `editors/vscode/syntaxes/eta.tmLanguage.json` (thread builtins) |
| 8 | `stdlib/std/supervisor.eta` | `eta/nng/src/eta/nng/nng_primitives.h` |
| 9 | `eta/nng/src/eta/nng/nng_port.h` | `eta/nng/src/eta/nng/nng_primitives.h` |
| 10 | `docs/networking.md`, `docs/message-passing.md`, `examples/*.eta`, `editors/vscode/src/childProcessTreeView.ts` | `docs/examples.md`, `docs/modules.md`, `editors/vscode/snippets/eta.json` (nng snippets), `editors/vscode/package.json` (child process tree view + activation event + command), `editors/vscode/src/extension.ts` (register tree view + tracker wiring), `editors/vscode/src/dapTypes.ts` (ChildProcessInfo) |

---

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| Blocking `recv!` freezes VM | High | **Default 1 s timeout** on all sockets; `'wait` flag for opt-in indefinite blocking; document `nng-poll` for event-loop patterns |
| Closure serialization is complex | Medium | Phase 7a uses name-based dispatch; defer full closure serialization; anonymous lambdas explicitly rejected with clear error |
| Child process zombies | Medium | Heartbeat (Phase 8); `ProcessManager` destructor kills all children; register SIGTERM/SIGINT handler to invoke destructor; nng pipe-notify callbacks for instant disconnect detection |
| Large message serialization overhead | Low | Binary format (Phase 6) minimizes overhead; tensor zero-copy path |
| `dynamic-wind` confusion with sockets | Low | Document clearly that sockets are not rewound; recommend `with-socket` |
| `ProcessManager` concurrent access | Medium | Guard `children_` with `std::mutex` (Phase 7); lock held only during vector mutation, not I/O; nng sockets are thread-safe by default |
| Remote spawning not automated | Low | V1 uses raw sockets for cross-host; `spawn-remote` deferred to post-v1 |
| nng API stability | Low | Pin to specific release tag (v1.9.0); nng has been stable since v1.0 with backward-compatible releases |

---

## V1 Scope Boundaries

The following features are **explicitly out of scope** for the initial
implementation.  They are natural extensions that can be layered on top of
the v1 foundation without breaking changes.

| Feature | Rationale | Future Path |
|---------|-----------|-------------|
| **Remote process spawning** (`spawn-remote`) | Requires SSH integration or a distributed node agent.  V1 supports cross-host messaging via raw `tcp://` sockets; remote processes are started independently. | Post-v1: a `spawn-remote` primitive that SSHs into a host and launches `etai`, or an opt-in discovery service similar to Erlang's EPMD. |
| **Actor name registry** | Erlang's `register/2` and `whereis/1` provide process lookup by name.  V1 requires knowing the endpoint or holding the socket handle directly. | Post-v1: a `register-actor` / `lookup-actor` API backed by a name table in the parent process or a dedicated registry service over `tcp://`. |
| **Distributed GC / remote references** | Cross-process garbage collection of remote handles adds significant complexity. | Not planned.  Actors manage their own lifecycle; dead references are detected via monitor / heartbeat. |
| **Full closure serialization for anonymous lambdas** | Anonymous closures have no stable name resolvable in a child VM.  Serializing code by bytecode index is fragile across recompilation. | Phase 7b supports named closures only.  Anonymous closures must use `spawn-thread-with` name-based dispatch. |
| **WebSocket / TLS transports** | nng supports pluggable transports; `wss://` and `tls+tcp://` can be added later. | Post-v1: enable `nng_tls` transport for encrypted communication; potentially add WebSocket transport for browser interop. |
