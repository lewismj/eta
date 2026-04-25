# DAP + VS Code Extension — Production-Grade Plan

[← Back to README](../README.md) · [Next Steps](next-steps.md) ·
[Existing DAP plan (capability flips)](dap_plan.md) ·
[Architecture](architecture.md) · [Bytecode & VM](bytecode-vm.md)

---

## Scope

This is a **comprehensive**, end-to-end plan to elevate the Eta debugging
experience from "functional" to "production-grade". It complements the
existing [`dap_plan.md`](dap_plan.md), which is narrowly focused on
flipping the remaining DAP capability flags. This document covers:

1. **DAP server** (`eta/dap/src/eta/dap/`) — protocol surface, paused-VM
   eval, multi-thread debugging, transport diagnostics.
2. **VS Code extension** (`editors/vscode/`) — UX, language services,
   configuration ergonomics, packaging, distribution, telemetry.
3. **Cross-cutting concerns** — testing, performance, accessibility,
   docs, marketplace polish.

Each stage names exact files / symbols and is independently shippable.

---

## Audit of the Current Implementation

> All claims in this section come from a fresh read of the source
> (April 2026), not from prior docs.

### DAP server — what actually exists

Source: [`eta/dap/src/eta/dap/dap_server.{h,cpp}`](../eta/dap/src/eta/dap/dap_server.cpp)
(1410 lines), [`dap_io.{h,cpp}`](../eta/dap/src/eta/dap/dap_io.cpp),
[`main_dap.cpp`](../eta/dap/src/eta/dap/main_dap.cpp) (26 lines).

**Capabilities advertised** (`handle_initialize`, lines 194–223):

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
| `supportsStepBack` | ❌ (out of scope) |
| `supportsGotoTargetsRequest` | ❌ |
| `supportsExceptionInfoRequest` | ❌ (yet `setExceptionBreakpoints` is handled) |
| `supportsExceptionFilterOptions` | ❌ |
| `supportsLogPoints` | ❌ |
| `supportsHitConditionalBreakpoints` | ❌ |
| `supportsDataBreakpoints` | ❌ |
| `supportsDisassembleRequest` | ❌ (we have a *custom* `eta/disassemble`, not the standard one) |
| `supportsSteppingGranularity` | ❌ |
| `supportsValueFormattingOptions` | ❌ |
| `supportsClipboardContext` | ❌ |
| `supportsModulesRequest` | ❌ |
| `supportsLoadedSourcesRequest` | ❌ |
| `supportsReadMemoryRequest` | ❌ |
| `supportsCancelRequest` | ❌ |
| `supportsTerminateThreadsRequest` | ❌ |

**Custom `eta/*` extensions:**

| Request | Purpose | Live |
|---|---|---|
| `eta/heapSnapshot` | Heap Inspector webview source | ✅ |
| `eta/inspectObject` | GC root drill-down | ✅ |
| `eta/disassemble` | Custom bytecode view | ✅ |
| `eta/childProcesses` | Spawned actor process tree | ✅ |

**Custom event** `eta-output` (lines 405, 409): script `display`/`error`
text routed to a dedicated VS Code output channel so it doesn't corrupt
the Content-Length-framed protocol stream.

**Threads** (`handle_threads`, lines 520–539): exposes the main VM
thread plus an entry per `spawn-thread` actor (when built with
`ETA_HAS_NNG`). However, **all stop events still carry `threadId: 1`**
(line 387) — actor threads cannot independently pause/step today.

**Paused-VM `evaluate`** (lines 750–838): explicitly does **only**
name lookups (locals → upvalues → globals exact → globals short). The
comment at line 763 documents *why*:
> "When the VM is paused mid-execution, calling `run_source()` would
> invoke `vm_.execute()` on the live stack/frame state, corrupting it."

This is the central blocker for conditional breakpoints, watch
expressions, `setVariable`, and logpoints.

**Transport** (`dap_io.cpp`): plain Content-Length framed JSON-RPC
over stdin/stdout. No tracing, no message-id correlation log, no
cancellation.

**Test surface**: `eta/test/src/dap_tests.cpp` (910 lines) already
exists with in-process DapServer + std::stringstream injection. The
existing `dap_plan.md`'s "Stage 9 — golden tests" claim of "no automated
coverage" is **out of date**. The harness runs synchronously
(server.run() drains the framed input, then exits) — good for protocol
shape tests, **inadequate** for tests that need to interact with a paused
VM, observe `stopped`/`continued`, then send a `continue`. We need an
async harness (Stage C0 of this plan).

### VS Code extension — what actually exists

Source: [`editors/vscode/`](../editors/vscode/), version `0.3.0`.

| File | Lines | Purpose |
|---|---|---|
| `package.json` | 274 | Manifest |
| `src/extension.ts` | 512 | Activation, LSP+DAP wiring, binary discovery, debug tracker |
| `src/heapView.ts` | 400 | Heap Inspector webview |
| `src/gcRootsTreeView.ts` | 221 | GC roots tree |
| `src/disassemblyView.ts` | 74 | Virtual-document disassembly |
| `src/disassemblyTreeView.ts` | 101 | Tree-view disassembly |
| `src/childProcessTreeView.ts` | 102 | Spawned actor tree |
| `src/testController.ts` | 284 | TAP-driven Test Explorer |
| `src/dapTypes.ts` | 51 | Shared response types |
| `snippets/eta.json` | 217 | ~20 snippets |

**Manifest gaps observed in `package.json`:**

- No `icon` set (Marketplace shows a placeholder).
- No `galleryBanner`, `badges`, `pricing`, `qna`, `bugs`, `homepage`.
- `categories` is `["Programming Languages"]` only — could add
  `["Debuggers", "Snippets", "Linters"]`.
- `breakpoints` is declared once at root and once inside
  `debuggers[].breakpoints` (redundant, harmless).
- `configurationAttributes.launch` exposes only `program` and
  `stopOnEntry`. Missing: `args`, `cwd`, `env`, `modulePath`,
  `etacFile` (run pre-compiled bytecode), `console` (integrated vs
  external terminal).
- `debuggers[].request` only declares `launch`; no `attach` flow.
- No `views/welcome` contribution — empty trees show no helpful
  "click here" guidance.
- `activationEvents` is over-specific in modern VS Code (≥ 1.74,
  the engines pin); most can be removed in favour of contribution-based
  activation.

**Extension code gaps observed:**

- Binary search paths in `extension.ts` (lines 99–116, 144–158,
  41–51 of `testController.ts`) are duplicated and hard-coded for a
  fixed list of CMake build dirs. A shared helper + workspace-config
  would dedupe.
