# Actor Model Improvement Plan

[Back to README](../../README.md) ·
[Stdlib Reference](../stdlib.md) ·
[Architecture](../architecture.md) ·
[Language Guide](../language_guide.md)

> **Status.** Authoritative implementation plan for moving Eta's actor
> story from the current nng-socket/process-spawn abstraction to an
> Erlang-class actor runtime: true process identities, per-actor
> mailboxes, selective receive, links, monitors, exit signals,
> supervisors, named registration, distribution, introspection, and
> OTP-style behaviours.
>
> Current reality: Eta exposes `spawn`, `spawn-thread`, `current-mailbox`,
> `send!`, `recv!`, `monitor`, and a small `std.supervisor` layer, but the
> "mailbox" is currently an nng PAIR socket (or parent-side socket), not a
> VM-level mailbox owned by an actor. That makes the API useful for simple
> message passing, but not yet on par with Erlang.

---

## 0) Executive summary

Eta should split its concurrency stack into two layers:

1. **`std.actor` / runtime actor system** — Erlang-like local actors,
   PIDs, real mailboxes, selective receive, links, monitors, exit
   signals, supervision, process registry, and introspection. This is the
   primary actor model.
2. **`std.net` / nng transport** — message bus and distribution backend
   for remote actors, pub/sub, request/reply, and external peers. nng
   remains valuable, but it should not be the in-memory mailbox
   abstraction.

The key design shift is:

> **Actors communicate with PIDs, not sockets.**
>
> nng sockets become an implementation detail for remote node transport,
> not the public identity of a local actor.

Recommended high-level sequence:

1. Build a local VM-level actor system with `pid`, `self`, `spawn`,
   `send`, `receive`, selective receive, and timeouts.
2. Add links, monitors, exit propagation, and trap-exit semantics.
3. Replace the current `std.supervisor` implementation with proper
   restart strategies and child specs.
4. Add OTP-style behaviours: `gen-server`, `gen-event`, and eventually
   `gen-stat(e)m`.
5. Bridge remote actors over nng using encoded PIDs and node routing.
6. Migrate `std.net` examples to use `std.actor` where appropriate while
   preserving low-level nng APIs for transport-level examples.

---

## 1) Current state in the repository

### 1.1 Public APIs today

| Surface | File | Current behaviour |
| ------- | ---- | ----------------- |
| `std.net` | `stdlib/std/net.eta` | High-level wrappers over nng sockets: `with-socket`, `request-reply`, `worker-pool`, `pub-sub`, `survey`. |
| `std.supervisor` | `stdlib/std/supervisor.eta` | Small supervisor loops built on `spawn`, `monitor`, `nng-poll`, `recv!`. Supports `one-for-one` and `one-for-all` only. |
| nng primitives | `packages/stdlib/native/nng/src/eta/nng/nng_primitives.h` | Registers `nng-socket`, `send!`, `recv!`, `spawn`, `spawn-kill`, `spawn-wait`, `current-mailbox`, `spawn-thread`, `thread-join`, `monitor`, `demonitor`, `enable-heartbeat`. |
| process manager | `packages/stdlib/native/nng/src/eta/nng/process_mgr.h` | Spawns OS child processes connected by nng PAIR sockets; can spawn in-process VM threads connected by nng `inproc://` PAIR sockets. |
| language docs | `docs/language_guide.md` | Describes "every actor owns a mailbox socket" and says same `send!`/`recv!` works for in-process threads, OS processes, remote peers. |

### 1.2 What exists and is worth keeping

1. **Cross-process and in-process spawning** already exists via
   `ProcessManager`.
2. **Serialization of Eta values** already exists for `send!` / `recv!`,
   including checks that VM-local AD tape values do not cross VM
   boundaries.
3. **nng transports** already cover `inproc://`, IPC, and TCP.
4. **Monitoring-ish callbacks** already exist for nng pipe disconnects.
5. **Heartbeat support** exists for socket liveness.
6. **Debug integration hooks** exist in `ProcessManager` for DAP child VM
   inspection.

These are strong foundations for distribution and tooling, but not
sufficient for Erlang-like actor semantics.

### 1.3 Gaps against Erlang

| Erlang capability | Eta today | Gap |
| ----------------- | --------- | --- |
| PID identity | Socket handle | Sockets are transport endpoints, not stable process identities. |
| `self()` | `current-mailbox` in children only | No universal current PID. |
| Per-process mailbox | nng socket receive queue | No VM-level mailbox, no queue introspection, no selective receive. |
| Send semantics | `send! socket value` | No send by PID/name; no local fast path; no delivery failure semantics. |
| Selective receive | None | Cannot scan mailbox by pattern/guard and preserve unmatched messages. |
| Receive timeout | Socket timeout / `'wait` | No `(receive ... after ...)` semantics. |
| Links | None | No bidirectional failure propagation. |
| Monitors | nng pipe callbacks | No ref-based process monitors independent of transport. |
| Exit signals | `spawn-kill` / process exit | No structured reasons, no `trap-exit`, no `EXIT` messages. |
| Supervision | Small polling loops | No child specs, restart intensity, transient/permanent/temporary restart, shutdown strategy. |
| Registered names | None | No `register`, `whereis`, global registry. |
| Remote nodes | nng manual endpoints | No node identity, remote PID encoding, cookie/auth, transparent send. |
| Scheduler | OS thread/process per actor | Not lightweight enough for Erlang-scale actor counts. |
| Reduction counting | None | No fair preemption of long-running actor code. |
| Introspection | minimal | No `process-info`, mailbox length, links, monitors, reductions, status. |
| OTP behaviours | None | No `gen_server`, `gen_event`, `gen_statem`, supervisor specs. |

