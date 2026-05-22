# ADR 0001: Actors Use VM Mailboxes; NNG Is Distribution Transport

Status: Accepted

## Context

Eta is moving from a socket-first concurrency model to a VM actor runtime
model with PID identity and mailbox semantics. The migration needs one stable
design lock that keeps runtime, stdlib wrappers, and docs aligned while M1-M8
ships.

## Decision

Actors use VM-owned mailboxes addressed by PID values. NNG remains the transport
layer for distribution and external messaging patterns, but not the in-memory
mailbox abstraction for local actors.

Key implementation anchors:

- `eta/core/src/eta/runtime/memory/heap.h`
- `eta/core/src/eta/runtime/factory.h`
- `eta/session/src/eta/session/driver.h`

Receive semantics for actor code are expressed through
`receive-match` / `receive-after`.

## Consequences

1. Actor APIs are PID-first (`std.actor`) and independent of socket handles.
2. Legacy socket workflows remain available in `std.net`.
3. Scheduler/reduction rollout can evolve independently of transport details.