- `EtaDebugAdapterTracker` (lines 372–488) does **per-message
  pretty-print logging** but never persists raw protocol traces; when
  things go wrong a user has nothing to attach to a bug report.
- `HeapInspectorPanel.refresh()` (`heapView.ts` line 64) sends
  `eta/heapSnapshot` — but the DAP server returns an error if the VM
  is **not paused** (`dap_server.cpp` line 933). The current
  webview shows an alarming "VM must be paused" error instead of an
  empty/disabled state.
- `disassemblyView.ts` returns a `currentPC` from the server — but
  the server (`dap_server.cpp` line 1119) hard-codes `currentPC = -1`.
  Result: the "◀ PC" indicator in `disassemblyTreeView.ts` line 67
  never lights up.
- Tracker `onDidSendMessage` ignores `eta/childProcesses` results —
  child-process tree only refreshes on `stopped`, missing actors that
  spawn between stops.
- `EtaDebugConfigurationProvider.resolveDebugConfiguration` (lines
  340–367) does not propagate `args`, `env`, `cwd`, or
  `ETA_MODULE_PATH` from launch config to the DAP process — only the
  global setting.
- `testController.ts` parses TAP (lines 68–97) but only extracts
  `message: ` from the YAML diagnostic block; standard TAP YAML
  blocks include `severity`, `data`, `at` (line/col) — none surfaced.
- No `InlineValuesProvider`, no `EvaluatableExpressionProvider`, no
  document-link provider for `(import std.foo)` clauses, no
  `CodeLensProvider` for `(defun main ...)`.
- `outputChannel` is typed as `OutputChannel`, not
  `LogOutputChannel` (added in VS Code 1.74) — we can't filter by
  level in the UI.
- No CI publishing workflow for the `.vsix`.
- No automated tests for the extension itself
  (`@vscode/test-electron`).

### LSP coordination

- `eta_lsp` already advertises `semanticTokensProvider`
  ([`lsp_server.cpp` line 413](../eta/lsp/src/eta/lsp/lsp_server.cpp)).
  README says "no semantic tokens yet" — **stale documentation**, not
  missing functionality.
- The extension does not enable `textDocument/inlayHint`,
  `textDocument/codeAction`, `textDocument/codeLens`, or
  `textDocument/documentLink` server-side. These are LSP gaps that
  feed back into the editor experience.

---

## Plan Overview

The work is split into **four tracks**, each with multiple stages.
Tracks can progress in parallel; numbered stages within a track are
sequential.

| Track | Owner area | Stages |
|---|---|---|
| **A — DAP Adapter** | `eta/dap/`, `eta/core/.../debug_state.h`, `vm.h` | A0 → A8 |
| **B — VS Code Extension** | `editors/vscode/src/` | B0 → B7 |
| **C — Testing & Quality** | `eta/test/src/dap_tests.cpp`, new `editors/vscode/test/` | C0 → C3 |
| **D — Distribution & Docs** | `editors/vscode/`, `docs/` | D0 → D3 |

---

## Track A — DAP Adapter

### A0 — Sandboxed Evaluator (foundation, blocking)

**Why first.** Three of the user-visible features (conditional
breakpoints, logpoints, `setVariable`, watch expressions) all need a
way to evaluate an Eta expression *against a paused frame's
environment* without disturbing the live VM stack — exactly what the
existing `handle_evaluate` refuses to do (line 763).

**Approach**

1. Introduce `class Sandbox` in
   `eta/core/src/eta/runtime/vm/sandbox.{h,cpp}`. API:
   ```cpp
   struct SandboxResult {
       std::expected<nanbox::LispVal, RuntimeError> value;
   };
   SandboxResult eval(VM& paused_vm,
                      std::size_t frame_index,
                      const std::string& expr);
   ```