---

## 2) Target semantics

### 2.1 Core principles

1. **Actor isolation.** Actors do not share mutable Eta heap objects.
   Sending a message copies/serializes immutable Eta values across actor
   boundaries. Future optimisation can use copy-on-write or immutable
   ref-counted blobs, but the semantic baseline is isolation.
2. **PID-based addressing.** A PID identifies an actor regardless of how
   it is implemented: lightweight local actor, OS child actor, or remote
   actor on another node.
3. **Per-actor FIFO mailbox.** Messages sent from a single sender to a
   single receiver preserve send order. Different senders may interleave.
4. **Selective receive.** Receiving scans the mailbox for the first
   message matching one of the receive clauses; unmatched messages remain
   in order for later receives.
5. **Failure is data.** Exits carry reasons; links propagate exits;
   monitors deliver `DOWN` messages; trapping exits converts linked exit
   signals into ordinary messages.
6. **Let it crash.** Supervisors restart failed children according to
   declared child specs and restart intensity limits.
7. **Distribution is transparent where safe.** Sending to remote PIDs uses
   the same API as local sends, but errors are explicit and security is
   opt-in via node cookies / TLS later.
8. **nng remains the transport layer.** Use nng for remote node channels
   and external message patterns, not as the local mailbox representation.

### 2.2 Compatibility targets

Eta does **not** need to clone Erlang syntax or every BEAM feature, but
it should match the operational model closely enough that Erlang users
recognise it.

Required v1 parity:

- `spawn`, `spawn-link`, `spawn-monitor`
- `self`
- PID values and `pid?`
- `send` / `!` equivalent
- mailbox-backed `receive` with timeout
- selective receive by predicate and by pattern helper
- `link`, `unlink`
- `monitor`, `demonitor`, monitor refs
- `exit`, `kill`, `trap-exit`
- `process-info`
- local name registry: `register`, `unregister`, `whereis`
- supervisor child specs and strategies
- `gen-server` behaviour

Deferred but planned:

- remote node discovery and transparent remote PID send
- global name registry
- hot code upgrade
- distributed monitors/links across nodes
- scheduler-level soft real-time guarantees
- ETS-like shared tables
- full OTP application packaging

---

## 3) New public module layout

### 3.1 New modules

| Module | File | Purpose |
| ------ | ---- | ------- |
| `std.actor` | `stdlib/std/actor.eta` | Core PID, spawn, send, receive, links, monitors, registry, process info. |
| `std.actor.mailbox` | `stdlib/std/actor/mailbox.eta` | Higher-level receive combinators, pattern helpers, mailbox debugging. |
| `std.actor.supervisor` | `stdlib/std/actor/supervisor.eta` | Erlang-style child specs, restart strategies, restart intensity. |
| `std.actor.gen_server` | `stdlib/std/actor/gen_server.eta` | OTP-like server behaviour: `start-link`, `call`, `cast`, callbacks. |
| `std.actor.gen_event` | `stdlib/std/actor/gen_event.eta` | Event manager behaviour. |
| `std.actor.registry` | `stdlib/std/actor/registry.eta` | Local registry API and optional process-group helpers. |
| `std.actor.node` | `stdlib/std/actor/node.eta` | Remote node connect/listen/handshake/routing over nng. |

### 3.2 Compatibility modules

| Existing module | Action |
| --------------- | ------ |
| `std.net` | Keep low-level nng/message-bus helpers. Add compatibility wrappers that can send to PIDs when given PIDs, but do not make it the actor API. |
| `std.supervisor` | Keep as a deprecated shim importing `std.actor.supervisor`; emit docs warning. |
| Raw `send!` / `recv!` | Keep for nng sockets. Add new PID APIs named `send` / `receive` to avoid overloading old socket semantics. |
| `current-mailbox` | Deprecate for actor code. New name is `self`. `current-mailbox` can return the current PID in actor contexts in compatibility mode, but docs should steer users to `self`. |

---

## 4) Runtime architecture

### 4.1 New core runtime files

| Concern | Path | Action |
| ------- | ---- | ------ |
| Actor system owner | `eta/core/src/eta/runtime/actor/actor_system.{h,cpp}` | NEW |
| PID representation | `eta/core/src/eta/runtime/actor/pid.{h,cpp}` | NEW |
| Mailbox queue | `eta/core/src/eta/runtime/actor/mailbox.{h,cpp}` | NEW |
| Actor process record | `eta/core/src/eta/runtime/actor/actor_process.{h,cpp}` | NEW |
| Scheduler | `eta/core/src/eta/runtime/actor/scheduler.{h,cpp}` | NEW |
| Links / monitors | `eta/core/src/eta/runtime/actor/failure.{h,cpp}` | NEW |
| Registry | `eta/core/src/eta/runtime/actor/registry.{h,cpp}` | NEW |
| Distribution bridge | `eta/core/src/eta/runtime/actor/node_transport.{h,cpp}` | NEW (M6.1) |
| Builtin primitives | `eta/core/src/eta/runtime/actor/actor_primitives.{h,cpp}` | NEW |
| Builtin catalog | `eta/core/src/eta/runtime/builtin_catalog.cpp` | EDIT |
| Builtin metadata | `eta/core/src/eta/runtime/builtin_metadata.cpp` | EDIT |
| Heap object kinds | `eta/core/src/eta/runtime/types/types.h` and heap factory files | EDIT for `Pid`, `MonitorRef`, possibly `ActorHandle` |
| Driver wiring | `eta/session/*` / driver setup files | EDIT to allocate one `ActorSystem` per interpreter/session |
| nng package bridge | `packages/stdlib/native/nng/src/eta/nng/*` | EDIT in M6.1 only, for remote transport bridge |

