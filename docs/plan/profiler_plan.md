# Profiler Plan: `eta_prof`

[Back to README](../../README.md) ·
[Architecture](../architecture.md) ·
[Language Guide](../language_guide.md) ·
[Build](../build.md)

> **Status.** Authoritative plan for adding a first-class profiler to the
> Eta runtime. This plan is executable without a second planning document.

---

## 0) Names and binaries

| Concern | Directory | CMake target | Binary / artefact |
| --- | --- | --- | --- |
| Profiler runtime (in-VM) | `eta/core/src/eta/runtime/prof/` | `eta_core` (existing) | — (compiled into `eta_core`) |
| CLI subcommand `eta prof` | `eta/cli/src/eta/cli/` | `eta` (existing) | `eta prof ...` |
| Standalone report viewer | `eta/tools/prof/` | `eta_prof` | **`eta_prof`** |
| Stdlib API | `stdlib/std/prof.eta` | — | `(import std.prof)` |

The build-profile flag (`--profile release|debug`) already exists on
`eta run` / `eta build`; to avoid a clash we use **`--prof`** (and the
subcommand `eta prof`) for performance profiling. Never overload
`--profile`.

---

## 1) Goals and non-goals

### Goals (v1)

1. Show where **wall-clock time is spent** at function granularity
   (Eta functions + named builtins), with self time, inclusive time,
   and call counts.
2. Produce both:
   - a **human-readable report** (flat + tree), and
   - a **machine-readable trace** (`speedscope` JSON) that opens in
     <https://speedscope.app>.
3. Work uniformly for:
   - `eta run` (script mode and package mode),
   - direct `etai` runs,
   - REPL usage via `std.prof` API:
     `(prof/with thunk)`, `(prof/start)`, `(prof/stop)`, `(prof/report)`.
4. Cover concurrent workloads that run as **in-process VM threads**
   (`spawn-thread*` cookbook demos), with per-thread profiles preserved
   in speedscope output.
5. Keep overhead within practical limits on cookbook quant workloads:
   - sampling mode: <= 5% at default 1 kHz
   - trace mode: <= 25%

### Non-goals (v1)

1. Allocation / GC profiling beyond coarse counters (deferred to phase 4).
2. Hardware counters (`perf_event_open`, ETW providers, etc.).
3. Continuous profiling daemons / remote upload.
4. Differential UI (export + external tools are enough for v1).
5. Profiling C++ internals inside `eta_core`; builtin total time is enough.
6. Profiling **out-of-process** child runtimes launched as separate
   `etai` processes (`ProcessManager::spawn`) - deferred.

---

## 2) Why both sampling and tracing

Two complementary modes, one shared aggregation backend:

| Mode | Strength | Weakness | When to use |
| --- | --- | --- | --- |
| **Sampling** | Near-constant overhead, reflects real wall time | Noisy on short runs, may miss rare paths | Default (`eta prof run`) |
| **Trace** | Exact call counts and precise per-call deltas | Higher perturbation on micro workloads | `--mode=trace` for debugging |

A single `Aggregator` consumes both event kinds so reporting is
mode-agnostic.

---

## 3) Repository layout

```text
eta/
|-- core/src/eta/runtime/prof/      # NEW
|   |-- profiler.h / .cpp           # PerThreadProfiler, sampler thread, hooks
|   |-- aggregator.h / .cpp         # Flat + call-tree tables
|   |-- frame_id.h / .cpp           # Stable interned frame ids
|   |-- sample_buffer.h / .cpp      # SPSC mailbox, one per VM thread
|   |-- clock.h                     # steady_clock wrapper
|   |-- speedscope.h / .cpp         # Speedscope JSON writer
|   |-- pprof.h / .cpp              # (phase 3) optional pprof writer
|   `-- report.h / .cpp             # pretty + json reporters
|-- cli/src/eta/cli/
|   `-- prof_subcommand.cpp         # NEW - eta prof run|report|merge|view
`-- tools/prof/                     # NEW - standalone offline viewer
    |-- CMakeLists.txt
    `-- src/eta/prof/main_eta_prof.cpp

stdlib/std/prof.eta                 # NEW - (import std.prof)
docs/guide/profiling.md             # NEW
docs/plan/profiler_plan.md          # this file
```

Profiler runtime code lives inside `eta_core` (no separate runtime lib):
hooks are on hot VM paths and should stay direct.

---

## 4) Design

### 4.1 What is a "frame"?

A frame is a single row in reports. A `FrameId` is interned from:

```text
(kind, qualified_name, source_span)
```

where:

- `kind ∈ {EtaFunction, Builtin, AnonymousLambda, TopLevel, ContinuationResume, UserRegion}`
- `qualified_name` includes module path, e.g.
  `cookbook/quant/european:price`
- anonymous lambdas use source labels, e.g.
  `<lambda@european.eta:42:7>`

Frame ids are dense `uint32_t` from
`InternTable<FrameKey, uint32_t>` behind `std::shared_mutex`
(cold path only).

### 4.1.1 Stable naming strategy (must land first)

This is a prerequisite for useful profiler output and deterministic
fixtures.

1. Keep existing top-level/module names stable (`<module>_init`).
2. For named user functions, emit a canonical report name:
   `<module>:<binding-name>`.
3. For lambdas/unnamed closures, use source-stable naming rather than
   emission counters:
   `<lambda@file:line:column>`.
4. Ensure this naming is deterministic across runs and independent of
   unrelated compile ordering.

This work belongs in **phase 0**, before metrics validation.

### 4.2 Hook points in the VM

Hooks are wired at central VM call/return helpers so all call forms are
covered:

1. Call setup paths for `Call`, `TailCall`, `Apply`, and `TailApply`
   after `dispatch_callee(...)` decides `SetupFrame`, `TailReuse`, or
   `Continue`.
2. Return paths in both:
   - `OpCode::Return`, and
   - the explicit tail fast path that calls `handle_return(pop())`.
3. Primitive dispatch in `dispatch_callee` (RAII around
   `prim->func(args)`), which is the single builtin timing hook.
4. Continuation/dynamic-wind transfer in `dispatch_callee` +
   `handle_return` via `on_continuation_jump(target_depth)` to
   truncate/restore profiler shadow state without fake leaves.

When profiling is off, hooks should be a single predictable fast-path
branch:

```cpp
if (!g_prof.enabled.load(std::memory_order_relaxed)) return;
```

### 4.3 Per-thread state and aggregator

```cpp
struct ShadowFrame {
    uint32_t frame_id;
    uint64_t enter_ns;
    uint64_t child_ns_at_enter;
};

struct PerThreadProfiler {
    std::vector<ShadowFrame> stack;
    uint64_t                 child_ns_total = 0;
    SampleBuffer             samples;   // SPSC mailbox for snapshots
    uint64_t                 thread_id;
};
```

`PerThreadProfiler*` is registered on thread start and removed on exit.

`Aggregator` owns:

1. `flat`: `frame_id -> {self_ns, incl_ns, calls}`
2. `tree`: `(parent_frame_id, child_frame_id) -> {incl_ns, calls}`
3. `stack_intern`: `vector<frame_id> -> stack_id` for speedscope samples
4. `counters`: named `uint64_t` counters from `prof/counter`

### 4.4 Sampler thread (race-free design)

A single sampler OS thread:

1. Sleeps on `condition_variable` with timeout `1s / sample_hz`
   (default 1000 Hz, configurable, capped 10 kHz).
2. On wake, increments global `sample_epoch`.
3. Each VM thread checks `sample_epoch` at safepoints
   (run-loop boundary + call/return boundaries); on change it snapshots
   **its own** current shadow stack into its own SPSC `SampleBuffer`.
4. Sampler drains all per-thread buffers into `Aggregator` periodically,
   and always at `stop()` / `report()`.

No thread reads another thread's `std::vector` internals concurrently.
This avoids undefined behavior under the C++ memory model.

Tail calls are represented correctly because the logical top frame is
updated before the next safepoint snapshot.

### 4.5 Clock

Use `steady_clock::now()` in v1 on all platforms.
`clock.h` reserves a future optional TSC path.

### 4.6 Concurrency scope (v1)

Supported in v1:

- in-process worker VMs (`spawn-thread*`, cookbook concurrency demos)

Not supported in v1:

- out-of-process child runtimes (`spawn` launching separate `etai`)

### 4.7 Overhead budget

| Mode | Hook cost | Sampler cost | Memory |
| --- | --- | --- | --- |
| off | single fast branch | 0 | 0 |
| sampling | branch + shadow-stack maintenance | timer tick + buffer drain | proportional to samples |
| trace | clock + stack bookkeeping per event | 0 | proportional to call count |

Hard gates are measured in CI benches (section 8), not estimated values.

---

## 5) CLI surface

### 5.1 New subcommand: `eta prof`

```text
eta prof run    [--mode sample|trace] [--hz N]
                [--out FILE] [--format pretty|json|speedscope|eta-prof|chrome|pprof]
                [--bin NAME | --example NAME | FILE.eta] [-- args...]

eta prof report [--format pretty|json|speedscope|chrome|pprof] FILE.eta-prof
eta prof merge  --out OUT.eta-prof IN1.eta-prof IN2.eta-prof ...
eta prof view   FILE.speedscope.json|FILE.eta-prof
```