2. `Sandbox::eval` does:
    - Compile `expr` via a separate, fresh `interpreter::Driver`
      **using the same heap and intern table** as the paused driver
      (so symbols and heap object IDs interoperate).
    - Build a transient `Stack` (not the VM's main stack) and run the
      compiled init function with `vm_.execute_in(stack, fn, env)` — a
      new overload added to `VM`.
    - Reads of locals / upvalues / globals are visible read-only.
    - Mutating opcodes (`SetGlobal`, `SetUpvalue`, `Bind`, `PutAttr`,
      `Set!`, calls into mutating primitives) trip a `sandbox_mode_`
      check on `VM` and return `RuntimeError::SandboxViolation`.
3. Plumb a tiny helper into `dap_server.{h,cpp}`:
   ```cpp
   bool eval_in_paused_frame(int frame_idx,
                             const std::string& expr,
                             std::string& out_str,
                             nanbox::LispVal* out_val = nullptr,
                             std::string* out_error = nullptr);
   ```
4. Refactor `handle_evaluate` so that:
    - When `args.context == "watch"` or `"hover"` and the expression is
      not a bare identifier, it uses the sandbox.
    - Bare identifiers continue to use the cheap name-lookup fast path.
    - When not paused, the existing `run_source` path stays.

**Files**

| File | Change |
|---|---|
| `eta/core/src/eta/runtime/vm/sandbox.h` | new |
| `eta/core/src/eta/runtime/vm/sandbox.cpp` | new |
| `eta/core/src/eta/runtime/vm/vm.h` | add `execute_in(...)`, `set_sandbox_mode(bool)`, `bool sandbox_mode_` |
| `eta/core/src/eta/runtime/vm/vm.cpp` | gate mutating opcodes |
| `eta/core/src/eta/runtime/error.h` | add `SandboxViolation` variant |
| `eta/dap/src/eta/dap/dap_server.{h,cpp}` | add `eval_in_paused_frame` helper, route `evaluate` through it |

**Tests** (`eta/test/src/vm/sandbox_test.cpp`, new):

- read locals/upvalues/globals from a paused VM at known span
- side-effecting expression returns `SandboxViolation`
- after eval: `vm_.stack().size()`, `vm_.frames().size()`, trail size,
  globals snapshot all byte-identical to before
- recursive eval leaves the main VM intact
- exception inside sandbox does not unwind the live trail

**DoD.** A new test in `dap_tests.cpp` pauses the VM at a breakpoint,
sends `evaluate { context: "watch", expression: "(+ x 1)" }`, asserts
the formatted result, then sends `continue` and verifies stepping still
works.

---

### A1 — `breakpointLocations` and source-map audit

**Why early.** Without it, VS Code silently shifts breakpoint clicks to
the next "valid" line, making every later test (Stages A2–A4) flaky.

**Approach**

- Each `BytecodeFunction` carries `source_map[pc] -> Span`. Aggregate:
  on every successful `Driver::run_file` / `compile_file`, build
  `std::unordered_map<uint32_t /*file_id*/, std::set<uint32_t>>`.
- Expose via `Driver::valid_lines_for(uint32_t file_id) const`.
- New handler `DapServer::handle_breakpoint_locations(id, args)` returns
  the intersection of requested lines with the valid set.
- Flip `supportsBreakpointLocationsRequest` to `true`.

**Files:** `interpreter/driver.h`, `dap_server.{h,cpp}`.

---

### A2 — Conditional + hit-count breakpoints

**Builds on:** A0.

`setBreakpoints`'s `SourceBreakpoint` carries `condition?`,
`hitCondition?`, `logMessage?` — today all three are dropped on the
floor (line 281 of `dap_server.cpp` only reads `line`).

1. Extend `struct PendingBp`:
   ```cpp
   struct PendingBp {
       int          line;
       int          id;
       std::string  condition;
       std::string  hit_condition;
       std::string  log_message;
       int          hit_count{0};
   };
   ```
2. Stop callback (currently `dap_server.cpp` lines 370–390) becomes:
   ```cpp
   if (ev.reason == StopReason::Breakpoint) {
       auto* bp = find_pending_bp(ev.span);
       if (bp) {
           ++bp->hit_count;
           if (!matches_hit_condition(bp->hit_condition, bp->hit_count)) {
               vm_.resume();
               return;
           }
           if (!bp->condition.empty()) {
               nanbox::LispVal v;
               if (!eval_in_paused_frame(0, bp->condition, _, &v, _) ||
                   !nanbox::is_truthy(v)) {
                   vm_.resume();
                   return;
               }
           }
           if (!bp->log_message.empty()) {
               emit_logpoint(bp->log_message, /*frame=*/0);
               vm_.resume();
               return;
           }
       }
   }
   send_event("stopped", ...);
   ```
3. Hit-condition parser supports `"5"`, `">= 5"`, `"% 10"`, etc.
4. Capability flips:
   `supportsConditionalBreakpoints`,
   `supportsHitConditionalBreakpoints`,
   `supportsLogPoints`.

**Tests**

- 100-iteration loop with `condition: "(= i 50)"` produces exactly one
  stop.
- `hitCondition: "3"` stops on the 3rd hit only.
- Logpoint `"i = {i}, sum = {(* 2 i)}"` produces N output events,
  zero stops.
- Malformed condition emits one `output` event with the
  `SandboxViolation` text and falls back to unconditional break.

---

### A3 — Function breakpoints

**Wire-level:** `setFunctionBreakpoints` with array of
`{ name, condition?, hitCondition? }`.

**Symbol resolution.** Each `BytecodeFunction` has `name`. Build
`Driver::function_locations(): unordered_map<string,
vector<BreakLocation>>` mapping qualified names (e.g.
`composition.top5`) and short names (`top5`) to entry spans.

**Resolution strategy:**

1. Exact full name (`std.io.println`).
2. Short name (`println`) — if multiple match, return `verified: false`
   with `message: "ambiguous: ..."`.
3. Glob (`std.io.*`) — A3.5 if there's demand.

Function breakpoints reduce to line breakpoints at the VM layer (no
new `DebugState` code).

**Capability:** `supportsFunctionBreakpoints = true`.

---

### A4 — `setVariable`

**Builds on:** A0.

**Scope (v1):** local frame slots, module globals. Upvalues read-only.
Compound (cons-car / vec-cell) deferred.

**Approach**

1. New handler `handle_set_variable`.
2. Compile RHS through Stage A0 sandbox; reject side-effecting
   expressions.
3. Mutation:
    - Frame slot: `vm_.set_local(frame, slot, val)` — writes a
      `TrailEntry::Bind` so backtracking correctly rolls it back.
    - Global: `vm_.set_global(slot, val)` — needs a new
      `TrailEntry::GlobalSet` variant for trail-aware behaviour.
4. Capability flip: `supportsSetVariable = true`.

**Tests**

- Set a local, continue, observe new value at next stop.
- Set a global, observe via `evaluate`.
- Reject `(set! x ...)` as RHS with structured error response.
- Inside `(findall ...)`, set a variable, force a backtrack, observe
  rollback.

---

### A5 — Restart, terminateThreads, cancel

**Restart (`supportsRestartRequest`)**

- Cache `last_launch_args_: Value`.
- `handle_restart`: pause+terminate VM thread, reset transient state
  (keep `pending_bps_`, drop `compound_refs_`, drop `executed_modules_`
  bookkeeping), re-run launch body.

**TerminateThreads** (`supportsTerminateThreadsRequest`): per-thread
kill of `spawn-thread` actors. Reuses the actor table from
`process_manager()`.

**Cancel** (`supportsCancelRequest`): cancel `eta/heapSnapshot` on
huge heaps via an `std::atomic<bool> cancelled_` checked by
`heap.for_each_entry`'s lambda.

---

### A6 — Per-thread state for actor threads (largest)

**Current state**

- `handle_threads` lists all threads, but **stop events always carry
  `threadId: 1`** (`dap_server.cpp` line 387).
- Each `spawn-thread` actor has its own `VM` + `DebugState`, so each
  *can* pause independently — the adapter just isn't routing.

**Design**

1. `class ThreadRegistry` (in `eta::dap`). Maps a stable
   `dap_thread_id` (monotonically allocated) to:
    - `VM*` (raw pointer; ownership in `Driver`'s thread table)
    - originating spawn span
    - friendly name (`"actor:42 worker-pool-worker"`)
2. Add a small subscription API on `Driver`:
   ```cpp
   using ThreadEvent = std::variant<ThreadStarted, ThreadExited>;
   void Driver::on_thread_event(std::function<void(const ThreadEvent&)>);
   ```
3. For each new thread, install the same `set_stop_callback` used for
   the main VM, with the callback closing over the thread's
   `dap_thread_id`.
4. `handle_threads`:
   ```cpp
   Array threads;
   for (const auto& t : registry_.list()) {
       threads.push_back(json::object({
           {"id",   Value(t.dap_id)},
           {"name", Value(t.display_name)},
       }));
   }
   ```
5. Every per-thread request (`stackTrace`, `scopes`, `variables`,
   `continue`, `next`, `stepIn`, `stepOut`, `pause`, `evaluate`,
   `setVariable`) takes `threadId`. Lookup → route to the right `VM`.
6. `allThreadsStopped`: `false` for actor threads (pausing one actor
   must not freeze the others); `true` for the main VM.
7. Send `thread { reason: "started" | "exited", threadId }` events.

**Refactor cost.** A0's `eval_in_paused_frame` and every helper that
currently does `driver_->vm()` must take a `VM&` explicitly.

---

### A7 — Standard `disassemble` request and stepping granularity

**`supportsDisassembleRequest = true`.** DAP defines a standard
`disassemble` request. Today we have only the custom `eta/disassemble`
returning a blob of text.

Support both: keep the custom for the existing view, add the standard
one returning structured `DisassembledInstruction[]` so VS Code's
built-in **Open Disassembly View** works.

**`supportsSteppingGranularity = true`.** Plumb `granularity` through
to a new `DebugState::step_over_instruction()` that ignores source-line
boundaries.

---

### A8 — Diagnostics: `--trace-protocol`

**CLI** (`main_dap.cpp`, currently 26 lines):

```
eta_dap [--trace-protocol [path]] [--log-level=info|debug|trace]
```

- If `path` is omitted, write to `stderr`.
- Otherwise append one JSON object per line: `{ "ts": ..., "dir":
  "in"|"out", "seq": ..., "msg": {...} }`.
- Wraps `read_message` / `write_message` in `dap_io.cpp` via a thin
  hook.

**File:** new `dap_trace.{h,cpp}`. One-day stage; ship **first** in the
order of operations because it makes every later stage easier to debug.

---

## Track B — VS Code Extension

### B0 — Structural cleanup (foundation)

1. **Centralise binary discovery** into `src/binaries.ts`:
   ```ts
   export interface EtaBinaries { lsp?: string; dap?: string; test?: string; etac?: string; etai?: string; }
   export function discoverBinaries(ctx: ExtensionContext): EtaBinaries;
   ```
2. **Switch to `LogOutputChannel`** for level-filtered logs.
3. **Move hard-coded build paths into a setting**
   `eta.binaries.searchPaths: string[]`.
4. **Delete redundant `breakpoints` declaration** (`package.json`
   line 203 vs 211).
5. **Simplify `activationEvents`.** Modern VS Code (≥ 1.74) activates
   on contribution points.
6. **Add `icon`, `repository`, `bugs`, `homepage`, `galleryBanner`,
   `categories: ["Programming Languages", "Debuggers", "Snippets"]`**.

---

### B1 — Launch configuration ergonomics

```jsonc
"configurationAttributes": {
  "launch": {
    "required": ["program"],
    "properties": {
      "program":     { "type": "string", "default": "${file}" },
      "args":        { "type": "array",  "items": { "type": "string" } },
      "cwd":         { "type": "string", "default": "${workspaceFolder}" },
      "env":         { "type": "object", "additionalProperties": { "type": "string" } },
      "modulePath":  { "type": "string" },
      "stopOnEntry": { "type": "boolean", "default": false },
      "etac":        { "type": "boolean", "default": false },
      "console":     { "type": "string",
                       "enum": ["debugConsole","integratedTerminal","externalTerminal"],
                       "default": "debugConsole" },
      "trace":       { "type": "boolean", "default": false }
    }
  }
}
```

`EtaDebugConfigurationProvider.resolveDebugConfiguration` validates
and resolves `${workspaceFolder}` etc.
`EtaDebugAdapterFactory.createDebugAdapterDescriptor` propagates `env`,
`modulePath`, and `trace` flag to the spawned process.

---

### B2 — Inline values, watch UX, code lenses ✅ **DONE**

**Inline values.** Implemented in
[`editors/vscode/src/inlineValues.ts`](../editors/vscode/src/inlineValues.ts):
`EtaInlineValuesProvider` tokenises the visible viewport (skipping
strings/comments, syntactic heads like `defun`/`lambda`, numeric literals,
quoted symbols and logic vars `?x`), de-duplicates per line, and emits
`InlineValueEvaluatableExpression` for each candidate identifier. VS Code
forwards each to the DAP server's sandboxed `evaluate` (A0).

**Evaluatable expression provider.** Implemented in
[`editors/vscode/src/evaluatableExpression.ts`](../editors/vscode/src/evaluatableExpression.ts):
`EtaEvaluatableExpressionProvider` returns the identifier under the cursor
(preferred) or grows outward to the smallest enclosing s-expression (≤ 256
chars), enabling hover-eval over arbitrary sub-forms like `(+ x y)`.

**Code lens.** Implemented in
[`editors/vscode/src/codeLens.ts`](../editors/vscode/src/codeLens.ts):
`EtaCodeLensProvider` adds **`▶ Run File`** / **`▶ Debug File`** above the
first `(module …)` or `(begin …)` form of every `.eta` file, and
**`▶ Run Tests in File`** / **`▶ Debug Tests in File`** for `*.test.eta`
files (the test action delegates to the Test Explorer via the new
`runTestsForUri` export from `testController.ts`).

**Document links.** Added bonus in
[`editors/vscode/src/documentLink.ts`](../editors/vscode/src/documentLink.ts):
`EtaDocumentLinkProvider` makes every dotted module name inside
`(import …)` clickable, resolved against `ETA_MODULE_PATH`, the document's
own directory, and the workspace `stdlib/` folder.

**Wiring.** All four providers are registered in
[`editors/vscode/src/extension.ts`](../editors/vscode/src/extension.ts)
under the `{ scheme: 'file', language: 'eta' }` selector. New commands
`eta.debugFile` and `eta.runTestFile` are contributed in
[`editors/vscode/package.json`](../editors/vscode/package.json) alongside
the existing `eta.runFile` (which now accepts an optional URI argument
from code-lens callers).

**Document link provider.** `(import std.io)` clauses become clickable
links resolving via `ETA_MODULE_PATH`.

---

### B3 — Heap Inspector v2 ✅ **DONE**

Implemented in [`editors/vscode/src/heapView.ts`](../editors/vscode/src/heapView.ts)
plus the new external bundle under
[`editors/vscode/media/heap/`](../editors/vscode/media/heap/)
(`heap.html`, `heap.css`, `heap.js`):

1. **Empty state, not error.** When `eta/heapSnapshot` returns
   *"VM must be paused"*, the panel now posts an `idle` message that
   renders a friendly "Pause the VM (breakpoint or step) to inspect
   the heap." placeholder instead of a red error banner.
2. **Inline HTML → external bundle.** `heap.html` / `heap.css` /
   `heap.js` are loaded via `webview.asWebviewUri` with
   `localResourceRoots: [media/]`. A strict, nonce-based
   **Content-Security-Policy** is emitted —
   `default-src 'none'; style-src ${cspSource}; script-src 'nonce-…'`
   — and enforced by the meta tag in `heap.html`.
3. **Theme integration.** All colours are sourced from
   `var(--vscode-*)` tokens (editor / panel / focus / badge / progress /
   error / git decorations); no hard-coded palette remains.
4. **Sortable, filterable kind table.** Click any column header to
   toggle sort (Kind / Count / Bytes, plus Δ Count / Δ Bytes in diff
   mode). The toolbar `Filter kinds…` input narrows the table by
   substring match in real time.
5. **Snapshot diff mode.** A `Capture Baseline` toolbar button stores
   the current snapshot; subsequent renders enable a `Diff` toggle that
   adds Δ-count / Δ-bytes columns, highlights *new* kinds (✚) and
   *removed* kinds (✖), and shows a Δ-bytes annotation on the memory
   gauge. `Clear Baseline` resets the comparison.
6. **Find paths to root (BFS).** Inspecting an object now exposes a
   `Find paths to root` button. The extension performs a forward BFS
   from every GC root in the latest snapshot, calling `eta/inspectObject`
   on demand (with an in-session cache), bounded by `MAX_BFS_NODES =
   4000` and stopping after `MAX_PATHS = 5` distinct paths. Results
   render as click-through chains
   `root → #parent → … → #target`, with a "search exhausted" notice if
   the limit was hit.
7. **Memory pressure gauge.** The memory bar (and the Cons Pool bar)
   turn `--vscode-editorWarning-foreground` at ≥70 % and
   `--vscode-errorForeground` at ≥90 %.

> Original B3 wishlist preserved below for traceability.

### B3 — Heap Inspector v2 (original wishlist)

1. **Don't show "VM must be paused" as an error** — open in an empty
   state with a "▶ Pause to inspect" button.
2. **Inline HTML → external bundle** (`media/heapView.{html,css,js}`)
   loaded via `webview.asWebviewUri`. CSP becomes enforceable.
3. **Theme integration** — use `var(--vscode-editor-background)` etc.
4. **Sortable / filterable kind table.**
5. **Snapshot diff** mode.
6. **"Find paths to root"** BFS via `eta/inspectObject`.
7. **Memory pressure gauge** tied to `totalBytes / softLimit`.

---

### B4 — Disassembly view fix-up ✅ **DONE**

1. **`currentPC` is no longer hard-coded** —
   [`dap_server.cpp:2192`](../eta/dap/src/eta/dap/dap_server.cpp) already
   returns `driver_->vm().paused_instruction_index()` (verified against
   [`vm.h:224`](../eta/core/src/eta/runtime/vm/vm.h)). The original audit
   (and the cited line 1119) was stale; the **◀ PC** indicator in
   [`disassemblyTreeView.ts`](../editors/vscode/src/disassemblyTreeView.ts)
   lights up correctly. A unit test
   ([`test/suite/disasm.test.ts`](../editors/vscode/test/suite/disasm.test.ts))
   asserts the parser tags the PC line.
2. **Grouped by function (collapsible).** Rewrote the tree provider to
   parse the disassembly buffer into a two-level tree —
   `=== function ===` nodes hold a *Constant pool* sub-group and the
   instruction stream as leaves. The function block containing the PC
   auto-expands; everything else stays collapsed for fast triage on
   large modules. `showCollapseAll` is now enabled in
   [`extension.ts`](../editors/vscode/src/extension.ts).
3. **Jump-to-callee on Call / TailCall.** Two complementary entry
   points:
    - **Tree view:** every `Call` / `TailCall` leaf carries a
      `command: eta.disassembly.gotoCallee` that opens the
      all-functions disassembly side-by-side and reveals the callee's
      `=== … ===` header. The callee index is recovered by walking
      backward through the function's instruction stream to the most
      recent `LoadConst N  ; <func:M>` annotation.
    - **Editor:** a new
      `EtaDisassemblyDefinitionProvider`
      ([`disassemblyView.ts`](../editors/vscode/src/disassemblyView.ts))
      registered for the `eta-disasm` URI scheme makes
      <kbd>F12</kbd> / <kbd>Ctrl+Click</kbd> on a `Call` line jump to
      the matching `=== … ===` header in the same buffer.
4. **Two-pane source ↔ disassembly on stop.** New setting
   `eta.debug.autoShowDisassembly` (default `false`). When enabled, the
   debug-adapter tracker calls `autoShowDisassemblyOnStop` on every
   `stopped` event — opens the disassembly side-by-side (preserving
   focus on the source), refreshes the buffer, and re-reveals the
   current PC line (`InCenterIfOutsideViewport`). When disabled, just
   the PC reveal still fires for users who manually opened the view.

> Original B4 wishlist preserved below for traceability.

### B4 — Disassembly view fix-up (original wishlist)

1. **Fix `currentPC = -1`** in `dap_server.cpp` line 1119 — surface
   real PC index. The "◀ PC" indicator (`disassemblyTreeView.ts` line
    67) starts working.
2. **Group disassembly by function** (collapsible).
3. **Jump-to-callee** on `Call` / `TailCall` lines via
   `registerDefinitionProvider` for `eta-disasm` scheme.
4. **Two-pane source ↔ disassembly** on stop events.

---

---

### B5 — Test Controller v2 ✅ **DONE**

Implemented in [`editors/vscode/src/testController.ts`](../editors/vscode/src/testController.ts)
(516 lines, up from the audited 284) with all five wishlist items live:

1. **Full TAP-13 YAML diagnostics.** `TapStreamParser` extracts
   `message`, `severity`, `at`, `expected`, and `actual` from every
   `---`/`...` block. `parseTapAtLocation` resolves `file:line[:col]`
   (relative paths against the test file's directory) into a
   `vscode.Location`, which is attached to `vscode.TestMessage.location`
   so the failure pops directly to the offending span. `expected` /
   `actual` are mirrored onto `TestMessage.expectedOutput` /
   `actualOutput`, lighting up the Test Explorer's built-in diff view.
2. **Streaming output.** `TapStreamParser.feed()` is fed each `stdout`
   chunk from `eta_test --format tap`; results are released the instant
   their YAML block closes (or the next `ok` / `not ok` arrives). Child
   `TestItem`s appear and turn green/red live, with raw `eta_test` text
   forwarded to `TestRun.appendOutput` so the UI matches a real
   terminal.
3. **Per-test cancellation.** A `cancelSub = token.onCancellationRequested`
   handler kills the in-flight `eta_test` process; on close, every child
   `TestItem` whose number was *not* yet in `reportedNums` is marked
   `skipped` (instead of failing the whole file), and the parent file
   item is also `skipped` rather than `failed`.
4. **Coverage profile.** `controller.createRunProfile('Coverage',
   TestRunProfileKind.Coverage, …)` invokes `eta_test --coverage`. When
   the runner doesn't recognise the flag (current binary), the profile
   degrades gracefully — detects `invalid argument: --coverage` on
   stderr, marks the file `skipped`, and prints a one-line explanation
   to the run output instead of failing.
5. **Debug profile.** `controller.createRunProfile('Debug',
   TestRunProfileKind.Debug, …)` calls `vscode.debug.startDebugging`
   with `{ type: 'eta', request: 'launch', program: <test file> }`,
   tracks the resulting `DebugSession`, propagates run cancellation to
   `vscode.debug.stopDebugging`, and resolves only when the session
   terminates.

**Bonus — public API for code-lens.** `runTestsForUri(uri)` is exported
so the B2 *▶ Run Tests in File* code-lens can drive the controller
without duplicating spawn/parse logic.

**Tests** ([`editors/vscode/test/suite/`](../editors/vscode/test/suite)):

- [`parseTap.test.ts`](../editors/vscode/test/suite/parseTap.test.ts)
  covers the batch parser surface (passing/failing tests, YAML
  extraction, CRLF, missing-dash descriptions, YAML-key gating).
- [`tapStream.test.ts`](../editors/vscode/test/suite/tapStream.test.ts)
  (new) drives `TapStreamParser` chunk-by-chunk — asserts that a
  result is released the moment the next `ok` arrives, that chunks
  split mid-line and mid-YAML reassemble correctly, that `flush()`
  drains the in-flight test, and that `all()` returns the full
  history. Companion suite for `parseTapAtLocation` covers
  `file:line:col`, `file:line`, relative-path resolution against the
  fallback URI, and unparsable input.

Two pre-existing bugs surfaced by the new streaming tests were fixed
inline:

- `parseTap` previously returned only `flush()`'s result (the in-flight
  test) instead of the full `all()` history, so a 2-test TAP stream
  yielded a single result. Fixed to drain via `flush()` then return
  `stream.all()`.
- `parseTapAtLocation`'s regex `^(.*):(\d+)(?::(\d+))?$` was greedy and
  swallowed the line number into the path component on `file:line:col`
  inputs. Made the path capture non-greedy (`(.*?)`), preserving
  Windows drive-letter handling (`C:\path\foo.eta:12:5`) thanks to the
  trailing `(\d+)` anchor.

> Original B5 wishlist preserved below for traceability.

### B5 — Test Controller v2 (original wishlist)

---

### B6 — LSP integration polish

- **Inlay hints** — `textDocument/inlayHint`.
- **Code actions** — Quick Fix UI for diagnostic codes.
- **Document highlight** — flash all uses of symbol under cursor.
- **Document link** — server-side import resolution.
- **Type hierarchy** — `define-record-type` graph navigation.

---

### B7 — Snippets, grammar, language config refresh ✅ **DONE**

The 217-line `snippets/eta.json` predated several language additions; it
has been refreshed to **42 snippets** in
[`editors/vscode/snippets/eta.json`](../editors/vscode/snippets/eta.json),
adding: `defrel` / `tabled` / `findall` / `run*` / `run1` / `run-n` /
`conde` / `fresh` / `freeze` / `dif`, `clp-domain` / `clp-solve` /
`clp-all-different` / `clpr-maximize` / `clpb-solve`,
`one-for-one` / `one-for-all`, `spawn-thread` / `spawn-thread-with` with
mailbox patterns, `define-record-type` upgraded with predicate +
accessors + setters, `grad` / `tape` (reverse-mode AD), `tensor` /
`backward` (torch), and `test-group`.

**Grammar** ([`syntaxes/eta.tmLanguage.json`](../editors/vscode/syntaxes/eta.tmLanguage.json))
gained dedicated scopes for:

- `?identifier` logic-variable convention → `variable.other.logic.eta`
- `clp:*` / `clpr:*` / `clpb:*` namespace heads → `support.function.clp.eta`
- relational forms (`defrel`, `tabled`, `conde`, `fresh`, `run*`,
  `findall`, `freeze`, `dif`, `==`, `membero`, …) →
  `keyword.control.relational.eta`
- supervisor strategies (`one-for-one`, `one-for-all`,
  `supervisor-poll`) → `keyword.control.supervisor.eta`
- torch / training-loop helpers → `support.function.torch.eta`
- AAD entry points (`grad`, `forward`, `backward`) →
  `support.function.ad.eta`
- test-framework forms (`test`, `test-group`, `assert-*`) →
  `support.function.test.eta`

**Language config** ([`language-configuration.json`](../editors/vscode/language-configuration.json))
adds form-aware `onEnterRules` for `(let ((` / `(letrec ((` /
`(fluid-let` binding lists, `(cond` / `(case` / `(conde` clauses, and
body-introducing heads (`define`, `define-syntax`, `defun`, `defrel`,
`lambda`, `when`, `unless`, `module`, `with-socket`, `spawn-thread*`,
`fresh`, `run*` / `run1` / `run-n`). The existing `wordPattern`
already permits `?` and `!` in identifiers (used by `null?`, `set!`,
`tape-stop!`, etc.), so it is unchanged.

Eta's macro system is hygienic R5RS-style **`(define-syntax NAME
(syntax-rules () …))`** — there is no `defmacro`. A
[`snippets.test.ts`](../editors/vscode/test/suite/snippets.test.ts) unit
test guards the snippets file against ever advertising it.

> Original B7 wishlist preserved below for traceability.

The 217-line `snippets/eta.json` predates several language additions:

- `clpr` / `clpb`, `freeze` / `dif`, `defrel` / `tabled` / `solve` /
  `findall`, `one-for-one` / `one-for-all`, `spawn` / `spawn-thread`
  with mailbox patterns, `define-record-type` with helper methods,
  `tape-with` / `tape-grad`.

**Grammar** scopes for logic vars (`?x`), CLP operators (`#>`, `#=`,
`#\=`), tape operations.

