# Project Status and Next Steps

[Back to README](../README.md) · [Architecture](architecture.md) ·
[Bytecode and VM](bytecode-vm.md) · [Compiler](compiler.md) ·
[Runtime and GC](runtime.md) · [Modules and Stdlib](modules.md) ·
[Logic](logic.md) · [CLP](clp.md) · [Release Notes](release-notes.md)

---

## Overview

This page tracks what is delivered in the current Eta baseline, and the
focused work items that are genuinely outstanding.  As of April 2026
the core language, runtime, GC, logic substrate, CLP family
(Z / FD / R / B), networking + actor model, and stdlib are all shipped
and tested.  The remaining roadmap is dominated by **tooling** — the
DAP, the VS Code extension, and a Jupyter kernel — plus a small number
of engine follow-ons.

---

## Delivered Baseline

### Language & VM

- Stable end-to-end pipeline: lexer → parser → expander → module linker
  → semantic analyzer → emitter → VM.
- `etac` AOT bytecode compiler producing `.etac`; `etai` loads `.etac`
  directly with full source-map support.
- Optimisation pipeline (constant folding, DCE) integrated and gated
  behind `-O`.
- Bytecode serializer / disassembler aligned to the same opcode set
  used by the VM.

### Runtime & GC

- Mark-sweep heap with explicit root enumeration (stack, globals,
  frames, trail, propagation queue, attribute store).
- Sharded concurrent heap with hazard-pointer reads.
- Unified backtracking recovery via `trail-mark` / `unwind-trail`
  covering bindings, attributes, CLP domains, `RealStore`, and cached
  simplex bounds.
- Finalizers and guardian queues with documented resurrection semantics
  ([finalizers.md](finalizers.md)).

### Logic & Constraints

- VM-native unification (`logic-var`, `unify`, `copy-term`, `ground?`,
  `deref-lvar`, `term`, `compound?` family) with runtime-configurable
  occurs-check.
- Attributed variables (`put-attr`, `get-attr`, `del-attr`,
  `attr-var?`, `register-attr-hook!`, `register-prop-attr!`).
- `freeze` / `dif` combinators in `std.freeze`.
- CLP(FD) with native Régin-style `all-different`; CLP(B) reified
  Boolean propagators with `clp:sat?` / `clp:taut?`; CLP(R) with
  simplex-backed feasibility, bound tightening, linear optimisation,
  and active-set convex QP.
- miniKanren-style combinators in `std.logic`; SLG-lite tabled
  relations in `std.db`.

### Networking & Actors

- nng-backed messaging over IPC / TCP / inproc.
- Process actors (`spawn`) and in-process thread actors
  (`spawn-thread`, `spawn-thread-with`).
- Lifecycle controls (`spawn-wait`, `spawn-kill`, `thread-join`,
  `thread-alive?`).
- Monitoring primitives (`monitor`, `demonitor`).
- Supervision trees in `std.supervisor` (`one-for-one`, `one-for-all`).

### libtorch & Stats

- Native libtorch bindings: tensors, autograd, `Sequential` / `linear` /
  activations, `sgd` / `adam`, GPU offload, `train-step!` / `forward` /
  `eval!`.
- Eigen-backed multivariate stats over fact tables (`stats:ols-multi`
  with `ColPivHouseholderQR`); `std.stats` descriptive stats, CIs,
  t-tests, OLS over lists.

### Tooling & Tests

- LSP server with parse / analysis-driven diagnostics, completions,
  hover.
- DAP server (`eta_dap`) with breakpoints, step over / in / out,
  call-stack and locals inspection, `evaluate` (incl. hover),
  `completions`, plus custom `eta/heapSnapshot` and
  `eta/inspectObject` protocol extensions.
- VS Code extension (`eta-scheme-lang` v0.3.0) with TextMate grammar,
  snippets, language-configuration, debug views (Heap Inspector,
  Disassembly, GC Roots, Child Processes), and a Test Controller for
  `*.test.eta`.
- C++ unit suite (`eta_core_test`) and stdlib test runner (`eta_test`)
  wired into CTest (`eta_stdlib_tests`) plus the convenience target
  `eta_rebuild_and_test`.
- QP rollout gate: `eta_qp_bench` benchmark plus
  `qp-benchmark.{ps1,sh}` with `--gate` thresholds (see
  [release-notes.md](release-notes.md)).

---

## Focus Areas — Tooling

These are the three deliverables most likely to shift Eta's day-to-day
usability for new users.

### 1) DAP Server — Review & Capability Expansion

The `eta_dap` adapter is functional for the common debug loop, but its
advertised capability set is conservative.  The table below mirrors the
`initialize` response in
[`dap_server.cpp`](../eta/dap/src/eta/dap/dap_server.cpp):