### 5.2 Existing-command flags

`eta run` accepts:

- `--prof[=sample|trace]`
- `--prof-out=FILE`
- `--prof-format=FMT`
- `--prof-hz=N`

These delegate to the same profiler machinery.
There is currently no `eta exec` command in this repo.

### 5.3 In-language API (`std.prof`)

`stdlib/std/prof.eta` exports:

```scheme
(prof/start    [mode] [hz])                          ; -> session
(prof/stop     session)                              ; -> report-handle | #f
(prof/with     thunk [mode] [hz])                    ; -> (values result report-handle)
(prof/report   handle [format])                      ; -> string
(prof/counter  name n)                               ; increment named counter
(prof/region   name thunk)                           ; push synthetic UserRegion
(prof/enabled?)                                      ; -> bool
```

---

## 6) Output formats

### 6.1 Pretty (default stdout)

Two sections:

1. flat top-N by self time
2. call tree (optionally depth-truncated)

plus a counters section when non-empty.

### 6.2 Speedscope JSON (primary structured format)

Emit valid speedscope schema with one profile per thread.
Fixtures and schema checks are CI-gated.

### 6.3 pprof (phase 3, optional)

Optional writer behind `ETA_PROF_PPROF=ON`.
Current implementation wires the export path and returns an explicit
runtime error until a full pprof serializer lands.

### 6.4 Chrome trace JSON (phase 3)

Transform from shared in-memory model.

### 6.5 `eta-prof` archive format

Internal merge/report archive:

- JSON payload with mode, frame intern table, trace aggregates or sampled
  profiles, and named counters.

`speedscope` is export format; `eta-prof` is archive format.

---

## 7) Phased roadmap

### Phase 0 - naming, hooks, scaffolding

1. Add `eta/core/src/eta/runtime/prof/` skeleton.
2. Land stable naming strategy (section 4.1.1).
3. Wire hooks in VM call/return helpers:
   `Call`, `TailCall`, `Apply`, `TailApply`, `Return`,
   `dispatch_callee`, `handle_return`.
4. Add off-overhead microbench under `eta/qa/bench/prof/`.

Gate:

- no functional changes with profiler disabled
- off-overhead <= 1% on benchmark workloads
- frame names deterministic across runs

### Phase 1 - trace MVP

1. Trace aggregator (flat + tree), per-thread shadow stack, global merge.
2. `eta prof run --mode=trace` + pretty report.
3. `std.prof` MVP:
   `prof/start`, `prof/stop`, `prof/with`, `prof/region`, `prof/report`.
4. Integration test:
   recursion workload shows expected dominant frame and call count sanity.

Gate:

- trace output is sane on `cookbook/quant/european.eta`
- trace overhead <= 50% for baseline workload

### Phase 2 - sampling + speedscope

Status update (2026-05-08):
- Sampler thread + epoch handshake + per-thread sample buffers are implemented.
- `eta prof run` now defaults to sampling mode.
- Speedscope JSON emission is wired for sampled sessions.
- `eta run --prof[=sample|trace]` and `--prof-out` route through the same runtime.
- Tests cover sampled speedscope schema/content and multi-thread sampling profiles.

1. Sampler thread + `sample_epoch` handshake + per-thread SPSC buffers.
2. Default `eta prof run` mode is sampling (1 kHz).
3. Speedscope writer + schema validation fixtures.
4. `eta run --prof[=sample]` + `--prof-out`.
5. Concurrency test on in-process `parallel-fib`:
   each worker thread appears in speedscope output.

Gate:

- speedscope renders cookbook quant workload
- sampling overhead <= 5% at 1 kHz

### Phase 3 - report/merge and extra formats

Status update (2026-05-08):
- `eta prof report`, `eta prof merge`, and `eta prof view` are implemented
  through the standalone `eta_prof` tool and surfaced through `eta prof`.
- `eta-prof` archive read/write/merge is implemented in runtime `prof/`.
- Chrome trace export and counters in pretty/json reports are implemented.
- `std.prof` exports `prof/counter` and forwards report format selection.
- Optional pprof integration is wired; writer remains a stub with a clear
  diagnostic.

1. `eta prof report`, `eta prof merge`, `eta prof view`.
2. `eta-prof` archive format + `eta/tools/prof/` standalone viewer.
3. Optional pprof writer (`ETA_PROF_PPROF=ON`).
4. Chrome trace writer.
5. Counters in pretty/json reports.