**Language config** — `wordPattern` to include `?` and `!`,
`onEnterRules` for `(let ...)` / `(cond ...)` indentation.

---

## Track C — Testing & Quality

### C0 — Async DAP test harness

The existing `dap_tests.cpp` runs the server **synchronously** in
`run_server` (line 141) — feed all input, then read all output.

```cpp
class AsyncDapHarness {
public:
    AsyncDapHarness();
    ~AsyncDapHarness();
    void send(const std::string& json);
    json::Value wait_response(const std::string& cmd, std::chrono::seconds timeout = 5s);
    json::Value wait_event(const std::string& event, std::chrono::seconds timeout = 5s);
};
```

Server runs on its own thread, communicating via paired stringstreams
+ condition variables.

---

### C1 — VS Code extension test suite ✅ **DONE**

1. **Unit tests** for the TAP parser (`parseTap` exported from
   [`src/testController.ts`](../editors/vscode/src/testController.ts) →
   [`test/suite/parseTap.test.ts`](../editors/vscode/test/suite/parseTap.test.ts)),
   and for the s-expression range walker (`enclosingSexpr` from
   [`src/evaluatableExpression.ts`](../editors/vscode/src/evaluatableExpression.ts) →
   [`test/suite/sexpr.test.ts`](../editors/vscode/test/suite/sexpr.test.ts)).
