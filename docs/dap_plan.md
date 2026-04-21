# DAP Implementation Plan

[← Back to README](../README.md) · [Next Steps](next-steps.md) ·
[Architecture](architecture.md) · [Bytecode & VM](bytecode-vm.md)

---

## Goal

Bring `eta_dap` to a **best-in-class debugging surface** before any
overhaul of the VS Code extension.  The extension work in
[next-steps.md §2](next-steps.md) is largely a *capability flip* once
the adapter advertises the right `supports*` flags — so the adapter
goes first.

This plan is **stage-by-stage**, each stage independently buildable,
testable, and shippable.  Every section names the concrete files to
touch and the existing types / functions it builds on, so any
contributor can pick up a stage without re-reading the whole DAP.

---

## Current Adapter Surface (Baseline)

Source: [`eta/dap/src/eta/dap/dap_server.{h,cpp}`](../eta/dap/src/eta/dap/dap_server.cpp)

### Advertised capabilities (`handle_initialize`)

| Capability | State |
|---|---|
| `supportsConfigurationDoneRequest` | ✅ |
| `supportsSetBreakpoints` | ✅ |
| `supportsTerminateRequest` | ✅ |
| `supportsEvaluateForHovers` | ✅ |
| `supportsCompletionsRequest` | ✅ |
| `supportsFunctionBreakpoints` | ❌ |
| `supportsConditionalBreakpoints` | ❌ |
| `supportsSetVariable` | ❌ |
| `supportsRestartRequest` | ❌ |
| `supportsBreakpointLocationsRequest` | ❌ |
| `supportsStepBack` | ❌ (out of scope — no time travel) |
| `supportsGotoTargetsRequest` | ❌ (low priority) |

### Custom protocol extensions

| Request | Purpose |
|---|---|
| `eta/heapSnapshot` | Drives the Heap Inspector webview |
| `eta/inspectObject` | Drill-down on GC root tree entries |
| `eta/disassemble` | Bytecode for the Disassembly view |
| `eta/childProcesses` | Snapshot of `spawn`-ed actor processes |

### VM-side debug substrate

The DAP layer is thin: most of the heavy lifting lives in
[`debug_state.h`](../eta/core/src/eta/runtime/vm/debug_state.h) and is
already plumbed into the VM hot loop via a single null check.

| Type / API | Used for |
|---|---|
| `DebugState::set_breakpoints(std::vector<BreakLocation>)` | Replace the breakpoint table |
| `DebugState::check_and_wait(span, depth)` | Hot-loop hook on every instruction |
| `StopEvent { reason, span, exception_text }` | Carried back to the DAP via `StopCallback` |
| `DebugState::request_pause()` | Async pause |
| `DebugState::step_over/in/out(span, depth)` | Step requests |
| `DebugState::notify_exception(msg, span)` | Unhandled-exception stop |

### Important constraint discovered during code review

> `handle_evaluate` currently **does not run arbitrary expressions
> while paused** — it does only name lookups in the live frame /
> globals. Comment in `dap_server.cpp` at line ~764:
>
> > "When the VM is paused mid-execution, calling `run_source()` would
> > invoke `vm_.execute()` on the live stack/frame state, corrupting
> > it and breaking subsequent stepping."
>
> Conditional breakpoints, watch expressions, and `setVariable` all
> need a **real sandboxed evaluator** that does not perturb the
> paused VM.  This is the hidden prerequisite for every Stage ≥ 2
> work item.  See **Stage 0 — Sandboxed Evaluator** below.

---

## Stage 0 — Sandboxed Evaluator (Prerequisite)

**Why first.** Three of the user-visible features below
(conditional breakpoints, logpoints, watch / `setVariable`) are
**impossible** without a way to evaluate an Eta expression *against a
paused frame's environment* without disturbing the live stack.

**Approach.** Add a `vm::Sandbox` helper that, given a paused VM,
runs an expression on a **shadow execution context**:

1. Compile the expression on the DAP thread via the existing
   `Driver::compile_string(...)` path (this is already exception-safe
   and produces a fresh `BytecodeFunction`).