| Capability | State | Notes |
|---|---|---|
| `setBreakpoints` (line) | ✅ | Source-map keyed |
| `configurationDone` | ✅ | Deferred-initialization flow (intentional, see comment in `dap_server.cpp`) |
| `terminate` | ✅ | |
| `evaluateForHovers` | ✅ | |
| `completions` | ✅ | |
| Custom `eta/heapSnapshot` | ✅ | Drives the Heap Inspector webview |
| Custom `eta/inspectObject` | ✅ | Drives drill-down in GC Roots tree |
| `conditionalBreakpoints` | ❌ | **Top of list to add** |
| `functionBreakpoints` | ❌ | Wire to symbol table |
| `setVariable` | ❌ | Useful for live REPL-during-debug |
| `restartRequest` | ❌ | Currently requires kill + relaunch |
| `breakpointLocations` | ❌ | Worth adding for accurate gutter clicks |
| `stepBack` | ❌ | Out of scope without time-travel |

**Concrete work items:**

- **Conditional breakpoints.** Plumb `condition` from `setBreakpoints`
  into the breakpoint table; evaluate the expression in the suspended
  frame's environment using the existing `handle_evaluate` path.
- **Logpoints.** DAP encodes these as breakpoints with `logMessage`;
  same plumbing as conditional, plus an `Output` event per hit.
- **Function breakpoints.** Use the emitter's per-function entry PC
  plus symbol-name lookup; surface VS Code's *Function Breakpoints* UI.
- **`setVariable`.** Restricted to local frame slots and globals;
  reuses the trail so it works correctly under backtracking.
- **`breakpointLocations`.** Emit the set of valid line numbers per
  source map so VS Code stops shifting clicks to the next valid line.
- **Restart.** Re-launch the VM from the same `launch` arguments
  without a full debug-adapter teardown.
- **Per-thread state for `spawn-thread` actors.** `threads` currently
  reports the main VM thread only; expose in-process thread actors as
  separate DAP threads so each can be paused / inspected
  independently.
- **Test coverage.** Add a small DAP integration harness driving
  `eta_dap` over stdin / stdout with golden JSON exchanges — there is
  currently no automated coverage of the protocol surface.
- **Diagnostics.** A `--trace-protocol` flag dumping every request /
  response would eliminate a class of "did the message arrive?" bugs
  during VS Code extension work.

A `docs/dap.md` page documenting both the standard surface and the
custom `eta/*` extensions should land alongside this work.

### 2) VS Code Extension — Tightening & Polish

Current shipping surface (`editors/vscode/src/`):

- `extension.ts` — entry point, LSP / DAP wiring
- `heapView.ts` — Heap Inspector webview
- `disassemblyView.ts` + `disassemblyTreeView.ts` — bytecode + tree
- `gcRootsTreeView.ts` — GC roots drill-down
- `childProcessTreeView.ts` — spawned actors
- `testController.ts` — VS Code Test Explorer integration for
  `*.test.eta`
- `dapTypes.ts` — DAP message typings

**Concrete work items:**

- **Inline values.** Implement `InlineValuesProvider` so the editor
  decorates locals with their current value during a stop event (uses
  the same `evaluate` path the hover does).
- **Watch expressions.** Once `setVariable` lands on the DAP side, the
  Watch view becomes round-trip-editable.
- **Conditional / log breakpoints UI.** Pure capability flip — once
  the DAP side advertises the relevant `supports*` flags, the
  extension needs no additional wiring.
- **Debug Console / REPL improvements.**
  - Multi-line input via `Shift+Enter` with persistent history.
  - Auto-import of `std.io` so `(println …)` works out of the box.
- **Heap Inspector polish.** Sortable columns, search / filter, and a
  *snapshot diff* mode (compare two snapshots to find leaks). Currently
  the view is read-only and refresh-only.
- **Disassembly view.** Add (a) follow-symbol / jump-to-callee on
  `Call` / `TailCall`, (b) two-pane view with source on the left and
  bytecode on the right for the current PC.
- **Test Controller.** Surface per-assertion failure locations using
  `*.test.eta` source maps — failures currently show only the test
  name.
- **Snippets refresh.** `snippets/eta.json` predates `clpr`, `clpb`,
  `freeze`, `dif`, `defrel`, supervision — add canonical templates.
- **README + screenshots.** `editors/vscode/README.md` is the
  Marketplace-facing page; align it with the main README's shipped
  feature list.
- **CI publish.** Add a workflow that builds the `.vsix` on tag push
  so the bundle layout's `editors/eta-lang-<version>.vsix` is
  reproducible rather than hand-built.