### 4.2 PID representation

A PID is an opaque Eta heap object with printable form similar to:

```text
#<pid 0.42.3>
#<pid eta@host:0.42.3>
```

Internal fields:

```cpp
struct Pid {
    uint64_t node_id;        // 0 for local node until distribution lands
    uint64_t actor_id;       // monotonically increasing per ActorSystem
    uint32_t incarnation;    // incremented if actor_id slot is recycled
};
```

Equality is structural: same `node_id`, `actor_id`, `incarnation`.
PIDs are serializable so they can be sent in messages. Remote PIDs use a
non-zero `node_id` and route through `NodeTransport`.

### 4.3 Mailbox representation

Each actor owns a `Mailbox`:

```cpp
struct MailboxMessage {
    Pid sender;
    LispVal payload;          // local actor fast path stores copied value
    uint64_t sequence;        // per-sender ordering diagnostics
    TimePoint enqueue_time;
};

class Mailbox {
public:
    void push(MailboxMessage msg);
    std::optional<MailboxMessage> pop_front();
    std::optional<MailboxMessage> selective_pop(Matcher matcher);
    size_t size() const;
    void close(ExitReason reason);
};
```

Implementation notes:

1. Use a mutex + condition variable initially; optimise to MPSC later.
2. Keep messages in a `std::deque` to support selective receive scanning.
3. `selective_pop` scans from oldest to newest and removes the first
   matching message; unmatched messages preserve their original order.
4. Track high-water marks and optional mailbox limits for diagnostics and
   backpressure.
5. Actor mailboxes are not nng sockets. nng receives can enqueue into a
   mailbox for remote distribution, but local actors do not poll sockets.

### 4.4 Actor process record

```cpp
enum class ActorState {
    Runnable,
    WaitingReceive,
    WaitingTimeout,
    Exiting,
    Dead
};

struct ActorProcess {
    Pid pid;
    Pid parent;
    std::string registered_name;
    Mailbox mailbox;
    ActorState state;
    bool trap_exit = false;
    std::unordered_set<Pid> links;
    std::unordered_map<MonitorRef, Pid> monitors_out;
    std::unordered_map<MonitorRef, Pid> monitors_in;
    ExitReason exit_reason;
    uint64_t reductions = 0;
    uint64_t messages_received = 0;
    uint64_t messages_sent = 0;
    // VM / closure / module entrypoint pointers for runnable actors.
};
```

### 4.5 Spawn implementation choices

Eta has two possible actor execution models:

1. **MVP: one OS thread + independent VM per actor.** This matches the
   existing `spawn-thread` direction and is easiest to implement safely.
   It will not scale to hundreds of thousands of actors, but it gets
   correct Erlang semantics first.
2. **Future: cooperative lightweight actors scheduled by reduction
   counting.** Multiple actors share an OS-thread scheduler pool, each
   actor has its own continuation/VM stack, and the VM yields after a
   reduction budget.

This plan deliberately stages these:

- M1–M4: correct semantics on one-VM-per-actor.
- M7: scheduler and reductions for large actor counts.

Correct semantics first; BEAM-scale scheduling second.

### 4.6 Scheduler MVP

MVP scheduler is an `ActorSystem` owner with:

1. A registry of live actors by PID.
2. A thread pool or one thread per actor, configurable by
   `ETA_ACTOR_SCHEDULER=thread-per-actor|pool`.
3. Blocking receive implemented by condition-variable wait on the
   actor's mailbox.
4. Timers implemented by `std::chrono` deadline + condition-variable
   timed wait.
5. Actor-local TLS pointer: `current_actor_pid`, `current_actor_system`.

### 4.7 Scheduler v2

Later, replace thread-per-actor with BEAM-like reductions:

1. VM instruction loop decrements a reduction counter per opcode / call.
2. Actor yields when counter reaches zero.
3. Scheduler has N run queues (one per scheduler thread), work stealing,
   IO/timer wait queues, and dirty scheduler threads for blocking native
   calls.
4. Builtins declare whether they are blocking; blocking builtins run on
   dirty schedulers or explicitly yield.

This is necessary for "Erlang-scale" actor counts, but it should not
block the semantic MVP.

---

## 5) Builtin primitive surface

### 5.1 Core primitives

Registered in `actor_primitives.{h,cpp}` and mirrored in
`builtin_catalog.cpp` / `builtin_metadata.cpp`.

| Primitive | Arity | Public wrapper | Description |
| --------- | :---: | -------------- | ----------- |
| `%actor-self` | 0 | `self` | Current actor PID; error outside an actor unless main process is registered as an actor. |
| `%actor-pid?` | 1 | `pid?` | Predicate. |
| `%actor-spawn` | variadic | `spawn` | Spawn a thunk or module/function as an actor; returns PID. |
| `%actor-spawn-link` | variadic | `spawn-link` | Spawn then link atomically. |
| `%actor-spawn-monitor` | variadic | `spawn-monitor` | Spawn and monitor atomically; returns `(pid . ref)`. |
| `%actor-send` | 2/3 | `send`, `!` | Send payload to PID or registered name. Optional sender override for system messages. |
| `%actor-receive` | 2 | `receive` | Receives by matcher object + timeout. The Eta wrapper provides clause syntax/combinators. |
| `%actor-mailbox-len` | 0/1 | `mailbox-length` | Current or given PID mailbox size. |
| `%actor-process-info` | 1/2 | `process-info` | Return alist of status, links, monitors, mailbox length, reductions, registered name. |
| `%actor-exit` | 2 | `exit` | Send exit signal to PID with reason. |
| `%actor-kill` | 1/2 | `kill` | Untrappable kill. |
| `%actor-trap-exit!` | 1 | `trap-exit!` | Set current actor trap-exit flag. |
| `%actor-link` | 1 | `link` | Link current actor to target PID. |
| `%actor-unlink` | 1 | `unlink` | Remove link. |
| `%actor-monitor` | 1 | `monitor` | Monitor target PID; returns monitor ref. |
| `%actor-demonitor` | 1/2 | `demonitor` | Remove monitor; optional `flush` to remove queued DOWN. |
| `%actor-register` | 2 | `register` | Bind symbol/name to PID. |
| `%actor-unregister` | 1 | `unregister` | Remove name. |
| `%actor-whereis` | 1 | `whereis` | Resolve name to PID or `#f`. |
| `%actor-registered` | 0 | `registered` | List registered names. |
| `%actor-sleep` | 1 | `sleep` | Actor-friendly sleep/yield; does not block scheduler v2. |
| `%actor-yield` | 0 | `yield` | Voluntary scheduler yield. |