2. Construct a transient evaluation environment whose **lexical
   parent** is the paused frame's environment record, i.e. local
   slots + upvalues are read-only visible.  Globals are visible
   read-only.
3. Run the compiled function on a **separate `Stack`** (heap-owned,
   not the VM's main stack) using a re-entrant mode of `VM::execute`
   that takes the stack as an argument.  No trail entries, no GC
   roots leak into the main run.  Mutating builtins return a
   `read-only` runtime error in this mode (gated by a `Sandbox`
   flag on `VM`).
4. On completion, free the shadow stack.  The main VM state is
   unchanged byte-for-byte.

**Files to add / edit:**

| File | Change |
|---|---|
| `eta/core/src/eta/runtime/vm/sandbox.h` *(new)* | `class Sandbox` API: `eval(LispVal& out, std::string expr, FrameRef frame)` |
| `eta/core/src/eta/runtime/vm/vm.h` | Add `execute_in(Stack& shadow, BytecodeFunction& fn, EnvRef env)` overload + read-only mode flag |
| `eta/core/src/eta/runtime/vm/vm.cpp` | Implement read-only guard for mutating opcodes (`Set!`, `Define`, `Bind`, etc.) when `sandbox_mode_ == true` |
| `eta/dap/src/eta/dap/dap_server.cpp` | New private helper `bool eval_in_paused_frame(int frame_idx, std::string expr, std::string& out_str, uint64_t* out_val);` used by every Stage ≥ 2 handler |

**Tests.** Add `eta/test/src/vm/sandbox_test.cpp`:

- read of locals / upvalues / globals returns the expected values
- attempted `(set! x ...)` returns a `runtime::vm::SandboxViolation`
- after sandbox eval, `vm_.stack().size()` and `vm_.frames().size()`
  are byte-identical to before
- exception inside the expression does not unwind the live trail

**Definition of done.** `eta_core_test` passes a regression that:
1. pauses the VM at a known span,
2. evaluates `(+ x y)` in that frame,
3. asserts the returned formatted value, and
4. asserts that resuming reaches the next breakpoint with stepping
   intact.

---

## Stage 1 — `breakpointLocations` and Source-Map Audit

**Goal.** Stop VS Code from silently shifting breakpoint clicks to
arbitrary lines.  This is purely additive and ships **before**
conditional breakpoints because it makes every later test more
reliable (you know exactly which lines can hold a breakpoint).

**Implementation.**

- Each `BytecodeFunction` already carries `source_map[pc]` (Span).
  Aggregate per-file: build `std::unordered_map<file_id,
  std::set<uint32_t>>` of valid lines on `Driver` startup and again
  after each `(load …)`.
- Add `bool DapServer::handle_breakpoint_locations(const Value& id,
  const Value& args)` returning the subset of requested lines that
  exist.
- Flip `supportsBreakpointLocationsRequest` to `true`.

**Files:** `dap_server.{h,cpp}`, `interpreter/driver.{h,cpp}`
(`std::set<uint32_t> Driver::valid_lines_for(file_id)`).

**Tests.** New golden exchange (`eta/test/src/dap/`) drives a small
`.eta` script and asserts that `breakpointLocations` returns the
correct set for known empty-line / comment-only / multi-statement
lines.

---

## Stage 2 — Conditional Breakpoints

**Builds on:** Stage 0 (sandboxed evaluator).

### Wire-level work

`setBreakpoints` carries an optional `condition: string` per
`SourceBreakpoint`.  Today `handle_set_breakpoints` ignores it.

1. Extend `struct PendingBp` in `dap_server.h`:
   ```cpp
   struct PendingBp {
       int          line;
       int          id;
       std::string  condition;     ///< empty == unconditional
       std::string  hit_condition;  ///< parsed by adapter, see below
       std::string  log_message;    ///< Stage 3
   };
   ```
2. Pass the condition through to `DebugState`:
   - Add `BreakLocation::condition` (string, empty == unconditional)
     and a per-location optional `hit_count_target`.
   - When `check_stop` matches a breakpoint, look up its condition; if
     non-empty, queue a callback to the DAP layer that:
     - calls `eval_in_paused_frame(0, condition, out, &val)`,
     - converts `val` to truthy / falsy via `nanbox::is_truthy(val)`,
     - if false, immediately `resume()` the VM (no `stopped` event sent).
3. Capability flip: `supportsConditionalBreakpoints = true`.

### Hit-count conditions

VS Code passes arbitrary strings in `hitCondition`.  Support the
common shapes:

| Pattern | Meaning |
|---|---|
| `"5"` | break only on the 5th hit |
| `">= 5"` | break on the 5th hit and after |
| `"% 10"` | break every 10th hit |

Parsing lives in `dap_server.cpp` (small lambda); evaluation lives in
`DebugState` next to the existing breakpoint match.

### Tests

- Conditional that always returns `#t` behaves exactly like an
  unconditional breakpoint.
- Conditional that returns `#f` produces zero `stopped` events across
  100 hits.
- `hitCondition` `"3"` produces exactly one stop on hit #3.
- Malformed condition logs a single `output` event and falls back to
  unconditional.

---

## Stage 3 — Logpoints

**Builds on:** Stage 2 (the plumbing is identical).

VS Code encodes logpoints as `SourceBreakpoint { logMessage: string }`.
Per the DAP spec, when a logpoint hits we **do not** emit `stopped`;
instead we expand `{expr}` interpolations and emit an `output` event,
then continue automatically.

1. Re-use the `PendingBp::log_message` field added in Stage 2.
2. In the breakpoint-callback path:
   - Tokenise `log_message` into literal text + `{ … }` segments.
   - For each `{…}`, call `eval_in_paused_frame(0, segment, out, …)`
     and substitute the formatted result.
   - `send_event("output", { category: "console", output: rendered })`.
   - `resume()` immediately.
3. No new capability flag required (DAP infers logpoint support from
   conditional-breakpoint support + the `logMessage` field).

**Tests.** Logpoint `"x = {x}"` on a loop produces N output events
with the expected substitutions and **zero** `stopped` events.

---

## Stage 4 — Function Breakpoints

**Goal.** Let the user say "break on entry to `factorial`" without
opening the source file.

### Wire-level

DAP uses `setFunctionBreakpoints` with an array of `{ name,
condition?, hitCondition? }`.  No source / line is supplied — the
adapter must resolve `name` to a function entry.

### Symbol resolution

Eta functions are bytecode-linked via the emitter.  Each
`BytecodeFunction` has a name (qualified, e.g. `composition.top5`)
and an entry source span (the `defun` / `lambda` form).

1. Build `std::unordered_map<std::string, std::vector<BreakLocation>>`
   on the `Driver`: name → list of `(file_id, entry_line)`.  Populate
   on each load / (re)compile.
2. Match strategies, in order:
   - Exact full name (`composition.top5`)
   - Short name (`top5`) — falls back to "ambiguous" if multiple match
   - Glob (`std.io.*`) — Stage 4.5 if needed
3. Set a regular `BreakLocation` for each resolved entry.  This means
   function breakpoints are **just line breakpoints in disguise** at
   the VM layer — no new debug-state code required.
4. Capability flip: `supportsFunctionBreakpoints = true`.

### New handler

```cpp
void DapServer::handle_set_function_breakpoints(const Value& id, const Value& args);
```

Wired in `dispatch()` next to `setBreakpoints`.

### Tests

- Setting `factorial` on `examples/hello.eta` stops at the function's
  entry span.
- Ambiguous short name returns `verified: false` with a `message`
  field listing candidates.
- Conditional + hit-count work the same as line breakpoints (re-uses
  Stage 2 plumbing).

---

## Stage 5 — `setVariable`

**Builds on:** Stage 0.

### Scope

Restricted to:

- Local frame slots (`scope = 0` from `encode_var_ref`)
- Module globals
- Upvalues — read-only for now (mutation requires walking the closure
  chain; defer)

Compound child slots (cons cars / vector cells) are explicitly
out-of-scope for v1 — a separate design pass.

### Implementation

1. New handler `handle_set_variable(id, args)` reading
   `variablesReference`, `name`, `value`.
2. Compile `value` via the same path as Stage 0's sandbox; require
   that the compiled expression is **side-effect-free** (sandbox mode
   already enforces this).  Evaluate it; bind the result.
3. Mutation path uses the existing trail:
   - For frame slots: `vm_.set_local(frame_idx, slot_idx, val)` writing
     a `TrailEntry::Bind`.
   - For globals: `vm_.set_global(slot, val)` writing a
     `TrailEntry::GlobalSet` (new variant).
4. Trail-aware `setVariable` means changes survive forward execution
   but are **rolled back** if the VM later backtracks past the trail
   mark — exactly what users expect when debugging logic / CLP code.
5. Capability flip: `supportsSetVariable = true`.

### Tests

- Set a local, continue, observe new value at next stop.
- Set a global, observe it via `evaluate`.
- Attempt to set a value containing `(set! …)` → error response with
  `SandboxViolation` text.
- `setVariable` from inside a `(findall …)` and observe rollback on
  backtrack.

---

## Stage 6 — `restartRequest`

### Wire-level

DAP's `restart` request signals "tear down the run, start it again
with the same arguments".  Today the user must `disconnect` and
re-launch.

### Implementation

1. Cache the original `launch` arguments in a `Value last_launch_args_`.
2. New handler `handle_restart(id, args)`:
   - `driver_->vm().request_pause()`, then `terminate` the VM thread
     cleanly.
   - Reset transient state (`pending_bps_` is **kept** — that's the
     whole point of restart).
   - Re-run the body of `handle_launch(last_id, last_launch_args_)`.
   - Send `process` event with the new PID-equivalent.
3. Capability flip: `supportsRestartRequest = true`.

### Tests

- Set a breakpoint, launch, hit it, continue to exit, restart →
  breakpoint fires again with no manual reconfiguration.
- Restart while paused at a breakpoint also works (extra teardown
  path).

---

## Stage 7 — Per-Thread State for `spawn-thread` Actors

**This is the largest stage.**  It surfaces in-process thread actors
(`spawn-thread`, `spawn-thread-with`) as DAP threads so each can be
inspected and paused independently.

### Current state

- `handle_threads` reports a single fixed `{ id: 1, name: "main" }`.
- All `StopEvent`s carry `threadId: 1` regardless of which thread
  produced them.
- `DebugState` is **per-VM**, but each `spawn-thread` actor runs in
  its own `VM` instance with its own `DebugState`.

### Design

1. **Thread registry.**  Add `class ThreadRegistry` to
   `eta::runtime` (or piggy-back on the existing actor table that
   feeds `eta/childProcesses`).  Maps a stable `dap_thread_id` (int,
   monotonically allocated by the adapter) to:
   - the `VM*` (or owning shared_ptr<Driver> for `spawn-thread`)
   - the originating spawn span
   - a friendly name (`"actor:42 (worker-pool-worker)"`)
2. **Adapter responsibilities.**
   - Subscribe to actor lifecycle events (already plumbed for the
     Child Processes view; reuse).
   - For each new thread, install the same `set_stop_callback` used
     for the main VM, with the callback closing over the right
     `dap_thread_id`.
   - On thread exit, send `thread` event `{ reason: "exited", threadId
     }` and remove from registry.
3. **`handle_threads`** becomes:
   ```cpp
   Array out;
   for (const auto& t : threads_.list()) {
       out.push_back(json::object({
           {"id",   Value(static_cast<int64_t>(t.dap_id))},
           {"name", Value(t.display_name)},
       }));
   }
   send_response(id, json::object({{"threads", out}}));
   ```
4. **Stop / step targeting.**  Every per-thread request
   (`stackTrace`, `scopes`, `variables`, `continue`, `next`, `stepIn`,
   `stepOut`, `pause`) carries `threadId`.  Route by registry lookup.
5. **`allThreadsStopped`.**  Stays `false` for actor threads —
   pausing one actor must not freeze the others.  Stays `true` for
   the main VM (since the legacy callers expect it).

### Trickiest sub-problem

The existing `handle_evaluate` and Stage 0 sandbox both implicitly
target "the" VM.  Both must take a `dap_thread_id` and route via the
registry.  This is a one-time refactor: pass `VM&` explicitly through
all paused-state helpers, deleting the captured `driver_->vm()` calls.

### Tests

- A `spawn-thread`-using example produces multiple entries in
  `threads`.
- Pause one actor → other actors continue running (verify by
  observing further `output` events from the others).
- Step in actor 2 stops only actor 2.
- Killed actor produces `thread { reason: "exited" }`.

---

## Stage 8 — Diagnostics: `--trace-protocol`

A no-feature, high-value win.  Adds a flag to `eta_dap`:

```
eta_dap --trace-protocol [path]
```

Behaviour:

- If `path` is omitted, write to `stderr`.
- Otherwise, append every parsed request and outgoing
  response / event to `path` as one JSON object per line.
- Add a sequence number and a wall-clock timestamp.
- Wraps `read_message` and `send`.

This eliminates whole classes of "did the message arrive?" bugs and
becomes the foundation for Stage 9 (golden tests).

**Files:** `eta/dap/src/eta/dap/main_dap.cpp` (CLI parsing),
`dap_io.{h,cpp}` (tracer hooks), tiny new `dap_trace.{h,cpp}`.

---

## Stage 9 — Test Coverage: Golden DAP Exchanges

**Builds on:** Stage 8 (the tracer doubles as the golden recorder).

### Harness

New target `eta_dap_test` (`eta/test/src/dap/`).  Drives `eta_dap`
out-of-process via stdin / stdout, framed Content-Length JSON-RPC.

```cpp
DapHarness h{"eta_dap"};
h.send_initialize();
h.send_launch("examples/hello.eta", /*stopOnEntry=*/true);
h.expect_event("stopped", {{"reason", "step"}, {"threadId", 1}});
h.send_continue(1);
h.expect_event("terminated");
```

### Initial test inventory

| Scenario | Stage covered |
|---|---|
| `initialize` advertises every flag we expect after each stage lands | regression for every flip |
| `setBreakpoints` round-trip on `examples/hello.eta` | baseline |
| `breakpointLocations` returns the correct set | Stage 1 |
| Conditional breakpoint that returns `#t` / `#f` | Stage 2 |
| Hit-count `"3"` stops on hit #3 | Stage 2 |
| Logpoint produces N output events, zero stops | Stage 3 |
| Function breakpoint by full name + short name + ambiguous | Stage 4 |
| `setVariable` for local + global; rejection of side-effecting RHS | Stage 5 |
| `restartRequest` re-arms breakpoints | Stage 6 |
| `spawn-thread` actor produces 2nd thread; pause one, other runs | Stage 7 |
| `--trace-protocol` produces a parseable line-JSON file | Stage 8 |

CI wires `eta_dap_test` into the existing `eta_rebuild_and_test`
target so every PR exercises the protocol surface.

---

## Stage 10 — `docs/dap.md` Reference

A standalone reference page (sibling to `next-steps.md`) documenting:

- The standard DAP capabilities Eta supports.
- The custom `eta/*` requests + their JSON shapes (today
  `heapSnapshot`, `inspectObject`, `disassemble`, `childProcesses`).
- The deferred-initialization handshake (already documented inline,
  worth promoting).
- The thread model after Stage 7 (`allThreadsStopped` semantics).
- The sandbox-eval guarantees from Stage 0 (what conditions /
  logpoints / `setVariable` *cannot* do).

This page is also where editor authors targeting Eta (Emacs DAP-mode,
Neovim's nvim-dap, JetBrains DAP plugin) will land — ship it.

---

## Sequencing & Sizing

| Stage | Depends on | Rough size | Risk |
|---|---|---|---|
| 0 — Sandbox eval | — | M (1–2 wk) | High (touches VM hot path) |
| 1 — `breakpointLocations` | — | S (1–2 d) | Low |
| 2 — Conditional breakpoints | 0, 1 | M (1 wk) | Medium |
| 3 — Logpoints | 2 | S (1–2 d) | Low |
| 4 — Function breakpoints | 1 | S (2–3 d) | Low |
| 5 — `setVariable` | 0 | M (3–5 d) | Medium (trail interaction) |
| 6 — Restart | — | S (1–2 d) | Low |
| 7 — Per-thread state | 0, 6 | L (2–3 wk) | High (concurrency) |
| 8 — `--trace-protocol` | — | S (1 d) | Low |
| 9 — Golden tests | 8 | M (rolls out per-stage) | Low |
| 10 — `docs/dap.md` | all of above | S (1–2 d) | Low |

### Recommended order

1. **Stage 8** first — gives us trace logs to debug everything else.
2. **Stage 1** + **Stage 6** in parallel — both small and unblock
   user-visible wins immediately.
3. **Stage 0** — the prerequisite for everything else.
4. **Stage 2 → 3 → 5 → 4** — feature waves, each with golden tests
   added as we go (Stage 9 is incremental, not a big-bang).
5. **Stage 7** — last and on its own; it touches the most code.
6. **Stage 10** — closes the work.

---

## Cross-cutting Concerns

### Thread safety

Stage 0's sandbox evaluator runs on the **DAP thread** while the VM
thread is parked at a stop point.  `vm_mutex_` already serialises
this; the sandbox must not re-enter the VM thread.  Asserts in debug
builds (`assert(vm_->is_paused())` before any sandbox eval) catch
violations early.

### Backwards compatibility

- All capability flips are additive — clients that don't ask for the
  new features see no behaviour change.
- The custom `eta/*` extensions are unchanged.
- The deferred-initialization handshake stays as-is; it's load-bearing
  for breakpoint timing.

### Performance

- Stage 0's sandbox is a **single extra null-pointer test** in the VM
  hot loop (the existing `if (debug_)` already gates everything; the
  sandbox only runs when paused, never on the hot path).
- Conditional / logpoint evaluation runs on the DAP thread; the VM is
  paused while it executes.  Worst case is "user wrote a very slow
  condition" — which is the user's bug, and the trace log will
  surface it.

### What we are NOT doing

- **Time travel / `stepBack`.**  Would require record-and-replay; out
  of scope for this plan.
- **DAP `attach` flow.**  Eta is launch-only; `attach` is a separate
  design (would need a debug socket on `etai`).
- **Memory write breakpoints.**  Niche, expensive, no current demand.
- **Compound `setVariable`** (editing inside a cons / vector).  Niche;
  defer until the simple cases are solid.

---

## Definition of Done (for the whole plan)

1. Every box in the §"Advertised capabilities" table at the top is
   ✅, except the explicitly-out-of-scope rows.
2. `eta_dap_test` runs on every CI build with ≥ 90% line coverage of
   `dap_server.cpp`.
3. `docs/dap.md` exists, links from `next-steps.md` and `README.md`.
4. The VS Code extension picks up conditional breakpoints, logpoints,
   function breakpoints, watch-edit, restart, and per-thread pause
   **with no extension-side code changes** — proving the adapter is
   the right place to invest.

---

## Source Locations Referenced

| Component | File |
|---|---|
| DAP server | [`eta/dap/src/eta/dap/dap_server.{h,cpp}`](../eta/dap/src/eta/dap/dap_server.cpp) |
| DAP CLI | [`eta/dap/src/eta/dap/main_dap.cpp`](../eta/dap/src/eta/dap/main_dap.cpp) |
| DAP framing | [`eta/dap/src/eta/dap/dap_io.{h,cpp}`](../eta/dap/src/eta/dap/dap_io.cpp) |
| VM debug substrate | [`eta/core/src/eta/runtime/vm/debug_state.h`](../eta/core/src/eta/runtime/vm/debug_state.h) |
| Bytecode + source maps | [`eta/core/src/eta/runtime/vm/bytecode.h`](../eta/core/src/eta/runtime/vm/bytecode.h) |
| VM façade | [`eta/core/src/eta/runtime/vm/vm.h`](../eta/core/src/eta/runtime/vm/vm.h) |
| Driver | [`eta/interpreter/src/eta/interpreter/driver.h`](../eta/interpreter/src/eta/interpreter/driver.h) |
| VS Code extension | [`editors/vscode/src/`](../editors/vscode/src/) |

