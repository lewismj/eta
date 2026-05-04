# Profiler Plan: `eta_prof`

[Back to README](../../README.md) ·
[Architecture](../architecture.md) ·
[Language Guide](../language_guide.md) ·
[Build](../build.md)

> **Status.** Authoritative plan for adding a first-class profiler to the
> Eta runtime. An implementer should be able to execute it without
> consulting any other planning document. Companion to the lint/format
> plan; same naming and phasing conventions.

---

## 0) Names and binaries

| Concern                       | Directory                        | CMake target | Binary / artefact         |
| ----------------------------- | -------------------------------- | ------------ | ------------------------- |
| Profiler runtime (in-VM)      | `eta/core/src/eta/runtime/prof/` | `eta_prof_lib` | — (links into `eta_core`) |
| CLI subcommand `eta prof`     | `eta/cli/src/eta/cli/`           | `eta` (existing) | `eta prof …`              |
| Standalone report viewer      | `eta/tools/prof/`                | `eta_prof`   | **`eta_prof`**            |
| Stdlib API                    | `stdlib/std/prof.eta`            | —            | `(import std/prof)`       |

The build-profile flag (`--profile release|debug`) already exists on
`eta run` / `eta build`; to avoid a clash we use **`--prof`** (and the
subcommand `eta prof`) for performance profiling. Never overload
`--profile`.

---

## 1) Goals and non-goals

### Goals (v1)

1. Tell the user **where wall-clock time is spent** in an Eta program at
   function granularity (user-defined Eta functions and named builtins),
   broken down into self time, inclusive time, and call counts.
2. Produce both a **human-readable report** (flat + tree) on stdout and a
   **machine-readable trace** (`speedscope` JSON) that opens directly in
   <https://speedscope.app> for flamegraph / sandwich views.
3. Work uniformly for `eta run`, `eta exec` (one-shot scripts), and the
   REPL via an in-language API — `(profile/with thunk)` /
   `(profile/start)` / `(profile/stop)` / `(profile/report)`.
4. Cover concurrent workloads (the `nng`-based cookbook concurrency
   demos): per-OS-thread sample buffers, merged at report time, with a
   thread axis preserved in the speedscope output.
5. **Acceptable overhead.** Sampling mode ≤ 5 % wall-clock overhead at
   the default 1 kHz on cookbook quant workloads. Instrumenting mode ≤
   25 % on the same.

### Non-goals (v1)

1. Allocation / GC profiling beyond a coarse counter (deferred to phase
   4).
2. Hardware performance counters (`perf_event_open`, ETW providers).
   Hooks are reserved but no implementation.
3. Continuous profiling daemons / remote upload.
4. Differential / A-B comparison UI (export is enough; speedscope already
   does this).
5. Profiling of **C++** code paths inside `eta_core` itself — that is
   what platform tools (perf, VTune, Instruments, ETW, `samply`) are
   for. We surface a builtin's name and inclusive time, not its
   internals.

---

## 2) Why both sampling and instrumenting

Two complementary modes, one shared aggregation backend:

| Mode             | Strength                                              | Weakness                                  | When to use                          |
| ---------------- | ----------------------------------------------------- | ----------------------------------------- | ------------------------------------ |
| **Sampling**     | Constant overhead, sees real wall time, no skew on hot tiny functions | Noisy on short runs; misses rare expensive calls | Default for `eta prof run` |
| **Instrumenting**| Exact call counts and self-time deltas                | Skews micro-benchmarks, distorts inlining shapes | `--mode=trace`; debugging counts     |

A single `Aggregator` (§4.3) consumes events from either source so the
reporting layer is mode-agnostic.

We deliberately do *not* ship a third "tracing-with-args" mode in v1.
Users who want span-style traces already get them from `spdlog`.

---

## 3) Repository layout