### 5.2 Exit reasons

Exit reasons are ordinary Eta values, with conventional symbols:

| Reason | Meaning |
| ------ | ------- |
| `'normal` | Normal completion. Does not kill linked processes unless explicitly sent. |
| `'shutdown` | Supervisor-initiated graceful shutdown. |
| `'(shutdown detail)` | Structured shutdown. |
| `'kill` | Untrappable kill, translated to killed reason at receiver. |
| `'(error message)` | Unhandled runtime error. |
| `'(throw value)` | User exception if Eta exposes throw/catch metadata. |

### 5.3 Monitor messages

Monitor messages follow Erlang shape with Eta symbols:

```scheme
'(DOWN ref process pid reason)
```

Example:

```scheme
(let-values (((pid ref) (spawn-monitor worker)))
  (receive
    ((list 'DOWN ref 'process pid reason) reason)
    (after 5000 'timeout)))
```

### 5.4 Link exit messages when trapping exits

If `trap-exit!` is true, linked exits are delivered as ordinary mailbox
messages:

```scheme
'(EXIT from-pid reason)
```

If `trap-exit!` is false and the reason is not `'normal`, the actor
exits with the same reason and propagates it to its own links.

---

## 6) Eta-level API design

### 6.1 `std.actor`

```scheme
(module std.actor
  (export
    ;; identity
    self pid? alive?

    ;; spawn
    spawn spawn-link spawn-monitor
    spawn-module spawn-module-link spawn-module-monitor

    ;; messaging
    send ! receive receive-after receive-match
    mailbox-length flush-mailbox!

    ;; failure
    exit kill trap-exit! link unlink monitor demonitor

    ;; registry
    register unregister whereis registered

    ;; introspection
    process-info processes

    ;; utility
    sleep yield)
  ...)
```

### 6.2 Send examples

```scheme
(import std.actor)

(define pid
  (spawn
    (lambda ()
      (let loop ()
        (receive
          (('ping from) (send from 'pong) (loop))
          (('stop) 'ok))))))

(send pid (list 'ping (self)))
(receive
  (('pong) 'ok)
  (after 1000 'timeout))
```

Alias form:

```scheme
(! pid '(work 42))
```

### 6.3 Receive API options

Eta may not have Erlang syntax macros today, so provide both a function
API and a macro/sugar API if the macro system allows it.

#### Function API MVP

```scheme
(receive-match
  (list
    (case (lambda (msg) (and (pair? msg) (eq? (car msg) 'ping)))
          (lambda (msg) 'got-ping))
    (case (lambda (msg) (equal? msg 'stop))
          (lambda (msg) 'stopped)))
  5000
  (lambda () 'timeout))
```

#### Preferred macro form

```scheme
(receive
  (('ping from) (send from 'pong) (loop))
  (('stop) 'ok)
  ((? number? n) (* n 2))
  (after 5000 'timeout))
```

Macro expansion creates matcher closures and passes them to
`%actor-receive`.

### 6.4 Selective receive semantics

Given mailbox:

```scheme
'(a 1), '(b 2), '(a 3)
```

and receive:

```scheme
(receive
  (('b x) x))
```

Result is `2`, and mailbox afterward is:

```scheme
'(a 1), '(a 3)
```

This exact behaviour is required and tested.

### 6.5 Main process as an actor

On interpreter startup, wrap the main VM in an actor record:

1. `self` works in top-level scripts and REPL.
2. Main has PID `#<pid 0.0.0>` or similar.
3. Actors spawned by main link/monitor against main normally.
4. `receive` works in the main process.

This avoids confusing "outside actor context" failures and makes Eta
feel Erlang-like from the first example.

---

## 7) Supervision plan

### 7.1 Replace `std.supervisor` with `std.actor.supervisor`

`std.supervisor` today is a polling loop over child sockets and a
minimal `(down ...)` message convention. Replace it with supervisor
processes that use links, monitor refs, and exit reasons.

### 7.2 Child spec

Represent child specs as alists initially:

```scheme
(child-spec
  'id          'worker-1
  'start       (lambda () (worker-loop))
  'restart     'permanent       ; permanent | transient | temporary
  'shutdown    5000             ; ms | 'brutal-kill | 'infinity
  'type        'worker          ; worker | supervisor
  'modules     '(my.worker))
```

Convenience constructor:

```scheme
(make-child-spec id start
  'restart 'permanent
  'shutdown 5000
  'type 'worker)
```

### 7.3 Strategies

Implement Erlang/OTP-style strategies:

| Strategy | Semantics |
| -------- | --------- |
| `one-for-one` | Restart only the failed child. |
| `one-for-all` | Terminate all children, restart all. |
| `rest-for-one` | Terminate/restart failed child and children started after it. |
| `simple-one-for-one` | Dynamic homogeneous children; optional, can be deferred. |

### 7.4 Restart intensity

Supervisor tracks restarts over a time window:

```scheme
(supervisor-start-link
  'strategy 'one-for-one
  'max-restarts 3
  'max-seconds 5
  'children specs)
```

If more than `max-restarts` occur within `max-seconds`, supervisor exits
with reason:

```scheme
'(shutdown restart-intensity-exceeded)
```

This propagates upward through links to parent supervisors.

### 7.5 Shutdown order

1. Send `exit child '(shutdown supervisor-shutdown)`.
2. Wait `shutdown` milliseconds for child to exit.
3. If still alive, `kill` child.
4. For `one-for-all` and `rest-for-one`, shut down in reverse start order.

### 7.6 Supervisor API

```scheme
(module std.actor.supervisor
  (export
    child-spec make-child-spec
    supervisor-start supervisor-start-link
    supervisor-pid? supervisor-which-children
    supervisor-count-children
    supervisor-start-child supervisor-terminate-child
    supervisor-restart-child supervisor-delete-child)
  ...)
```

### 7.7 Migration shim

`stdlib/std/supervisor.eta` becomes:

```scheme
(module std.supervisor
  (export one-for-one one-for-all)
  (import std.actor.supervisor)
  ... compatibility wrappers ...)
```

Docs mark it deprecated in favour of `std.actor.supervisor`.

---

## 8) OTP-style behaviours

### 8.1 `std.actor.gen_server`

API:

```scheme
(gen-server-start module init-arg opts)
(gen-server-start-link module init-arg opts)
(gen-server-call server request timeout-ms)
(gen-server-cast server message)
(gen-server-stop server reason timeout-ms)
```

Callback contract in a server module:

```scheme
(define (init arg)              ; -> '(ok state) | '(stop reason)
  ...)

(define (handle-call request from state)
  ;; -> '(reply reply new-state)
  ;; -> '(noreply new-state)
  ;; -> '(stop reason reply new-state)
  ...)

(define (handle-cast message state)
  ;; -> '(noreply new-state) | '(stop reason new-state)
  ...)

(define (handle-info message state)
  ;; ordinary unmatched mailbox messages
  ...)

(define (terminate reason state)
  ...)
```

Message protocol:

```scheme
'(gen-call ref from request)
'(gen-cast message)
```

Replies:

```scheme
'(gen-reply ref value)
```

### 8.2 `std.actor.gen_event`

Event manager with dynamic handlers:

```scheme
(gen-event-start-link opts)
(gen-event-add-handler manager handler-module init-arg)
(gen-event-delete-handler manager handler-id reason)
(gen-event-notify manager event)
(gen-event-sync-notify manager event timeout-ms)
```

### 8.3 `std.actor.gen_statem` / `gen_fsm`

Defer until after `gen_server` and supervisor are stable. State machines
need better pattern syntax and callback docs.

---

## 9) Distribution plan

### 9.1 Use nng for node transport

nng remains the right dependency for node-to-node transport:

1. Listener socket per node.
2. Auth handshake with node name + cookie.
3. Version negotiation and feature flags.
4. Multiplex logical actor messages over one or more nng sockets.
5. Heartbeats reuse existing `enable-heartbeat` concepts.

### 9.2 Node identity

```scheme
(node-name)              ; => 'eta@hostname
(node-listen endpoint opts)
(node-connect endpoint opts)
(nodes)                  ; connected nodes
(disconnect-node node)
```

Node IDs are assigned during handshake and stored in `ActorSystem`.

### 9.3 Remote PID encoding

Remote PID serialized form:

```scheme
#(pid node-name actor-id incarnation creation)
```

Routing table maps `node-name` / `node_id` to a `NodeConnection`.

### 9.4 Remote send

`send` checks PID locality:

1. Local PID → enqueue directly into mailbox.
2. Remote PID → serialize message envelope and send via `NodeTransport`.
3. Unknown node → return `#f` or raise based on option; default Erlang-like
   `send` returns message value even if delivery cannot be proven, but Eta
   should also provide `send/checked` returning delivery status.

Message envelope:

```scheme
'(actor-msg from-pid to-pid payload sequence)
```

### 9.5 Remote monitors and links

MVP distribution supports remote `send` only. Later phases add:

1. Remote monitors using `(monitor from-ref from-pid target-pid)` control
   messages and `DOWN` replies.
2. Remote links using link handshake and node-down propagation.
3. Node monitors: `monitor-node`, `nodeup`, `nodedown` messages.

### 9.6 Security

MVP:

1. Shared cookie, modelled after Erlang cookie.
2. Cookie never printed in logs.
3. Reject node connections with bad cookie.

Future:

1. TLS transport once the libcurl plan's TLS dependency story is settled
   or nng TLS is configured reliably.
2. Per-node ACLs.
3. Signed package/node identities.

---

## 10) Error handling and failure semantics

### 10.1 Normal actor completion

If an actor thunk returns normally, actor exits with reason `'normal`.
Linked actors are not killed by a normal exit unless the exiting actor was
explicitly killed or the link was to a process that treats normal as
shutdown (supervisor children generally report normal to supervisor).

### 10.2 Unhandled Eta error

Unhandled runtime error exits actor with:

```scheme
'(error <message> <metadata>)
```

The metadata alist may include source location, stack trace, primitive
name, and AD error tags if present.

### 10.3 Exit propagation

When actor `A` exits with non-normal reason:

1. For each linked actor `B`:
   - if `B.trap_exit = #t`, enqueue `'(EXIT A reason)` into `B`'s mailbox.
   - otherwise, recursively exit `B` with the same reason.
2. For each monitor registered by actor `M`, enqueue
   `'(DOWN ref process A reason)` to `M`.
3. Remove actor from registry if it was registered.
4. Mark actor dead and release resources.

### 10.4 `kill`

`kill` sends untrappable reason `'kill`; target receives effective reason
`'killed` in monitor messages, matching Erlang's distinction.

---

## 11) Backpressure and mailbox limits

Erlang mailboxes are unbounded in practice, but Eta should expose limits
because it will run in smaller local processes and notebooks.

Actor options:

```scheme
(spawn worker
  'mailbox-limit 100000
  'on-mailbox-full 'block)     ; block | drop-new | drop-old | error
```

Default: unbounded for compatibility, with warnings when length exceeds
`ETA_ACTOR_MAILBOX_WARN` (default 10,000).

Expose:

```scheme
(mailbox-length pid)
(process-info pid 'message-queue-len)
(process-info pid 'message-queue-high-watermark)
```

---

## 12) Tooling and observability

### 12.1 Process info

`process-info` returns an alist:

```scheme
'((pid . #<pid 0.42.0>)
  (registered-name . worker-1)
  (state . waiting-receive)
  (message-queue-len . 12)
  (message-queue-high-watermark . 302)
  (links . (#<pid 0.1.0>))
  (monitors . (...))
  (monitored-by . (...))
  (reductions . 18432)
  (heap-bytes . 1048576)
  (started-at . 123456789)
  (current-function . worker-loop))
```

### 12.2 Actor listing

```scheme
(processes)          ; all local PIDs
(processes 'remote)  ; optional, remote known PIDs
```

### 12.3 DAP integration

Extend existing DAP child process tree:

1. Show actor tree grouped by supervisor.
2. Show PID, registered name, mailbox length, state.
3. Allow inspecting mailbox (debug mode only; destructive receive is not
   allowed).
4. Allow kill/exit actor from debugger.

### 12.4 Profiling

`std.prof` should include actor dimensions:

1. Reductions by actor.
2. Mailbox latency histogram.
3. Messages sent/received per actor.
4. Scheduler run queue length.
5. Blocking native calls by actor.

---

## 13) Documentation deliverables

| Document | Action |
| -------- | ------ |
| `docs/stdlib/actor.md` | NEW: core actor API reference. |
| `docs/stdlib/actor-supervisor.md` | NEW: supervisor API, strategies, child specs. |
| `docs/stdlib/actor-gen-server.md` | NEW: gen-server behaviour guide. |
| `docs/stdlib/net.md` | EDIT: clarify nng is transport/message bus, not the primary actor mailbox. |
| `docs/language_guide.md` | EDIT: replace current concurrency section with true actor model description. |
| `docs/architecture.md` | EDIT: add actor runtime architecture and nng distribution bridge. |
| `docs/build.md` | EDIT: document build flags if actor runtime can be toggled. |
| `docs/release-notes.md` | EDIT: migration notes and deprecations. |

Cookbook examples:

| Example | Purpose |
| ------- | ------- |
| `cookbook/concurrency/actor-ping-pong.eta` | Basic `spawn`, `send`, `receive`. |
| `cookbook/concurrency/selective-receive.eta` | Demonstrates mailbox scanning and unmatched preservation. |
| `cookbook/concurrency/linked-failure.eta` | `spawn-link`, `trap-exit!`, `EXIT` messages. |
| `cookbook/concurrency/monitor-down.eta` | `spawn-monitor`, `DOWN` messages. |
| `cookbook/concurrency/supervisor-tree.eta` | Restart strategies and intensity. |
| `cookbook/concurrency/gen-server-counter.eta` | OTP-style counter service. |
| `cookbook/concurrency/distributed-actors.eta` | Remote send over nng (M6.1). |

---

## 14) Tests

### 14.1 Unit tests — runtime actor system

New C++ tests under `eta/qa/test/src/actor_*_tests.cpp`:

1. `pid_identity` — PIDs compare equal only for same node/id/incarnation.
2. `mailbox_fifo_single_sender` — ordering preserved.
3. `mailbox_interleaving_multi_sender` — no lost messages under parallel
   senders.
4. `selective_receive_preserves_unmatched` — exact §6.4 semantics.
5. `receive_timeout` — returns timeout value after deadline.
6. `spawn_returns_pid` — actor runs thunk and exits normal.
7. `send_to_dead_pid` — defined behaviour (`#f` or explicit error in
   checked API).
8. `link_exit_propagation` — linked actor exits on non-normal reason.
9. `trap_exit_message` — trapping actor receives `EXIT` instead of dying.
10. `monitor_down` — monitor receives `DOWN` exactly once.
11. `demonitor_flush` — queued `DOWN` can be flushed.
12. `registry_register_whereis_unregister` — name lifecycle.
13. `process_info` — mailbox length/state/links visible.
14. `mailbox_limit_block/drop/error` — configured backpressure modes.
15. `actor_system_shutdown` — all actors terminate cleanly on session exit.

### 14.2 Eta stdlib tests

New tests under `stdlib/tests/actor*.test.eta`:

1. Basic ping/pong.
2. Nested spawn and `self` correctness.
3. Selective receive with multiple clauses.
4. Timeout `after` clause.
5. Link/unlink behaviour.
6. Monitor/demonitor behaviour.
7. Registry by symbol name.
8. `gen-server` counter call/cast.
9. Supervisor `one-for-one` restart.
10. Supervisor restart intensity shutdown.

### 14.3 Stress tests

