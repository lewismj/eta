# ADR 0001: Actors Use VM Mailboxes; NNG Is Distribution Transport

[Back to Architecture](../architecture.md)

## Status

Accepted on 2026-05-21.

## Context

Eta's current concurrency primitives (`spawn`, `spawn-thread`,
`current-mailbox`, `send!`, `recv!`, and socket `monitor`) use NNG PAIR
sockets as actor mailbox handles. This is useful for process and thread
messaging today, but it does not provide PID identity, VM mailbox
introspection, or selective receive semantics.

M0 requires a design lock before runtime implementation begins.

## Decision

1. Actors are addressed by PID values and consume messages from VM-owned
   mailboxes.
2. NNG remains in Eta as transport for distribution and external message
   bus patterns; NNG sockets are not the long-term local mailbox
   representation.
3. Heap object extension points for `Pid` and `MonitorRef` are:
   - `eta/core/src/eta/runtime/memory/heap.h` (`ObjectKind` enum and
     typed heap dispatch).
   - `eta/core/src/eta/runtime/factory.h` (`make_heap_object` template and
     typed constructors).
   - `eta/core/src/eta/runtime/types/types.h` (aggregated runtime type
     includes for boxed object structs).
4. Session ownership for one actor runtime per interpreter session remains
   anchored in `eta/session/src/eta/session/driver.h` and
   `eta/session/src/eta/session/driver.cpp` via the `Driver`-owned runtime
   field. The VM-level `ActorSystem` introduced in M1 will follow this
   ownership model (one per `Driver` session).
5. Receive API rollout is function-first. Eta's macro system is
   `syntax-rules`-only (documented in `docs/guide/macros.md`), so M1
   ships function helpers first (`receive-match` / `receive-after`), with
   macro sugar evaluated in M2.

## Consequences

1. Existing socket-based concurrency APIs stay available as compatibility
   surfaces and `std.net` transport primitives.
2. Documentation must clearly label the current socket mailbox model as
   transitional.
3. M1 implementation work can proceed without blocking on macro syntax
   design.