### 3) Jupyter Kernel via [xeus](https://github.com/jupyter-xeus/xeus)

Goal: a `xeus-eta` kernel so notebooks become a first-class Eta
front-end alongside `etai` and `eta_repl`.

**Why xeus over a Python wrapper:**

- xeus is a C++ implementation of the Jupyter messaging protocol —
  Eta already lives in C++, so the kernel can embed `Driver` directly
  without an FFI hop.
- Async I/O, comm targets, and rich `display_data` are first-class.
- Existing precedents (`xeus-cling`, `xeus-python`, `xeus-lua`,
  `xeus-cpp`) provide a working blueprint.

**Implementation outline:**

| Phase | Deliverable |
|---|---|
| 1 | New executable `eta_jupyter` (`eta/jupyter/`) linking `eta_core` + `xeus` + `xeus-zmq`. CMake `find_package(xeus)` / `find_package(xeus-zmq)` (or a `cmake/FetchXeus.cmake` mirroring the existing `FetchNng.cmake`). |
| 2 | `EtaInterpreter : public xeus::xinterpreter` overriding `execute_request_impl`, `complete_request_impl`, `inspect_request_impl`, `is_complete_request_impl`. Thin wrapper around the existing `Driver` REPL surface used by `eta_repl`. |
| 3 | Kernel-spec installation: `eta_jupyter --install` writes `kernels/eta/kernel.json` to the Jupyter data dir. |
| 4 | Rich display: detect when a result is a `FactTable` / tensor / DAG and emit `application/vnd.eta.facttable+json` plus an HTML fallback. |
| 5 | Comm channels for Heap-Inspector / Disassembly equivalents in JupyterLab — reuse the DAP webview HTML where possible. |
| 6 | Conda-Forge / PyPI packaging so `mamba install xeus-eta` and `jupyter labextension install` give a one-line setup. |

**Open design questions:**

- One kernel per notebook process vs. shared `Driver` across cells —
  shared is simpler and matches `eta_repl`, but loses crash isolation.
- Auto-load `(import std.io)` in cell 0?  `etai` does, `eta_repl`
  does; notebooks probably should too.
- How to expose `spawn` / actor processes from a notebook cell —
  likely via the existing inproc transport, with a
  `(jupyter:show pid)` widget surfacing the Child Processes tree
  contents.

A `docs/jupyter.md` design page should land alongside the first
prototype.

---

## Engine Follow-ons (Lower Priority)

### Performance

- Repeatable benchmark harness in CI (the QP gate is a good template
  for a general benchmark format).
- Optimiser passes beyond constant folding + DCE: function inlining,
  beta-reduction of trivial closures, interprocedural constant
  propagation.
- VM dispatch: super-instructions for hot opcode pairs and inline
  caches for global lookups.

### Runtime & GC

- Generational / incremental GC to reduce stop-the-world pauses on the
  larger heaps produced by libtorch + fact-table workloads.
- Adaptive soft-limit triggering and long-running fragmentation
  measurement.

### Logic / CLP

- Native VM-level tabling engine (current `tabled` workflow is
  stdlib-level in `std.db` with conservative cache flushing).
- Optional WAM-style logic bytecode path for specialised logic
  workloads.
- CLP(R) strict inequalities without epsilon shifts.

### Distributed Actor Runtime

- Built-in distributed supervision and name-registry semantics at
  runtime level (beyond current library helpers in `std.supervisor`).
- Cluster membership / heartbeat protocol on top of nng SURVEYOR.

### Documentation

- `docs/jupyter.md` (alongside the kernel prototype).
- `docs/regex.md` - `std.regex` reference and performance notes.
- `docs/dap.md` — currently the DAP surface is documented only in code
  comments; a protocol-extension reference would help editor authors.
- `docs/clpr.md` — split out from `clp.md` once that page exceeds a
  screen.

---

## Recently Completed (was on this list, now shipped)

- ✅ CLP(R) full Stage 6 rollout (linear + convex QP) — see
  [release-notes.md](release-notes.md).
- ✅ CLP(B) Boolean propagation (`std.clpb`).
- ✅ Attributed-variable combinators (`std.freeze`).
- ✅ Relations + SLG-lite tabling (`std.db`).
- ✅ Erlang-style supervision trees (`std.supervisor`).
- ✅ miniKanren combinators in `std.logic`.
- ✅ Finalizers & guardians.
- ✅ Causal portfolio decision engine showcase
  ([portfolio.md](portfolio.md)).
- Native CSV subsystem (`std.csv`) and fact-table CSV bridge
  (`fact-table-load-csv`, `fact-table-save-csv`) - see [csv.md](csv.md).