2. **Integration tests** with `@vscode/test-cli` + `@vscode/test-electron`:
    - Activate extension and assert language registration
      ([`test/suite/extension.test.ts`](../editors/vscode/test/suite/extension.test.ts)).
    - Open `.eta` files (fixture under `test/fixtures/`) and exercise
      `vscode.executeCodeLensProvider` and `vscode.executeLinkProvider`
      ([`codeLens.test.ts`](../editors/vscode/test/suite/codeLens.test.ts),
      [`documentLink.test.ts`](../editors/vscode/test/suite/documentLink.test.ts)).
    - Assert the contributed commands are present and code-lens commands
      resolve to registered handlers.
3. **CI matrix** — [`.github/workflows/vscode.yml`](../.github/workflows/vscode.yml)
   runs the suite on `ubuntu-latest`, `windows-latest`, and `macos-latest`
   (Linux uses `xvfb-run`).

Local invocation: `npm test` in `editors/vscode/` (driven by
[`.vscode-test.mjs`](../editors/vscode/.vscode-test.mjs) and compiled by
[`tsconfig.test.json`](../editors/vscode/tsconfig.test.json)). Initial
suite: **22 passing**.

A full-debug-session integration test (launch fixture, assert `stopped`
event) is intentionally deferred until the `eta_dap` binary is available
on CI runners.