Gate:

- merge combines N runs correctly
- pprof path returns deterministic diagnostics until serializer lands

### Phase 4 - allocations + notebook integration

Status update (2026-05-08):
- Coarse allocation profiling is implemented through allocator hooks and
  reported as `bytes_allocated` in flat reports (pretty/json) and archives.
- `eta_jupyter` implements `%%prof` cell magic with inline flamegraph HTML and
  text report output.
- `eta_repl` implements one-shot `:prof` meta-command for profiling the next
  submission.
- Gate satisfied: notebook profiling now runs and visualizes in-process.

1. Coarse allocation profiling (`bytes_allocated` per frame) from
   allocator hooks.
2. Notebook `%%prof` magic in `eta_jupyter` with inline flamegraph view.
3. REPL `:prof` meta-command in `etai`.

Gate:

- notebook cell can profile + visualize without shell

### Phase 5 - hardening

1. Long-run stability test (30 min sampling run, bounded memory growth).
2. Public API freeze for `std.prof`.
3. Docs finalization.

Gate:

- 1.0 API/format freeze for `std.prof` + `eta-prof`

---

## 8) Testing strategy

1. **Unit tests** under `eta/qa/test/src/prof/`:
   - frame-id interning correctness + thread safety
   - aggregator arithmetic on synthetic streams
   - sample buffer SPSC stress (producer/consumer, no torn reads)
   - speedscope schema conformance on pinned fixtures
2. **Integration tests** under `eta/qa/test/src/cookbook/prof/`:
   - recursion dominance + call-count checks
   - in-process concurrency profile count checks
   - tail recursion bounded depth behavior
3. **Benchmarks** under `eta/qa/bench/prof/`:
   - profiler-off overhead gate
   - sampling overhead gate (1 kHz)
   - trace overhead gate
4. **Fuzz** under `eta/qa/fuzz/`:
   random programs through `eta prof run` should not crash or leak
   unbounded memory.
5. **Property check**:
   flat `self_ns` sum tracks observed sampled wall time within tolerance
   (sampling period + scheduler jitter).

---

## 9) Documentation deliverables

1. `docs/guide/profiling.md`:
   sampling vs trace, CLI/API reference, report reading guide, pitfalls.
2. `docs/stdlib.md`: add `std.prof`.
3. `docs/architecture.md`: add profiling subsection.
4. `docs/release-notes.md`: phase entries.
5. `eta/tools/prof/README.md`: extending output formats.
6. `docs/next-steps.md`: link this plan once phase 0 lands.

---

## 10) Risks and decisions

1. **Hot-path overhead when off**
   - Mitigation: single fast branch + phase-0 benchmark gate.
2. **Tail-call and self-time accounting**
   - Mitigation: explicit tail-reuse handling in call helper hooks.
3. **Continuations / `call/cc`**
   - Mitigation: dedicated continuation-jump profiler hook in
     `dispatch_callee`/`handle_return`.
4. **Sampling correctness under concurrency**
   - Decision: cooperative per-thread snapshots (`sample_epoch`) rather
     than foreign-thread stack reads.
5. **Builtin granularity**
   - Decision: attribute full builtin wall time to builtin frame.
6. **Anonymous lambda identity**
   - Decision: source-span based names; users can refine via
     `prof/region`.
7. **CLI `--profile` clash**
   - Decision: keep `--profile` for build mode, use `--prof` for profiler.
8. **Scope decision**
   - v1 supports in-process threads only; out-of-process deferred.
9. **Stable naming decision**
   - Stable frame naming is phase-0 prerequisite, not post-processing.

---

## 11) Milestones / acceptance criteria

Profiler v1 is complete when all are true:

- [x] Build produces `eta` with `prof` subcommand and standalone
      `eta_prof`; `(import std.prof)` resolves.
- [ ] `eta prof run --mode=sample cookbook/quant/european.eta`
      produces speedscope JSON that opens and highlights dominant quant
      frames.
- [ ] `eta prof run --mode=trace cookbook/basics/recursion.eta`
      reports exact expected recursion call counts.
- [ ] In-process concurrency profiling shows >= 4 thread profiles on
      4-worker `parallel-fib`.
- [ ] Overhead gates pass: <= 1% off, <= 5% sampling at 1 kHz,
      <= 50% trace.
- [x] `eta prof merge` merges multiple runs correctly.
- [ ] Pretty, json, speedscope (and pprof when enabled) pass fixture checks.
- [x] `std.prof` API (`prof/with`, `prof/region`, `prof/counter`) is
      covered in unit + integration tests.
- [ ] Profiling docs are published and linked from docs index / next steps.
