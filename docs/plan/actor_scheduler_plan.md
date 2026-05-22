# Actor Scheduler Plan (M7)

## 0) Purpose

M7 changes the actor execution model and has the highest regression risk in
the actor roadmap. This document splits M7 into implementation checkpoints
that are safe to land and easy to validate.

Primary outcomes:

1. Reduction-based fairness in VM execution.
2. Bounded scheduler threads (not one OS thread per actor).
3. Dirty scheduler path for blocking native calls.
4. Proven scale gate: 100k mostly-idle actors, 10k active actors.

---

## 1) Baseline and constraints

Current baseline in the repository:

1. `ActorSystem::spawn` creates one `std::thread` per actor.
2. `Driver::spawn_actor_for_vm` serializes closure state and starts child VM
   execution on that actor thread.
3. Mailbox waiting is condition-variable based and semantically correct, but
   scale is limited by OS thread count.

Hard constraints for M7:

1. No semantic regressions against M1-M6.2 actor behavior.
2. Keep a runtime escape hatch back to `thread-per-actor` until M7.6 closes.
3. Every chunk ships with explicit tests and a rollback switch.

---

## 2) Runtime switches and observability (required before cutover)

Use explicit rollout controls throughout M7:

1. `ETA_ACTOR_SCHEDULER=thread-per-actor|pool|pool-shadow`
2. `ETA_ACTOR_REDUCTION_BUDGET=<int>` (default conservative, e.g. 2000)
3. `ETA_ACTOR_DIRTY_SCHEDULERS=<int>` (default 0 until M7.5)

Add scheduler/process metrics that can be asserted in tests:

1. `process-info`: reductions used, run-state, last-yield reason.
2. Global counters: runnable queue depth, scheduler wakeups, dirty queue depth.
3. Debug stats snapshot API for deterministic tests in `eta_core_test`.

---

## 3) Chunked implementation plan

### M7.1 - Reduction accounting (no scheduling behavior change)

Scope:

1. Add reduction counters in `VM` execution loop (`vm/vm.{h,cpp}`).
2. Charge reductions per dispatched opcode/call path.
3. Expose read-only counters through actor process info.

Ownership:

1. `eta/core/src/eta/runtime/vm/vm.{h,cpp}`
2. `eta/core/src/eta/runtime/actor/actor_system.{h,cpp}`
3. `eta/core/src/eta/runtime/primitives/core_primitives_actor.cpp`

Safety rule:

1. No yielding yet. Execution order must remain byte-for-byte equivalent.

Tests:

1. Add VM unit tests for counter reset/increment monotonicity.
2. Run existing actor runtime suite unchanged.

Gate:

1. All existing actor tests pass in `thread-per-actor`.
2. Reduction counters are visible and stable in tests.

---

### M7.2 - Yield points and resumable actor execution state

Scope:

1. Introduce yield result/status from VM execution (budget exhausted,
   blocked-on-receive, finished, error).
2. Add deterministic yield points when reduction budget reaches zero.
3. Preserve mailbox/selective-receive semantics while allowing resume.

Ownership:

1. `eta/core/src/eta/runtime/vm/vm.{h,cpp}`
2. `eta/core/src/eta/runtime/actor/actor_system.{h,cpp}`
3. `eta/session/src/eta/session/driver.cpp` (spawn/run integration)

Safety rule:

1. Still default to `thread-per-actor`. Yield support is inert unless
   scheduler mode enables it.

Tests:

1. New tests forcing very small reduction budgets (1, 5, 20) with identical
   observable results.
2. Re-run selective receive order tests and monitor/link tests.

Gate:

1. Semantics parity holds under tiny and default reduction budgets.

Implementation notes (current):

1. `VM::execute_with_status(...)` now reports `finished`,
   `budget-exhausted`, or `blocked-on-receive` and keeps VM state resumable
   between slices.
2. `Driver::run_spawned_actor(...)` runs actor entry code through
   `execute_with_status(...)`; yielding remains disabled in
   `thread-per-actor` mode.
3. `process-info` now sources `last-yield-reason` from actor runtime state
   (`none`, `budget-exhausted`, `blocked-on-receive`, `finished`, `error`).
4. Coverage includes `vm_execute_with_status_yields_and_resumes_at_budget_boundary`
   and `actor_semantics_parity_under_tiny_reduction_budgets`.

---

### M7.3 - Scheduler pool and run queues (shadow mode)

Scope:

1. Add scheduler core (`actor/scheduler.{h,cpp}`) with:
   - fixed worker pool
   - per-worker run queue
   - global fallback queue and basic work stealing
2. Add actor run-state machine (`runnable`, `running`, `waiting`, `exited`).
3. Implement `pool-shadow` mode to exercise queueing/metrics without making it
   the default path for all actors.

Ownership:

1. `eta/core/src/eta/runtime/actor/scheduler.{h,cpp}` (new)
2. `eta/core/src/eta/runtime/actor/actor_system.{h,cpp}`
3. `eta/session/src/eta/session/driver.cpp`

Safety rule:

1. `thread-per-actor` remains production default.
2. Shadow mode can be disabled instantly by env var.