---

### C2 — Performance & regression

- **DAP startup** < 500 ms on `examples/hello.eta`.
- **Heap snapshot** of 100k objects < 100 ms (or report progress).
- **Variables-pane expansion** of large vectors must page.
- **No retained snapshots** after `continue` (valgrind-checked).

---

### C3 — Telemetry (opt-in)

Setting: `eta.telemetry.enabled` (default `false`). Use VS Code's
`TelemetryLogger` API.

Track: adapter starts/exits + exit code, custom `eta/*` request errors,
breakpoint verification failures.

---

## Track D — Distribution & Docs

### D0 — Marketplace polish

1. **Icon** — 128×128 PNG from `docs/img/eta_logo.svg`.
2. **Hero screenshots** in `editors/vscode/media/`.
3. **`displayName`** to `"Eta (Scheme) Language"` .
4. **`badges`** for build-status, version, license.
5. **`extensionPack`** companion bundling Eta + recommended deps.

---

Defer D1 and D2, only docus on D3.

### D1 — Distribution channels

1. **VS Code Marketplace** via `vsce publish`.
2. **OpenVSX** for VSCodium / Theia / Gitpod.
3. **GitHub Release asset** (`eta-vscode-vX.Y.Z.vsix` pinned name).
4. **Auto-update gates** — CI fails if version not bumped.