```
eta/
├── core/src/eta/runtime/prof/      # NEW
│   ├── profiler.h / .cpp           # PerThreadProfiler, sampler thread, hooks
│   ├── aggregator.h / .cpp         # Flat + call-tree tables
│   ├── frame_id.h / .cpp           # Stable interned (func_name, span) ids
│   ├── sample_buffer.h / .cpp      # Lock-free MPSC ring, one per VM thread
│   ├── clock.h                     # steady_clock wrapper, calibrated TSC fast path
│   ├── speedscope.h / .cpp         # Speedscope JSON writer
│   ├── pprof.h / .cpp              # (phase 3) gperftools pprof writer
│   └── report.h / .cpp             # Pretty + JSON text reporters
├── cli/src/eta/cli/
│   └── prof_subcommand.cpp         # NEW — `eta prof run|report|merge`
└── tools/prof/                     # NEW — standalone offline viewer
    ├── CMakeLists.txt
    └── src/eta/prof/main_eta_prof.cpp
stdlib/std/prof.eta                 # NEW — (import std/prof)
docs/guide/profiling.md             # NEW — user guide
docs/plan/profiler_plan.md          # this file
```

The new code lives **inside** `eta_core` (not a separate library) for two
reasons: (a) the hooks must be on the VM hot path with no virtual
dispatch, (b) profiling is a runtime feature, not a tool. The
standalone `eta_prof` binary only ships the offline reader/renderer and
links a thin subset.

---

## 4) Design

### 4.1 What is a "frame"?

A frame is whatever the user expects to see as a row in the report.
Concretely a `FrameId` is the interned tuple

```
(kind, qualified_name, source_span)
```

where `kind ∈ {EtaFunction, Builtin, AnonymousLambda, TopLevel,
ContinuationResume}`. `qualified_name` includes the module path
(`std/math/sin`, `cookbook/quant/european:price`). Anonymous lambdas
are labelled with their defining span (`<lambda@european.eta:42:7>`).

Frame ids are dense `uint32_t` allocated by an
`InternTable<FrameKey, uint32_t>` shared across threads, behind an
`std::shared_mutex` (cold path: only on first sight of a function).

### 4.2 Hook points in the VM

The bytecode VM (`eta/core/src/eta/runtime/vm/vm.cpp`) already centralises
calls in three opcodes — this is exactly where the hooks belong:

1. `OpCode::Call`            — `prof::on_enter(frame_id)`
2. `OpCode::TailCall`        — `prof::on_tail_call(callee_frame_id)`
   (pops current frame's sample, pushes callee's; preserves wall time
   accounting)
3. `OpCode::Return`          — `prof::on_leave()`
4. Builtin dispatch (the C++ trampoline that resolves a `Builtin*` and
   calls it) — wrapped in a RAII `BuiltinScope` that acts as
   enter/leave for builtin frames. This is the **only** place builtins
   need to be touched.
5. Continuation reinstatement (`OpCode::CallCC`, `DynamicWind` family)
   — explicit `on_continuation_jump(target_depth)` that truncates the
   shadow stack to `target_depth` rather than emitting fake leaves.

The hooks are **branchless when profiling is off**: a single
`if (!g_prof.enabled.load(std::memory_order_relaxed)) return;` at the
top, and `g_prof.enabled` is an `std::atomic<bool>` initialised to
`false`. With branch prediction this is < 1 ns per call when off.

### 4.3 Per-thread shadow stack and aggregator

```cpp
struct ShadowFrame {
    uint32_t frame_id;
    uint64_t enter_ns;        // steady_clock::now()
    uint64_t child_ns_at_enter; // for self-time accounting
};

struct PerThreadProfiler {
    std::vector<ShadowFrame> stack;   // grows with calls
    uint64_t                 child_ns_total = 0;
    SampleBuffer             samples;  // for sampling mode (lock-free)
    uint64_t                 thread_id;
};
```

The thread-local pointer is published on VM-thread spawn so the sampler
thread (§4.4) can walk every live VM thread's shadow stack at sample
time.

`Aggregator` owns:

1. `flat`: `frame_id → {self_ns, incl_ns, calls}` — updated by enter/leave
   in instrumenting mode, by sample walks in sampling mode.
2. `tree`: edges `(parent_frame_id, child_frame_id) → {incl_ns, calls}`,
   reconstructed from the shadow-stack snapshots stored in the sample
   buffer (each sample stores a stack-id from a stack interner).
3. `stack_intern`: dedupes `vector<frame_id>` → `stack_id`, the
   speedscope "profile.samples[]" element type.

### 4.4 Sampler thread

A single OS thread:

1. Sleeps on a `condition_variable` with timeout = `1s / sample_hz`
   (default 1000 Hz, configurable, capped 10 kHz).
2. On wake, iterates the registry of live VM threads and copies each
   shadow stack into a thread-local SPSC ring (`SampleBuffer::push`).
   Reads are racy by construction; we accept this and rely on the
   shadow stack being a contiguous `std::vector` whose `size()` we read
   with `memory_order_acquire` *after* a `memory_order_release` store
   in `on_enter`/`on_leave`. Stale stacks at sample time are
   indistinguishable from sampling between two instructions — the
   intended semantics.
3. Drains every ring into the `Aggregator` at flush points (`stop()`,
   `report()`).

**Tail calls** appear correctly because `on_tail_call` updates the top
of stack atomically.

**Recursion** is handled because samples carry the full stack, so the
flat view collapses recursive frames (counts add) while the tree view
preserves depth.

### 4.5 Clock

`steady_clock::now()` is fast enough on Linux (vDSO) and Windows
(`QueryPerformanceCounter`) — < 30 ns. We do not ship a TSC fast path
in v1; `clock.h` reserves the seam.

### 4.6 Concurrent / nng workloads

`std/concurrency` and the cookbook spawn worker VM threads. Each worker
has its own `PerThreadProfiler` registered/unregistered on thread
start/exit. Samples carry `thread_id`; the speedscope export emits one
`profile` per thread, which speedscope renders as separate flame rows.

The in-language API can scope profiling to a single thread:
`(profile/with-thread thunk)` profiles only the calling thread.

### 4.7 Overhead budget

| Mode      | Hook cost           | Sampler cost              | Memory                              |
| --------- | ------------------- | ------------------------- | ----------------------------------- |
| off       | ~1 ns/call (branch) | 0                         | 0                                   |
| sampling  | ~1 ns/call          | ~50 µs per sample × hz    | 16 B/sample × hz × runtime_seconds |
| trace     | ~30 ns/call (clock + 2 atomic ops + push) | 0 | 32 B/event × call count            |

At 1 kHz on a 60 s run we budget ≤ 1 MB of sample memory per VM
thread; samples flush to disk every 1 MB if `--prof-out` is set, else
buffered to end.

---

## 5) CLI surface

### 5.1 New subcommand: `eta prof`

```
eta prof run    [--mode sample|trace] [--hz N] [--out FILE.speedscope.json]
                [--format speedscope|pprof|chrome|pretty|json]
                [--threads all|main]  <script.eta> [-- args...]
eta prof report [--format pretty|json|speedscope] FILE.eta-prof
eta prof merge  --out OUT.eta-prof  IN1.eta-prof IN2.eta-prof ...
eta prof view   FILE.speedscope.json    # opens speedscope.app in browser
```

Flags:

| Flag                | Default                  | Notes                                     |
| ------------------- | ------------------------ | ----------------------------------------- |
| `--mode`            | `sample`                 | `sample` or `trace`                       |
| `--hz`              | `1000`                   | sampling frequency, range `[10, 10000]`   |
| `--out`             | `./prof.speedscope.json` | output path                               |
| `--format`          | inferred from `--out`    | `pretty` is text-only; for `--out` use a structured format |
| `--threads`         | `all`                    | `all` or `main`                           |
| `--include`         | (none)                   | regex of frame names to keep              |
| `--exclude`         | `^std/` (off by default) | regex of frame names to drop              |
| `--max-depth`       | unlimited                | truncate tree report                      |
| `--top`             | `30`                     | flat report row count                     |
| `--no-color`        | tty-aware                |                                           |

Exit codes follow the existing `eta` conventions (0 success, 1 runtime
error in profilee, 64 bad CLI usage, 70 internal error).

### 5.2 Existing-command flags (opt-in)

For convenience, `eta run` and `eta exec` accept `--prof[=MODE]` and
`--prof-out=FILE`, which delegate to the same machinery. This keeps
`eta prof run script.eta` and `eta run --prof script.eta` equivalent.

### 5.3 In-language API (`std/prof`)

`stdlib/std/prof.eta` exports:

```scheme
(prof/start    [#:mode 'sample|'trace] [#:hz 1000])  ; -> session
(prof/stop     session)                              ; -> report-handle
(prof/with     thunk [#:mode 'sample] [#:hz 1000])   ; -> (values result report)
(prof/report   handle [#:format 'pretty|'json|'tree]) ; -> string | hashmap
(prof/save     handle path [#:format 'speedscope|'pprof|'eta-prof]) ; -> #t
(prof/region   name thunk)   ; user-named span pushed onto shadow stack
(prof/counter  name n)       ; increments a named counter, surfaced in report
(prof/enabled?)              ; -> bool
```

`prof/region` is the way users get **arbitrary spans** (e.g.
`(prof/region "monte-carlo:inner-loop" (lambda () ...))`). Internally it
pushes a synthetic `FrameId` of kind `UserRegion`.

`prof/counter` covers the "I want to know how many times X happened"
case without abusing function calls.

---

## 6) Output formats

### 6.1 Pretty (default for stdout)

Two sections: flat top-N and inverted call tree. Mirrors `pprof`'s
`top` and `tree` commands so the output is familiar.

```
eta prof: sample mode, 1000 Hz, 12.43 s wall, 12 412 samples, 4 threads

Flat (top 10 by self time):
  self%   self        incl%   incl       calls    name
  31.4%   3.90 s      48.2%   5.99 s     1 240 003  cookbook/quant/european:simulate-path
  17.1%   2.12 s      17.1%   2.12 s    36 200 110  std/math:exp
   9.8%   1.22 s      62.0%   7.71 s         1 000  cookbook/quant/european:price
   ...

Tree (truncated at depth 6, edges with ≥1% incl):
  100.0%  12.43 s  <root>
  ├─ 62.0%   7.71 s  cookbook/quant/european:price
  │  ├─ 48.2%   5.99 s  cookbook/quant/european:simulate-path
  │  │  ├─ 17.1%   2.12 s  std/math:exp
  │  │  └─ 11.4%   1.42 s  std/math:sqrt
  ...

Counters:
  rng-draws        36 200 110
```

### 6.2 Speedscope JSON (primary structured format)

Implements the `speedscope` schema (event-format profile, one per
thread, frames table, samples + weights). Verified by loading in
<https://speedscope.app>; sample fixtures live under
`eta/core/tests/prof/fixtures/`.

### 6.3 pprof (phase 3)

Google `pprof` proto v3 — enables `pprof -http=:8080 prof.pb.gz` and
the existing ecosystem (`pprof -flame`, etc.). Optional; gated behind a
CMake option `ETA_PROF_PPROF=ON`. We pull `protobuf` only if enabled.

### 6.4 `chrome://tracing` JSON (phase 3)

Trivial transform from speedscope; useful inside Chromium-based
inspectors and Perfetto.

### 6.5 `eta-prof` binary format

Compact internal format used by `eta prof merge` and `eta prof report`:
a header + intern tables + sample stream (zstd-framed). Speedscope is
the *export*; `eta-prof` is the *archive*. This separation lets us
evolve the on-disk schema without breaking external viewers.

---

## 7) Phased delivery roadmap

### Phase 0 — Hooks and scaffolding

1. `eta/core/src/eta/runtime/prof/` skeleton (`profiler.h/.cpp`,
   `frame_id.h/.cpp`, `clock.h`, empty `Aggregator`).
2. Wire enter/leave/tail/builtin/cont-jump hooks in `vm.cpp` and the
   builtin trampoline. Branchless-off when disabled.
3. Microbenchmark in `eta/qa/bench/` proving overhead-when-off ≤ 1 % on
   `cookbook/basics/recursion.eta` and `cookbook/quant/european.eta`.

Gate: bench passes on Linux + Windows; no functional change with
profiler off.

### Phase 1 — Trace mode MVP

1. Instrumenting aggregator (flat + tree), per-thread shadow stack,
   global merge.
2. `eta prof run --mode=trace` CLI; pretty reporter only.
3. Stdlib `std/prof`: `prof/start`, `prof/stop`, `prof/with`,
   `prof/region`, `prof/report`.
4. Integration test: profile `cookbook/basics/recursion.eta` and assert
   `fact` (or `fib`) appears in the top-3 by inclusive time.