Tests:

1. New unit tests for enqueue/dequeue correctness and state transitions.
2. Actor runtime tests in both `thread-per-actor` and `pool-shadow`.

Gate:

1. Queue/state tests deterministic.
2. No functional difference versus baseline actor suites.

Implementation notes (current):

1. Added `eta/core/src/eta/runtime/actor/scheduler.{h,cpp}` with:
   - fixed worker pool startup/shutdown
   - per-worker run queues
   - global fallback queue
   - basic work stealing
2. `ActorSystem` now tracks run state per process (`runnable`, `running`,
   `waiting`, `exited`) and surfaces it through `process-info`.
3. `pool-shadow` now boots scheduler workers and records runnable queueing
   metrics while actor execution stays on `thread-per-actor`.
4. Coverage includes deterministic queue/work-steal tests and actor run-state
   transition + shadow-metrics assertions.

---

### M7.4 - Pool mode as first-class execution path

Scope:

1. In `pool` mode, `spawn` no longer creates one thread per actor.
2. Actor execution runs in scheduler workers using resumable VM state.
3. Mailbox push/wakeup path enqueues runnable actors exactly once.

Ownership:

1. `eta/core/src/eta/runtime/actor/actor_system.{h,cpp}`
2. `eta/core/src/eta/runtime/actor/scheduler.{h,cpp}`
3. `eta/session/src/eta/session/driver.cpp`

Safety rule:

1. Keep `thread-per-actor` available as rollback mode.
2. Guard against duplicate run-queue entries and lost wakeups.

Tests:

1. Full actor runtime tests in `pool` mode.
2. OTP behavior tests (`stdlib/tests/actor*.test.eta`) in `pool` mode.
3. Mid-scale stress (10k actors ping/pong) in CI or gated perf lane.

Gate:

1. No OS thread explosion in `pool` mode.
2. Functional suites pass in both scheduler modes.

---

### M7.5 - Dirty scheduler for blocking native calls

Scope:

1. Add dirty worker pool for blocking builtins.
2. Add builtin metadata/dispatch path for blocking classification.
3. Ensure normal scheduler workers stay responsive while dirty work runs.

Ownership:

1. `eta/core/src/eta/runtime/actor/scheduler.{h,cpp}`
2. `eta/core/src/eta/runtime/builtin_catalog.cpp`
3. `eta/core/src/eta/runtime/primitives/*` (blocking builtins only)

Safety rule:

1. Dirty scheduling is opt-in by builtin classification and env var.

Tests:

1. Add blocking primitive regression tests showing non-blocking actors keep
   making progress.
2. Verify dirty-queue backpressure and shutdown behavior.

Gate:

1. Blocking workloads do not starve regular actor scheduling.

Implementation notes (current):

1. `ActorSystem` now reads `ETA_ACTOR_DIRTY_SCHEDULERS` when pool mode starts
   and wires dirty workers into `Scheduler` startup.
2. VM primitive dispatch routes builtins classified as blocking
   (`builtin_catalog`) to dirty workers in `pool` mode, then resumes actor
   execution when the dirty task completes.
3. Added actor wake-up plumbing (`notify_external_runnable`) so dirty-task
   completion cannot lose runnable signals during pool dispatch handoff.
4. Added regression coverage for:
   - runnable scheduler dispatch progress while a dirty task is blocked
     (single scheduler worker and single dirty worker)
   - dirty queue backpressure and shutdown behavior

---

### M7.6 - Scale validation and default cutover

Scope:

1. Execute final scale gates:
   - 100k mostly-idle actors with bounded memory
   - 10k active ping/pong without thread exhaustion
2. Move default scheduler mode to `pool` after gates are green.
3. Keep `thread-per-actor` fallback for one release cycle.

Ownership:

1. `eta/qa/test/src` (stress/perf harnesses)
2. runtime actor scheduler/config docs
3. release notes and migration docs

Tests:

1. Dedicated stress tests for idle-count memory ceiling.
2. Active throughput/stability tests with monitor/link traffic mixed in.
3. Soak test (long-running) in non-PR perf lane.

Gate:

1. M7 acceptance gate in `actor_improvement_plan.md` is met.
2. Default `pool` mode is stable in CI + perf lane.

---

## 4) Test matrix per checkpoint

For every M7.x checkpoint, run:

1. `eta_core_test` actor suite (`actor_runtime_tests`).
2. stdlib actor suites (`stdlib/tests/actor.test.eta`,
   `stdlib/tests/actor_gen_server.test.eta`).
3. Dual-mode verification while fallback exists:
   - `ETA_ACTOR_SCHEDULER=thread-per-actor`
   - `ETA_ACTOR_SCHEDULER=pool` (or `pool-shadow` during M7.3)

Do not advance checkpoints if either mode regresses before M7.6 cutover.

---

## 5) Exit criteria

M7 is complete when:

1. All M7.1-M7.6 gates are green.
2. Scheduler defaults to `pool`.
3. `thread-per-actor` remains a documented fallback for one release.
4. The original M7 gate in the main roadmap is satisfied with reproducible
   stress results.
