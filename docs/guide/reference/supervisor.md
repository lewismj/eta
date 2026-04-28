# Supervision Trees — `std.supervisor`

[<- Back to README](../../../README.md) · [Message Passing](message-passing.md) ·
[Networking](networking.md)

---

## Overview

`std.supervisor` provides Erlang-inspired supervision strategies for
Eta actor processes.  It composes directly with the existing actor
primitives (`spawn`, `monitor`, `recv!`, `send!`, `nng-poll`,
`spawn-kill`) and adds no new runtime machinery — just a structured
event loop.

Two strategies ship out of the box:

| Strategy | Restart policy |
|----------|----------------|
| `one-for-one` | Only the crashed child is restarted |
| `one-for-all` | Any crash kills **all** siblings and restarts the whole group |

Both functions run a polling loop and **do not return normally**.

```scheme
(import std.supervisor)
```

> [!NOTE]
> The supervisor itself must be a **spawned child** (so it has a
> mailbox).  Calling it on the main process would block the VM on
> `recv!`.  Alternatively the included poll loop uses a 500 ms timeout
> so the supervisor can interleave with other periodic work.

---

## API

### `(one-for-one children-specs)`

```scheme
(one-for-one '("worker-a.eta" "worker-b.eta" "worker-c.eta"))
```

- Spawns one child per module path in `children-specs`.
- Calls `(monitor sock)` on each spawned socket so the supervisor
  receives a `(down ...)` message when a child dies.
- On `(down ...)`, locates the dead child's slot, respawns **only**
  that module, and re-monitors the new socket.
- All other children continue uninterrupted.

### `(one-for-all children-specs)`

```scheme
(one-for-all '("ingester.eta" "worker.eta" "publisher.eta"))
```

- Spawns + monitors every child as in `one-for-one`.
- On **any** `(down ...)`:
  1. Calls `spawn-kill` on every still-live child.
  2. Restarts the entire group from scratch.
- Suited for tightly-coupled groups where partial state would be
  meaningless after one peer dies.

---

## Implementation Sketch

```text
spawn each child           ─► monitor each child
        │                            │
        ▼                            ▼
   poll loop  ◄──── nng-poll(socks, 500ms) ────┐
        │                                       │
        ▼                                       │
   (down ...)?  ──no──► continue ───────────────┘
        │
        yes
        ▼
   one-for-one : respawn dead slot, re-monitor
   one-for-all : kill-all, start-all
```

The supervisor stores child sockets in a vector for stable index-based
restart.  `nng-poll` with a short timeout keeps the loop responsive
without busy-waiting.

---

## Composition Patterns

- **Hierarchical supervision.**  A supervisor is just a process —
  spawn supervisors *inside* supervisors to build trees.
- **Mixed strategies.**  Place latency-sensitive workers under a
  `one-for-one` parent and stateful pipelines under a `one-for-all`
  parent; both can sit under a top-level `one-for-one` root.
- **Health checks.**  Children can `send!` heartbeat messages; extend
  the supervisor's `recv!` arm to treat missing heartbeats as a soft
  crash.

---

## Source Locations

| Component | File |
|-----------|------|
| `std.supervisor` module | [`stdlib/std/supervisor.eta`](../../../stdlib/std/supervisor.eta) |
| Actor primitives | [`stdlib/std/net.eta`](../../../stdlib/std/net.eta), [`docs/networking.md`](networking.md) |
| Message-passing model | [`docs/message-passing.md`](message-passing.md) |