1. Spawn 10,000 actors that each receive one message and reply.
2. One actor receives 1,000,000 messages; verify no loss, measure mailbox
   high watermark.
3. Selective receive scan over 100,000 unmatched messages; document
   expected O(n) cost.
4. Link storm: tree of 10,000 linked actors exits with bounded time.
5. Supervisor restart storm hits restart-intensity gate reliably.

### 14.4 Distributed transport tests (M6.1)

1. Start two Eta nodes on loopback nng endpoints.
2. Successful handshake includes node name and cookie check.
3. Send local -> remote and remote -> local messages.
4. Serialize PID inside message and reply to it.
5. Bad cookie rejects connection and does not add a connected node entry.

### 14.5 Distributed monitor/failure tests (M6.2)

1. Node monitor receives `nodeup` exactly once per new connection.
2. Node monitor receives `nodedown` on disconnect and on bad-cookie reconnect attempts.
3. Remote process monitor receives one `DOWN` for normal exit, error exit, and node loss.
4. `demonitor` flush option prevents stale `DOWN` delivery after local cancel.

---

## 15) Phased delivery roadmap

### M0 — Audit and design lock (1 week)

1. Confirm heap object extension points for `Pid` and `MonitorRef`.
2. Confirm Driver/session ownership location for `ActorSystem`.
3. Confirm whether Eta macro system can implement preferred `receive`
   syntax; otherwise commit to function API first.
4. Write ADR: "Actors use VM mailboxes; nng is distribution transport".
5. Add docs warning to current actor section: existing mailbox is nng
   socket-based and subject to change.

Gate: design ADR accepted; file/path decisions final.

### M1 — Local actor MVP (2–3 weeks)

1. Add `Pid`, `Mailbox`, `ActorProcess`, `ActorSystem`.
2. Treat main VM as actor with PID.
3. Implement `%actor-self`, `%actor-pid?`, `%actor-spawn`,
   `%actor-send`, `%actor-receive`, `%actor-mailbox-len`.
4. Implement `stdlib/std/actor.eta` wrappers: `self`, `pid?`, `spawn`,
   `send`, `!`, `receive-match`, `receive-after`, `mailbox-length`.
5. Add unit tests for PID, mailbox FIFO, spawn, send, receive timeout.

Gate: cookbook `actor-ping-pong.eta` passes without nng sockets.

### M2 — Selective receive and pattern API (1–2 weeks)

1. Implement mailbox scanning matcher in runtime.
2. Add Eta matcher helpers: `case`, `match-list`, `match-symbol`,
   predicate matchers.
3. If macros support it, add preferred `receive` syntax with `after`.
4. Add tests for unmatched message preservation and clause order.

Gate: exact §6.4 selective receive test passes.

### M3 — Failure semantics: links, monitors, exits (2 weeks)

1. Implement `ExitReason`, actor lifecycle transitions, cleanup.
2. Implement `link`, `unlink`, `monitor`, `demonitor`, `exit`, `kill`,
   `trap-exit!`.
3. Implement `EXIT` and `DOWN` message delivery.
4. Update existing `monitor` naming conflict with nng:
   - `std.actor/monitor` for PID monitor.
   - `std.net/nng-monitor` or raw `monitor` compatibility wrapper for
     sockets, with docs warning.
5. Add tests for links, trap exits, monitors, demonitor flush.

Gate: linked failure propagation and monitor `DOWN` tests pass.

### M4 — Supervision and registry (2 weeks)

1. Implement local registry: `register`, `unregister`, `whereis`,
   `registered`.
2. Implement `std.actor.supervisor` child specs, strategies, restart
   intensity, shutdown order.
3. Convert `std.supervisor` to compatibility shim.
4. Add supervisor cookbook and tests.

Gate: supervisor `one-for-one`, `one-for-all`, `rest-for-one`, and
restart intensity tests pass.

### M5 — OTP behaviours (2–3 weeks)

1. Implement `std.actor.gen_server`.
2. Add `gen-server-counter` cookbook.
3. Add `std.actor.gen_event` if `gen_server` stabilises quickly;
   otherwise defer to M6.2.
4. Document callback contracts and message protocols.

Gate: `gen-server-call` / `cast` / stop / terminate callback tests pass.

### M6.1 — Distribution transport bridge over nng (2–3 weeks)

1. Add `NodeTransport` using nng sockets for node-to-node actor envelopes.
2. Add node name, cookie, handshake, and connected-node table lifecycle.
3. Serialize PIDs (including node identity) in message payloads.
4. Implement remote `send` plus `send/checked` delivery-status API.
5. Add wire-envelope version byte/feature flags so incompatible nodes fail fast.
6. Update `std.net` docs to show how low-level nng differs from
   `std.actor.node`, and add first `std.actor.node` cookbook usage.

Gate: two Eta nodes on loopback complete handshake, reject bad cookie,
exchange PIDs, and exchange actor messages.

### M6.2 — Distributed monitors and node lifecycle semantics (1–2 weeks)

1. Implement node monitors: `monitor-node`, `nodeup`, `nodedown`.
2. Implement remote process monitors and remote `DOWN` forwarding.
3. Define node-loss `DOWN` reasons and enforce exactly-once monitor delivery.
4. Add `demonitor` flush semantics for remote-monitor cancellation.
5. Keep remote links deferred to M7 after failure-matrix documentation.
6. Add docs section for distributed failure semantics and netsplit behaviour.

Gate: node monitor and remote monitor tests pass for normal exit, abnormal
exit, and node disconnect/reconnect scenarios.

### M7 — Scheduler and reductions (longer-term, 4–8 weeks)

Detailed checkpoint plan: `docs/plan/actor_scheduler_plan.md` (M7.1-M7.6).

