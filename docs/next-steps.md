# Project Status and Next Steps

[Back to README](../README.md) · [Architecture](architecture.md) ·
[Bytecode and VM](bytecode-vm.md) · [Compiler](compiler.md) ·
[Runtime and GC](runtime.md) · [Modules and Stdlib](modules.md) ·
[Logic](logic.md) · [CLP](clp.md) · [Jupyter](jupyter.md) ·
[Release Notes](release-notes.md)

---

## Overview

This page tracks what is delivered in the current Eta baseline, and the
focused work items that are genuinely outstanding. As of April 2026
the core language, runtime, GC, logic substrate, CLP family
(Z / FD / R / B), networking + actor model, libtorch bindings, stdlib,
LSP, DAP, VS Code extension, and Jupyter kernel are all shipped and
tested. The remaining roadmap is dominated by **packaging / polish**
(conda-forge for the Jupyter kernel, Binder, CI publish for the VS Code
extension) plus a small number of engine follow-ons.

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

### Tooling, Notebooks & Tests

- LSP server with parse / analysis-driven diagnostics, completions,
  hover.
- DAP server (`eta_dap`) with breakpoints (line, conditional,
  hit-count, logpoints, function), step over / in / out (incl.
  instruction granularity), call-stack and locals inspection,
  `evaluate` (incl. hover), `completions`, `setVariable`, `restart`,
  `cancel`, `breakpointLocations`, `terminateThreads`, plus custom
  `eta/heapSnapshot` and `eta/inspectObject` extensions.
- VS Code extension (`eta-scheme-lang`) with TextMate grammar,
  snippets, language-configuration, debug views (Heap Inspector,
  Disassembly, GC Roots, Child Processes), and a Test Controller for
  `*.test.eta`.
- Jupyter kernel (`eta_jupyter`) — xeus-based, embeds the `Driver`
  directly; `--install` writes the kernelspec; rich-display MIME
  bundles for tensors, fact tables, heap snapshots; three showcase
  notebooks under `examples/notebooks/`. See [jupyter.md](jupyter.md).
- C++ unit suite (`eta_core_test`) and stdlib test runner (`eta_test`)
  wired into CTest (`eta_stdlib_tests`) plus the convenience target
  `eta_rebuild_and_test`.
- Async DAP harness (`eta/test/src/dap_tests.cpp`) covering the
  paused-session round trip.
- QP rollout gate: `eta_qp_bench` benchmark plus
  `qp-benchmark.{ps1,sh}` with `--gate` thresholds (see
  [release-notes.md](release-notes.md)).

---

## Outstanding Work

The list is short and mostly about distribution.

### 1) Jupyter — Packaging & JupyterLab Front-End

The kernel itself is shipped and the three showcase notebooks run
end-to-end. What's left:

- **Conda-forge package.** `recipes/xeus-eta/meta.yaml` is drafted but
  not yet submitted upstream. Until that lands, Binder cannot
  `mamba install xeus-eta`; the workaround is the Dockerfile path
  documented in [`docs/eta_plan.md` §2](eta_plan.md).
- **Binder enablement.** Once either the conda-forge package or the
  in-tree `binder/` Dockerfile is verified end-to-end, add three
  Binder badges (Basics, AAD, Portfolio) to the top of the README.
  Placeholder is already reserved in `README.md`.
- **JupyterLab labextension.** Comm channels for Heap-Inspector and
  Disassembly equivalents in JupyterLab — reuse the existing DAP
  webview HTML where possible.
- **Per-notebook crash isolation.** Currently one shared `Driver` per
  kernel process (matches `eta_repl` semantics). Decide whether to
  offer a per-cell sandbox mode for long-running notebooks that load
  GPU tensors.

### 2) DAP / VS Code — Polish

The DAP advertised capability set is now broad; the remaining items
are UX polish:

- **Per-thread state for `spawn-thread` actors.** `threads` lists
  actor threads, but stop / stack / evaluate is still routed through
  the main VM. Finish full per-thread pause / inspect / step routing.
- **Inline values.** Implement `InlineValuesProvider` so the editor
  decorates locals with their current value during a stop event.
- **Watch expressions.** Richer expression evaluation for `setVariable`
  edit RHS.
- **Debug Console / REPL improvements.**
  - Multi-line input via `Shift+Enter` with persistent history.
  - Auto-import of `std.io` so `(println …)` works out of the box.
- **Heap Inspector polish.** Sortable columns, search / filter, and a
  *snapshot diff* mode (compare two snapshots to find leaks).
- **Disassembly view.** Follow-symbol / jump-to-callee on `Call` /
  `TailCall`, and a two-pane source ↔ bytecode view for the current PC.
- **Test Controller.** Surface per-assertion failure locations using
  `*.test.eta` source maps — failures currently show only the test
  name.
- **Snippets refresh.** `snippets/eta.json` predates `clpr`, `clpb`,
  `freeze`, `dif`, `defrel`, supervision, `std.jupyter` — add canonical
  templates.
- **CI publish.** Add a workflow that builds the `.vsix` on tag push so
  the bundle layout's `editors/eta-lang-<version>.vsix` is reproducible
  rather than hand-built.

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

- `docs/index.md` — the umbrella index referenced from the trimmed
  README documentation table; needs to gather every existing page into
  a single landing.
- `docs/clpr.md` — split out from `clp.md` once that page exceeds a
  screen.
- More notebook-led tutorials under `examples/notebooks/` (xVA, SABR,
  causal-factor primer).

---

## Recently Completed (was on this list, now shipped)

- ✅ Jupyter kernel (`eta_jupyter`) via xeus, with three showcase
  notebooks and rich-display MIME bundles — see
  [jupyter.md](jupyter.md) and the
  [release notes](release-notes.md#2026-04-26).
- ✅ DAP capability expansion: conditional / function breakpoints,
  logpoints, `setVariable`, `restart`, `cancel`, `breakpointLocations`,
  `terminateThreads`, instruction-granularity stepping, async test
  harness.
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
- ✅ Native CSV subsystem (`std.csv`) and fact-table CSV bridge
  (`fact-table-load-csv`, `fact-table-save-csv`) — see
  [csv.md](csv.md).
- ✅ REPL redefinition shadowing for new submissions, documented in
  [repl.md](repl.md).
- ✅ DAP and VS Code reference docs: [dap.md](dap.md),
  [vscode.md](vscode.md).
- ✅ Regex reference: [regex.md](regex.md).