---

### D2 — CI workflow

New `.github/workflows/vscode.yml` building/testing on
ubuntu/windows/macos and publishing to Marketplace + OpenVSX on tag
push.

---

### D3 — Documentation

1. **`docs/dap.md`** — protocol reference.
2. **`docs/vscode.md`** — extension user guide.
3. **Refresh `editors/vscode/README.md`** — drop the false "no semantic
   tokens" line, add screenshots.
4. **Refresh `docs/next-steps.md`** — collapse DAP/VS Code sections to
   "see [`daps_plan.md`](daps_plan.md)".

---

## Sequencing & Sizing

### Recommended order

1. **A8** (`--trace-protocol`) — 1 day, unblocks every later debug
   session.
2. **B0** (extension cleanup) — 2-3 days, unblocks every later B
   stage.
3. **A1** (`breakpointLocations`) — 1-2 days, prevents flaky tests.
4. **C0** (async harness) — 3 days, prerequisite for testing
   stages A2+.
5. **A0** (sandbox) — 1-2 weeks, unblocks A2/A4 and B2.
6. **A2** (conditional bp + logpoints) and **B1** (launch config) in
   parallel — 1 week each.
7. **A4** (`setVariable`) and **B2** (inline values + watch) in
   parallel — 1 week each.
8. **A3** (function bp), **A5** (restart/cancel), **B3** (Heap
   Inspector v2), **B4** (Disassembly fix-up) in parallel — 2-4 days
   each.
9. **A7** (standard `disassemble`, stepping granularity) — 3-5 days.
10. **A6** (per-thread state) — 2-3 weeks; do last because it
    touches the most code.
11. **B5** (Test Controller v2), **B6** (LSP polish), **B7** (snippets)
    — 2-4 days each, can ship anytime.
12. **C1** (extension tests), **C2** (perf gates), **C3** (telemetry)
    — anytime after the corresponding feature lands.
13. **D3** (distribution + docs) — ship continuously alongside the
    feature work.

### Size table