1. Add VM reduction counter and yield points.
2. Implement actor run queues and scheduler pool.
3. Move from one-thread-per-actor to lightweight actor scheduling.
4. Add dirty scheduler path for blocking native calls.
5. Stress-test 100k mostly-idle actors.

Gate: 100k idle actors can exist with bounded memory; 10k active actors
complete ping/pong stress test without OS thread exhaustion.

### M8 — Polish and compatibility cleanup (1–2 weeks)

1. Update all cookbook concurrency examples.
2. Update docs and release notes.
3. Add deprecation warnings for old socket-as-mailbox actor examples.
4. Ensure LSP builtin names and docs know all new primitives.
5. Bundle migration guide: "from `current-mailbox` / `send!` to
   `self` / `send` / `receive`."

Gate: docs, examples, and tests are all aligned with the new actor API.

---

## 16) Migration strategy

### 16.1 Keep old nng code working

Do not remove:

- `nng-socket`
- `nng-listen`
- `nng-dial`
- `send!`
- `recv!`
- `nng-poll`
- `nng-subscribe`
- `nng-set-option`

These remain transport primitives for external nng peers and examples.

### 16.2 Rename actor-facing docs

Current docs say "actor owns a mailbox socket". Replace with:

> Eta actors own VM-level mailboxes addressed by PIDs. `std.net` exposes
> nng sockets for transport-level messaging and distributed-node links.

### 16.3 Compatibility wrappers

For one release cycle:

1. `(current-mailbox)` in actor context returns `(self)` but warns in docs.
2. `(send! pid msg)` may dispatch to actor send if first arg is PID, but
   docs should prefer `(send pid msg)`.
3. `(recv! pid)` should **not** be overloaded if avoidable; receiving is
   actor-local and should use `receive`. Overloading `recv!` by PID would
   confuse mailbox ownership.

### 16.4 Cookbook migration

| Old example | New shape |
| ----------- | --------- |
| `worker-pool-worker.eta` uses `(current-mailbox)` | Worker uses `(receive ...)` and replies to sender PID included in task. |
| `message-passing.eta` uses sockets | Keep as nng transport example; add actor-native version. |
| `parallel-map.eta` spawns worker modules | Use `std.actor` worker pool with PIDs. |
| `distributed-compute.eta` | Use `std.actor.node` in M6.1. |

---

## 17) Open questions and risks

1. **Macro support for receive syntax.** If Eta cannot yet express the
   preferred `receive` macro, ship `receive-match` first and add syntax
   later. Do not block runtime semantics on syntax.
2. **Heap isolation.** Current VM values may reference mutable heap
   objects. The first implementation should serialize/deep-copy messages
   even for local actor sends, preserving isolation at the cost of speed.
3. **One-thread-per-actor MVP.** Correct but not Erlang-scale. Be honest
   in docs: M1–M6.2 are semantics parity; M7 is scalability parity.
4. **Blocking native calls.** libtorch, future libcurl, HiGHS, DB drivers,
   and file IO can block scheduler threads. Need dirty scheduler or worker
   pool before M7 is considered complete.
5. **Name collision: `monitor`.** nng already registers `monitor` for
   sockets. Actor `monitor` should live in `std.actor`; raw nng monitor can
   become `nng-monitor` or remain unqualified only in compatibility mode.
6. **Distributed failure semantics.** Erlang's distributed monitors/links
   are subtle under netsplits. Start with remote send and node monitors;
   add remote links only after the failure matrix is documented.
7. **Serialization compatibility.** PID and monitor ref wire format must be
   versioned so old/new nodes fail gracefully.
8. **Security.** Cookie auth is enough for local cluster dev, not hostile
   networks. Document this loudly until TLS lands.
9. **Debugger consistency.** Actor inspection must not mutate mailboxes;
   debug mailbox peeking should copy or snapshot.
10. **Supervisor restart storms.** Must implement intensity limits before
    promoting supervisors as production-ready.

---

## 18) Acceptance checklist

- [ ] `std.actor` exists and exports core PID/spawn/send/receive APIs.
- [ ] Main process has a PID; `(self)` works at top level.
- [ ] Actors have VM-level mailboxes independent of nng sockets.
- [ ] Selective receive preserves unmatched message order exactly.
- [ ] Receive supports timeout / `after` semantics.
- [ ] Links propagate non-normal exits.
- [ ] `trap-exit!` converts linked exits into `'(EXIT from reason)`
      messages.
- [ ] Monitors deliver exactly one `'(DOWN ref process pid reason)`.
- [ ] Registry supports `register`, `whereis`, `unregister`, `registered`.
- [ ] `process-info` exposes PID, state, mailbox length, links, monitors,
      reductions/messages counters where available.
- [ ] `std.actor.supervisor` supports `one-for-one`, `one-for-all`,
      `rest-for-one`, restart types, shutdown policies, restart intensity.
- [ ] `std.supervisor` is a compatibility shim or clearly deprecated.
- [ ] `std.actor.gen_server` supports start-link, call, cast, stop,
      terminate callback.
- [ ] Existing nng examples still run through `std.net`.
- [ ] Actor-native cookbook examples exist and pass.
- [ ] Docs no longer claim Eta's actor mailbox is an nng socket except in
      legacy/transport sections.
- [x] Distributed actor send over nng works between two loopback nodes
      (M6.1).
- [x] Bad-cookie handshake rejection and node-table consistency checks pass
      (M6.1).
- [ ] Distributed node monitors and remote process monitors deliver expected
      `nodeup` / `nodedown` / `DOWN` semantics exactly once (M6.2).
- [ ] Scheduler/reduction stress goals pass when M7 is complete.