Gate: `eta prof run --mode=trace cookbook/quant/european.eta` produces
a sane pretty report; trace overhead ≤ 50 %.

### Phase 2 — Sampling mode + speedscope export

1. Sampler thread, lock-free SPSC sample rings, stack interner.
2. `eta prof run` defaults to sampling at 1 kHz.
3. Speedscope JSON writer; round-trip test (load via a tiny vendored
   schema check in `qa/`).
4. `--prof[=sample]` flag on `eta run` / `eta exec`.
5. Concurrency coverage: profile `cookbook/concurrency/parallel-fib.eta`,
   confirm worker threads each have a profile in the speedscope output.

Gate: speedscope viewer renders the cookbook quant workload; sampling
overhead ≤ 5 % at 1 kHz.

### Phase 3 — Reporting, merge, additional formats

1. `eta prof report`, `eta prof merge`, `eta prof view`.
2. `eta-prof` archive format (zstd-framed) + standalone `eta_prof`
   binary under `eta/tools/prof/`.
3. Optional pprof writer (CMake `ETA_PROF_PPROF=ON`).
4. Chrome trace JSON writer.
5. Counters surfaced in report.

Gate: `eta prof merge` produces a single report from N concurrent runs;
pprof opens cleanly when enabled.

### Phase 4 — Allocations and notebook integration

1. Coarse allocation profiling: hook `runtime/memory/heap.h` allocator
   to bump per-frame `bytes_allocated`; surface in flat report.
2. Notebook integration: `cookbook/notebooks/` magic
   `%%prof [--mode sample]` powered by `xeus`; renders an inline
   speedscope iframe.
3. REPL `:prof` meta-command in `etai`.

Gate: a notebook cell can profile and visualise without touching the
shell.

### Phase 5 — Hardening

1. Long-run stability test: 30-min `xva-wwr` simulation under sampling;
   memory growth bounded.
2. Public API freeze on `std/prof` (semver from here).
3. Documentation finalised; `docs/guide/profiling.md` published.

Gate: 1.0 — `std/prof` API and `eta-prof` archive format frozen.

---

## 8) Testing strategy

1. **Unit tests** under `eta/core/tests/prof/`:
   - `frame_id` interning correctness and thread-safety.
   - `Aggregator` exact arithmetic on synthetic event streams.
   - `SampleBuffer` SPSC stress test (1 producer, 1 consumer, 10 M
     samples, no losses, no torn reads).
   - Speedscope writer schema conformance against pinned fixtures.
2. **Integration tests** under `cookbook/tests/integration/prof/`:
   - `recover_recursion.eta`: profile `(fib 30)` in trace mode; assert
     `fib` is the dominant frame and call count matches the closed
     form.
   - `recover_concurrency.eta`: profile a 4-worker `parallel-fib`;
     assert ≥ 4 thread profiles in the speedscope output and that
     worker self-time sums to within 10 % of wall × workers.
   - `recover_tail_call.eta`: deeply tail-recursive loop reports a
     bounded shadow-stack depth.
3. **Overhead benchmarks** in `eta/qa/bench/prof/`:
   - `prof_off_overhead`: assert ≤ 1 % wall regression vs baseline on
     `european.eta`.
   - `prof_sample_overhead`: assert ≤ 5 % at 1 kHz on `european.eta`.
   - `prof_trace_overhead`: assert ≤ 50 % on `european.eta`.
   Numbers are CI-gated on a tagged self-hosted runner; a soft warning
   on GitHub-hosted runners.
4. **Fuzz** via `eta/qa/fuzz/`: feed random Eta programs through
   `eta prof run` and assert no crashes / no unbounded memory growth.
5. **Property test**: for any program `P`, the sum over flat rows of
   `self_ns` equals the sampler-observed wall time within ±1 sample
   period.

---

## 9) Documentation deliverables

1. `docs/guide/profiling.md` — user guide: when to use sampling vs
   trace, CLI reference, in-language API, reading the report,
   speedscope walkthrough, common pitfalls (cold starts, JIT-warmup-ish
   effects from constant interning).
2. `docs/stdlib.md` — append `std/prof` section.
3. `docs/architecture.md` — append "Profiling" subsection covering the
   VM hooks and shadow-stack model.