| Stage | Days | Risk |
|---|---:|:---:|
| A0 Sandbox eval | 5–10 | High |
| A1 BreakpointLocations | 1–2 | Low |
| A2 Conditional/log bp | 5–7 | Med |
| A3 Function bp | 2–3 | Low |
| A4 setVariable | 3–5 | Med |
| A5 Restart/cancel | 2–3 | Low |
| A6 Per-thread state | 10–15 | High |
| A7 Std disassemble + step granularity | 3–5 | Low |
| A8 --trace-protocol | 1 | Low |
| B0 Cleanup | 2–3 | Low |
| B1 Launch config | 3–5 | Low |
| B2 Inline values/code lens | 5–7 | Med |
| B3 Heap Inspector v2 | 5–7 | Low |
| B4 Disassembly fix-up | 2–3 | Low |
| B5 Test Controller v2 | 3–5 | Low |
| B6 LSP polish | 3–5 | Med |
| B7 Snippets/grammar | 2–3 | Low |
| C0 Async harness | 3 | Med |
| C1 Extension tests | 5–7 | Med |
| C2 Perf gates | 3–5 | Low |
| C3 Telemetry | 3–5 | Low |
| D0 Marketplace polish | 2 | Low |
| D1 Distribution channels | 2 | Low |
| D2 CI workflow | 2 | Low |
| D3 Docs | 3–5 | Low |

Total: ~80–120 engineer-days; calendar time depends on parallelism.

---

## Cross-cutting Concerns

### Thread safety

Stage A0's sandbox runs on the **DAP thread** while the VM is parked.
`vm_mutex_` (`dap_server.h` line 91) already serialises this. Add
`assert(vm_->is_paused())` in debug builds at every sandbox entry.

After A6, every helper that touches `driver_->vm()` must take a `VM&`
explicitly.

### Backwards compatibility

- Capability flips are additive — old clients see no behaviour change.
- Custom `eta/*` extensions are unchanged; new ones use the same
  prefix.
- The deferred-initialization handshake (`handle_initialize` line
  214–222 comment) stays — load-bearing for breakpoint timing.
- New `launch.json` properties are all optional with sensible defaults.

### Performance

- Sandbox eval is a single extra null-check in the VM hot loop;
  sandbox only runs when paused.
- Conditional/logpoint evaluation runs on the DAP thread with the VM
  paused. Worst case is "user wrote a slow condition" — trace log
  surfaces it.
- Heap snapshot of 100k objects is currently synchronous and ~100 ms;
  with cancellation (A5) it remains responsive.

### Security

- Sandbox enforces read-only mode against mutating opcodes. Conditions
  that try to call out to shell / network builtins return
  `SandboxViolation`.
- Telemetry (C3) is opt-in and stripped of file paths / source
  contents.
- The `.vsix` is signed via `vsce publish --pat` from CI.

### What we are NOT doing

- **Time travel (`stepBack`)** — would require record/replay.
- **DAP `attach` flow** — Eta is launch-only; future plan.
- **Memory write breakpoints** — niche, no current demand.
- **Compound `setVariable`** (editing inside cons / vector) — defer
  until simple cases solid.
- **Notebook protocol** — that's the Jupyter kernel work in
  [`jupyter_plan.md`](jupyter_plan.md).

---

## Definition of Done

A stage is done when:

1. Every box in the §"Capabilities advertised" table at the top is
   ✅, except explicitly out-of-scope rows.
2. `eta_dap_test` plus the new async harness exercises every stage's
   scenario.
3. `editors/vscode/test/` runs in CI on Linux/Windows/macOS against
   stable + insiders VS Code.
4. `docs/dap.md` and `docs/vscode.md` exist and link from
   `next-steps.md` and the main `README.md`.
5. The `.vsix` is auto-published to Marketplace + OpenVSX on every
   tagged release.
6. Heap Inspector, Disassembly view, Test Explorer, and Inline Values
   all work end-to-end against a fresh `examples/portfolio.eta` debug
   session, with no manual configuration beyond opening the workspace.

---

## Source Locations Referenced

| Component | File |
|---|---|
| DAP server | [`eta/dap/src/eta/dap/dap_server.{h,cpp}`](../eta/dap/src/eta/dap/dap_server.cpp) |
| DAP CLI | [`eta/dap/src/eta/dap/main_dap.cpp`](../eta/dap/src/eta/dap/main_dap.cpp) |
| DAP framing | [`eta/dap/src/eta/dap/dap_io.{h,cpp}`](../eta/dap/src/eta/dap/dap_io.cpp) |
| DAP tests | [`eta/test/src/dap_tests.cpp`](../eta/test/src/dap_tests.cpp) |
| VM debug substrate | [`eta/core/src/eta/runtime/vm/debug_state.h`](../eta/core/src/eta/runtime/vm/debug_state.h) |
| VM façade | [`eta/core/src/eta/runtime/vm/vm.h`](../eta/core/src/eta/runtime/vm/vm.h) |
| Driver | [`eta/interpreter/src/eta/interpreter/driver.h`](../eta/interpreter/src/eta/interpreter/driver.h) |
| LSP server | [`eta/lsp/src/eta/lsp/lsp_server.cpp`](../eta/lsp/src/eta/lsp/lsp_server.cpp) |
| VS Code manifest | [`editors/vscode/package.json`](../editors/vscode/package.json) |
| VS Code activation | [`editors/vscode/src/extension.ts`](../editors/vscode/src/extension.ts) |
| Heap Inspector | [`editors/vscode/src/heapView.ts`](../editors/vscode/src/heapView.ts) |
| GC Roots tree | [`editors/vscode/src/gcRootsTreeView.ts`](../editors/vscode/src/gcRootsTreeView.ts) |
| Disassembly view | [`editors/vscode/src/disassemblyView.ts`](../editors/vscode/src/disassemblyView.ts) |
| Disassembly tree | [`editors/vscode/src/disassemblyTreeView.ts`](../editors/vscode/src/disassemblyTreeView.ts) |
| Child process tree | [`editors/vscode/src/childProcessTreeView.ts`](../editors/vscode/src/childProcessTreeView.ts) |
| Test controller | [`editors/vscode/src/testController.ts`](../editors/vscode/src/testController.ts) |
| Snippets | [`editors/vscode/snippets/eta.json`](../editors/vscode/snippets/eta.json) |


Side note:
Notable findings worth highlighting
1) While auditing the code (not just the docs), I uncovered three concrete bugs that the existing dap_plan.md doesn't mention:
currentPC is hard-coded to -1 in dap_server.cpp line 1119 → the "◀ PC" indicator in the disassembly tree never lights up.
All stop events carry threadId: 1 (line 387) regardless of which actor produced them — actor threads are listed but cannot be paused/inspected independently.
Heap Inspector shows "VM must be paused" as an error instead of an empty state, because the webview always issues eta/heapSnapshot on open and the server returns an error response when the VM is running.
Also corrected one stale claim from dap_plan.md and the README: dap_tests.cpp already exists with 910 lines of coverage, and the LSP already advertises semantic tokens. The new plan is calibrated to this actual baseline, not to outdated assumptions.