4. `docs/release-notes.md` — entry per phase.
5. `eta/tools/prof/README.md` — how to add a new output format.
6. Update `docs/next-steps.md` to add a link to this plan once Phase 0
   lands.

---

## 10) Risks and open questions

1. **Hot-path overhead even when off.** Mitigated by branchless-off
   guard + microbenchmark gate (Phase 0 acceptance). If the gate fails
   we fall back to a build-time switch (`ETA_ENABLE_PROFILER`,
   default ON in debug, opt-in in release).
2. **Tail calls and self-time accounting.** Tail-called frames inherit
   the caller's wall window; we explicitly *replace* the stack top
   rather than push, so inclusive time stays correct and self time is
   attributed to the callee. Documented in `docs/guide/profiling.md`.
3. **Continuations / `call/cc`.** Resuming a continuation can jump
   arbitrarily up the stack. We snapshot the shadow stack on capture
   and restore it on invoke, then emit synthetic
   `on_continuation_jump`. Any time spent between capture and resume
   is attributed to the resumer, not the original capturer.
4. **Sampler racing the shadow stack.** Acceptable by design (samples
   are statistical). The acquire/release pair around `stack.size()`
   avoids torn reads; out-of-bounds reads are impossible because the
   `vector`'s storage is reserved upfront (`stack.reserve(1024)` on
   thread spawn; grows only at safe points).
5. **Builtin granularity.** A single builtin like `std/math:exp` may
   call into libtorch; we attribute *all* of that time to `exp`. To
   drill further, point a platform profiler (`samply`, `perf`,
   `Instruments`, ETW) at the process — a recipe for that lives in
   `docs/guide/profiling.md`.
6. **Anonymous lambdas.** Labelled by definition span; if the same
   `lambda` is closed over with different upvals they share a
   `FrameId`. Documented; users wanting to disambiguate use
   `prof/region`.
7. **`--profile` flag clash.** Resolved by using `--prof` for
   profiling. Lint rule (in `eta_lint`, future) can flag CLI
   documentation that confuses the two.
8. **Speedscope schema drift.** Pin a tested schema version in
   `eta/core/src/eta/runtime/prof/speedscope.h`; bump deliberately.
9. **Windows clock resolution.** `steady_clock` on MSVC is QPC-backed
   (~100 ns) which is enough; verified in Phase 0 bench.
10. **Notebook iframe sandboxing.** Phase 4 must serve speedscope from
    a local copy bundled with `xeus-eta`; do not depend on
    `speedscope.app` being reachable.

---

## 11) Milestones / acceptance criteria (combined v1)

The profiler v1 is complete when **all** of the following hold:

- [ ] `cmake --build` produces an `eta` binary with the `prof`
      subcommand and a standalone `eta_prof` viewer; `(import std/prof)`
      resolves and exposes the documented API.
- [ ] `eta prof run --mode=sample cookbook/quant/european.eta` produces
      a `prof.speedscope.json` that opens in speedscope and shows
      `simulate-path` and `price` as dominant frames.
- [ ] `eta prof run --mode=trace cookbook/basics/recursion.eta` reports
      exact call counts for `fact` matching the analytic value.
- [ ] Concurrent profiling: a 4-worker `parallel-fib` run yields ≥ 4
      thread profiles in the speedscope export.
- [ ] Overhead gates green: ≤ 1 % off, ≤ 5 % sampling at 1 kHz, ≤ 50 %
      trace.
- [ ] `eta prof merge` reproduces the union of two single-thread runs
      bitwise-identically to a single two-thread run (modulo timestamps).
- [ ] Pretty, JSON, speedscope, and (if `ETA_PROF_PPROF=ON`) pprof
      reporters all conformance-tested against pinned fixtures.
- [ ] `std/prof` covered by unit + integration tests; `prof/with`,
      `prof/region`, `prof/counter` exercised in cookbook recipes.
- [ ] `docs/guide/profiling.md`, `docs/stdlib.md` (`std/prof` section),
      `docs/architecture.md` ("Profiling" subsection), and
      `docs/release-notes.md` entries published.
- [ ] `docs/next-steps.md` links to this plan.
- [ ] Notebook `%%prof` magic renders an inline flamegraph in
      `cookbook/notebooks/`.

