# Project Status and Next Steps

[Back to README](../README.md) · [Architecture](architecture.md) ·
[Bytecode and VM](bytecode-vm.md) · [Compiler](compiler.md) ·
[Runtime and GC](runtime.md) · [Modules and Stdlib](modules.md) ·
[Logic](logic.md) · [CLP](clp.md) · [Jupyter](jupyter.md) ·
[Optimisations](optimisations.md) · [Release Notes](release-notes.md)

---

## Overview

This page tracks what is delivered in the current Eta baseline, and the
focused work items that are genuinely outstanding. As of **April 2026**
the core language, runtime, GC, logic substrate, CLP family
(Z / FD / R / B), networking + actor model, libtorch bindings, stdlib,
LSP, DAP, VS Code extension, and Jupyter kernel are all shipped and
tested. The optimisation pipeline (`etac -O`) now ships
`ConstantFolding`, `PrimitiveSpecialisation`, and `DeadCodeElimination`,
plus a lock-free function-registry read path and self-recursive
tail-call → backward-jump emission (see
[optimisations.md](optimisations.md)).

The remaining roadmap splits into three buckets:

1. **Distribution / packaging polish** — conda-forge, Binder badges,
   CI publish for the `.vsix`.
2. **Engine follow-ons** — further optimiser passes, generational GC,
   per-thread DAP routing.
3. **Hosted-platform layer** *(new phase, see end of doc)* — the
   functionality a script needs from its host OS. Hash maps, sets,
   `getenv`, argv, filesystem, subprocess, and JSON are now shipped
   (`std.hashmap`, `std.hashset`, `std.os`, `std.fs`, `std.process`,
   `std.json`); the remaining gaps are atoms, HTTP, FFI, and a
   condition system. This is the largest *capability* gap between Eta
   and a comparable hosted language (Clojure, Racket, Common Lisp,
   Chez).

---

## Delivered Baseline

### Language & VM

- Stable end-to-end pipeline: lexer → parser → expander → module linker
  → semantic analyzer → optimisation pipeline → emitter → VM.
- `etac` AOT bytecode compiler producing `.etac`; `etai` loads `.etac`
  directly with full source-map support.
- Optimisation pipeline (`etac -O`):
  - `ConstantFolding` — binary `+ - * /` over numeric literals (callee
    name-resolved; hardening planned).
  - `PrimitiveSpecialisation` — proven-builtin call sites lower to
    dedicated VM opcodes (`Add`, `Sub`, `Mul`, `Div`, `Eq`, `Cons`,
    `Car`, `Cdr`).
  - `DeadCodeElimination` — pure non-tail `Begin` operands collapsed.
- VM: self-recursive tail calls in unary form lower to a backward
  `Jump` (no `TailCall` overhead). Function registry uses a lock-free
  read path; primitive dispatch forwards arguments as
  `std::span<const LispVal>` over the VM stack (zero-copy).
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
- Exception machinery: `raise` / `catch` are first-class special forms
  in the reader and semantic analyzer.

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

### I/O — what already exists

- Text + binary file ports: `open-input-file`, `open-output-file`,
  `open-input-bytevector`, `open-output-bytevector`,
  `get-output-bytevector`, `read-u8`, `write-u8`, `binary-port?`,
  `port?`, `input-port?`, `output-port?`, `close-port` family.
- String ports: `open-input-string`, `open-output-string`,
  `get-output-string`.
- Current-port redirection: `current-input-port`,
  `current-output-port`, `current-error-port`, with
  `set-current-{input,output,error}-port!` and the `with-*-port`
  wrappers in `std.io`.
- Native CSV codec (`std.csv`) and `regex` bindings (`std.regex`).
- Time primitives (`std.time`) — wall, monotonic, sleep, ISO-8601
  formatting, calendar parts.

### Tooling, Notebooks & Tests

- LSP server with parse / analysis-driven diagnostics, completions,
  hover.
- DAP server (`eta_dap`) with breakpoints (line, conditional,
  hit-count, logpoints, function), step over / in / out (incl.
  instruction granularity), call-stack and locals inspection,
  `evaluate` (incl. hover), `completions`, `setVariable`, `restart`,
  `cancel`, `breakpointLocations`, `terminateThreads`, plus custom
  `eta/heapSnapshot`, `eta/inspectObject`, `eta/disassemble`,
  `eta/localMemory`, `eta/childProcesses` extensions.
- VS Code extension (`eta-scheme-lang`) with TextMate grammar,
  snippets, language-configuration, debug views (Heap Inspector,
  Disassembly, GC Roots, Child Processes), inline-values provider
  (off by default), evaluatable-expression hover, code-lens, document
  links, jump-to-callee on `Call` / `TailCall` instructions, and a
  Test Controller for `*.test.eta`.
- Jupyter kernel (`eta_jupyter`) — xeus-based, embeds the `Driver`
  directly; `--install` writes the kernelspec; rich-display MIME
  bundles for tensors, fact tables, heap snapshots; three showcase
  notebooks under `cookbook/notebooks/`. See [jupyter.md](jupyter.md).
- C++ unit suite (`eta_core_test`) and stdlib test runner (`eta_test`
  wired into CTest (`eta_stdlib_tests`) plus the convenience target
  `eta_rebuild_and_test`.
- Async DAP harness (`eta/qa/test/src/dap_tests.cpp`) covering the
  paused-session round trip.
- QP rollout gate: `eta_qp_bench` benchmark plus
  `qp-benchmark.{ps1,sh}` with `--gate` thresholds (see
  [release-notes.md](release-notes.md)).

---

## Outstanding Work — Distribution & Polish

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

The DAP advertised capability set is now broad and most of last
quarter's polish list shipped (inline-values provider,
evaluatable-expression hover, code-lens, document links,
jump-to-callee, child-processes view). Remaining items are narrower:

- **Per-thread state for `spawn-thread` actors.** `threads` lists
  actor threads, but stop / stack / evaluate is still routed through
  the main VM. Finish full per-thread pause / inspect / step routing.
- **Watch expressions.** Richer expression evaluation for `setVariable`
  edit RHS.
- **Debug Console / REPL improvements.**
  - Multi-line input via `Shift+Enter` with persistent history.
  - Auto-import of `std.io` so `(println …)` works out of the box.
- **Heap Inspector polish.** Sortable columns, search / filter, and a
  *snapshot diff* mode (compare two snapshots to find leaks).
- **Test Controller.** Surface per-assertion failure locations using
  `*.test.eta` source maps — failures currently show only the test
  name.
- **Snippets refresh.** `snippets/eta.json` predates `clpr`, `clpb`,
  `freeze`, `dif`, `defrel`, supervision, `std.jupyter`, and the
  hosted-platform modules listed below — add canonical templates as
  each ships.
- **CI publish.** Add a workflow that builds the `.vsix` on tag push so
  the bundle layout's `editors/eta-lang-<version>.vsix` is reproducible
  rather than hand-built. Bundle-script DLL sanity check now matches
  both `xeus.dll` and the FetchContent-prefixed `libxeus.dll` family
  (fixed April 2026; see release notes).

---

## Engine Follow-ons (Lower Priority)

### Performance — track [optimisations.md](optimisations.md)

The optimisation plan is now a separate, prioritised document. The
short version:

- **Done.** Constant folding (binary), DCE, primitive specialisation,
  self-recursive tail-call → backward jump (unary), lock-free function
  registry, zero-copy primitive arg dispatch.
- **Next** (in rollout order): harden constant folding under builtin
  shadowing → n-ary folding + algebraic identities + constant/copy
  propagation through `let` → bytecode peephole → dead-store /
  frame-shrink → AAD flonum fast path → closure elimination for
  non-escaping lambdas → constant-pool / quote interning →
  flow-sensitive type specialisation.
- **Benchmarking.** Repeatable harness in CI (the QP gate is a good
  template for a general benchmark format covering AAD primal, CLP(R)
  feasibility, Monte Carlo, and stdlib hot paths).

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
- More notebook-led tutorials under `cookbook/notebooks/` (xVA, SABR,
  causal primer based on `cookbook/causal/causal_demo.eta`).

---

## Next Phase — Hosted-Platform Layer (proposal)

Eta today is a strong **language and runtime**: VM, GC, logic, CLP,
AAD, libtorch, actors, Jupyter. What it is **not yet** is a strong
**hosted platform** — the things a script needs from its operating
system to replace Python or Ruby for everyday automation. This
section enumerates the gap by comparing against Clojure (the closest
relative on the JVM side), R7RS-large, and Common Lisp, and proposes
a delivery order.

> **Why this is the next phase.** Every other runway item
> (`generational GC`, `WAM bytecode`, `distributed supervision`) is
> incremental — it makes existing workloads faster or larger. The
> hosted-platform gap is *categorical*: until it lands you cannot
> write a deploy script, a config-file processor, an HTTP webhook, or
> a Jupyter notebook that ingests live data without dropping into
> another language. Closing the gap unlocks an order of magnitude
> more use-cases per delivered LoC than any of the engine follow-ons.

### Capability matrix — what's missing vs Clojure

| Capability | Clojure | Eta today | Gap |
|---|---|---|---|
| **Hash map / set** | `{:a 1}`, `#{1 2}` (persistent) | native `hash-map` / `hash-set` + `std.hashmap` / `std.hashset` | Medium — reader literals / HAMT / transients deferred |
| **Atom / ref / agent** (CAS cells) | `atom`, `ref`, `agent` | none | Big — actors fill some of this, not all |
| **getenv / setenv** | `(System/getenv ...)` | `os:getenv` / `os:setenv!` / `os:unsetenv!` / `os:environment-variables` (`std.os`) | Closed |
| **argv / command-line** | `*command-line-args*` | `os:command-line-arguments` (`std.os`) | Closed |
| **Filesystem ops** | `clojure.java.io` | full `std.fs` (`fs:file-exists?`, `fs:directory?`, `fs:delete-file`, `fs:make-directory`, `fs:list-directory`, `fs:path-join`, `fs:path-split`, `fs:path-normalize`, `fs:temp-file`, `fs:temp-directory`, `fs:file-modification-time`, `fs:file-size`) | Closed |
| **Subprocess / `exec`** | `clojure.java.shell/sh` | `std.process` (`process:run`, `process:spawn`, lifecycle controls, stdio ports) | Closed |
| **JSON parser / serialiser** | `clojure.data.json`, `cheshire` | `json:read` / `json:read-string` / `json:write` / `json:write-string` (`std.json`, RFC 8259, hash-map / vector decode, optional integer-exact mode) | Closed |
| **`format` / `printf`** | `(format "%.3f" x)` | string ports + `display` | Medium — verbose for numeric reports |
| **HTTP client** | `clj-http`, `hato` | nng raw sockets only | Medium |
| **HTTP server** | `ring` | nng REQ/REP only | Medium |
| **FFI / dlopen** | JNI / native-image | none | Medium — torch/nng are hard-linked |
| **Condition / restart system** | `ex-info` / `ex-data` | `raise`/`catch` | Medium — works, but no restart frames |
| **Structured logging** | `tools.logging`, `mulog` | `std.log` (levels, per-logger filters, stdout/stderr/file/rotating/daily/port sinks, pattern + custom formatters) | Closed |
| **Persistent vector / list / map** | core | mutable vectors, immutable lists, no maps | Medium — concurrency-relevant |
| **Protocols / multimethods** | `defprotocol`, `defmulti` | none | Smaller — generic dispatch via cond/case today |
| **Transducers** | core | none | Smaller — `map`/`filter`/`foldl` cover most cases |
| **call/cc** | none (JVM limit) | not yet | Smaller — niche; aligns with R7RS |
| **delay / force / streams** | `delay`/`force`, `lazy-seq` | none | Smaller |
| **Destructuring `let`** | `(let [{:keys [a b]} m] ...)` | none | Smaller — quality-of-life |

### Delivery order (proposal)

Each layer is independently shippable, independently benchmarkable,
and unlocks a distinct class of script. Recommended order:

#### Phase H1 — Process & Filesystem (highest ROI)

The thing that is missing 100% of the time when someone tries to use
Eta as a Python replacement.

- **Builtins** (in `os_primitives.h`) — **shipped April 2026**:
  - `getenv`, `setenv!`, `unsetenv!`, `environment-variables`
  - `command-line-arguments` (collected by `etai`/`etac`, surfaced to
    the program)
  - `exit`, `current-directory`, `change-directory!`
  - `file-exists?`, `directory?`, `delete-file`, `make-directory`,
    `list-directory`, `path-join`, `path-split`, `path-normalize`,
    `temp-file`, `temp-directory`
  - `file-modification-time`, `file-size`
- **Stdlib** — `std.os` and `std.fs` shipped (see
  [`os.md`](guide/reference/os.md), [`fs.md`](guide/reference/fs.md)).
- **Subprocess** — shipped in `std.process`:
  - blocking `process:run` / `process:run-string`
  - non-blocking `process:spawn` with lifecycle controls
    (`process:wait`, `process:kill`, `process:terminate`,
    `process:pid`, `process:alive?`, `process:exit-code`)
  - child stdio as Eta ports
    (`process:stdin-port`, `process:stdout-port`, `process:stderr-port`)
  - timeout and operational error prefixes
    (`process-timeout`, `process-spawn-failed`, `process-not-found`,
    `process-wait-failed`)

#### Phase H2 — Hash Map / Set + Atom (concurrency primitive)

Hash map and hash set are now shipped. The remaining H2 gap is Atom.

- **Hash map** shipped as a first-class immutable value:
  `make-hash-map`, `hash-map?`, `hash-map-ref`, `hash-map-assoc`,
  `hash-map-dissoc`, `hash-map-keys`, `hash-map-values`,
  `hash-map-size`, `hash-map->list`, `list->hash-map`,
  `hash-map-fold`, and `hash`.
- **Hash set** shipped on the same backing model:
  `make-hash-set`, `hash-set?`, `hash-set-add`, `hash-set-remove`,
  `hash-set-contains?`, `hash-set-union`, `hash-set-intersect`,
  `hash-set-diff`.
- **Atom** — single-cell mutable reference with CAS:
  - `(atom v)`, `atom?`, `(deref a)`, `(reset! a v)`,
    `(swap! a fn args …)`, `(compare-and-set! a old new)`.
  - Backed by `std::atomic<LispVal>` (boxed) with the existing GC
    barrier. No watcher chain in v1; add later if needed.
- **Stdlib** now includes `std.hashmap` and `std.hashset`; `std.atom`
  remains outstanding with the Atom runtime work.

> **Why hash map before HTTP / JSON.** JSON parsing returns a hash
> map. HTTP libraries return responses as hash maps. There is no
> point shipping JSON / HTTP without the data type they naturally
> produce.

#### Phase H3 — JSON, Format, Logging

The "make it usable for real work" layer, on top of H1+H2.

- **`std.json`** — **shipped April 2026.** `json:read` (port → value),
  `json:read-string`, `json:write`, `json:write-string`. RFC 8259
  via a hand-written in-tree codec (`eta/core/src/eta/util/json.h`,
  no third-party dependency); decodes objects to hash maps, arrays
  to vectors, numbers to flonums. Pass `'keep-integers-exact? #t` to
  preserve integer-typed JSON numbers as fixnums. Auto-imported by
  `std.prelude`. See [`json.md`](guide/reference/json.md).
- **`std.format`** — `format` à la SLIB / Common Lisp tiny subset:
  `~a` (display), `~s` (write), `~d` (decimal), `~f` (float with
  precision), `~e` (scientific), `~%` (newline), `~~`. Useful for
  reports and log messages. ~200 LoC pure Eta on top of string ports.
- **`std.log`** — **shipped April 2026.** Structured logger with
  levels (`trace`/`debug`/`info`/`warn`/`error`/`critical`), per-logger
  level filtering, multiple sink kinds (stdout, stderr, file,
  size-based rotating, daily, arbitrary port), configurable pattern
  via `log:set-pattern!`, custom formatter callbacks via
  `log:set-formatter!`, `log:flush!` / `log:flush-on!` controls, and
  `log:shutdown!` for clean teardown. The default logger writes to the
  active error port so `eta_jupyter` surfaces logs as cell output
  without further wiring. Tests in `stdlib/tests/log.test.eta`;
  reference at [`log.md`](guide/reference/log.md).

#### Phase H4 — HTTP & FFI

- **`std.http`** — minimal HTTP client (boost::beast or curl) with
  blocking `(http:get url [headers])` / `(http:post url body
  [headers])` returning a hash map `{(status . n) (headers . hm)
  (body . bytevector)}`. A blocking client is enough to unblock
  webhook ingest, REST-API consumption, and notebook-time data
  pulls; a server can come later via the same library.
- **`std.ffi`** — `dlopen` / `dlsym` thin wrapper. Two layers:
  - low-level `(ffi:open "libfoo.so")`,
    `(ffi:symbol lib "fn_name" '(int double *char) 'int)` returning a
    callable;
  - high-level `define-foreign` macro that desugars to the above.
  Risk: GC-safety of foreign callbacks. Scope v1 to *outbound* calls
  only — Eta calling C — and document that callbacks coming back into
  Eta are out of scope.

#### Phase H5 — Condition / restart system + protocols + transducers

The "feels like a mature Lisp" finishing layer. Each is independently
useful but each is also pure quality-of-life on top of H1–H4.

- **Condition / restart system** (Common Lisp `cerror` / `restart-case`
  family) — replaces today's `raise`/`catch` with a richer model that
  separates *signalling* a condition from *handling* it from
  *recovering* via a named restart. Useful in actor supervisors and
  REPL workflows.
- **`defprotocol` / `defmethod`** — generic dispatch on the type of
  the first argument (single dispatch in v1). Backed by a small
  vtable indexed off the heap-object kind. Used internally by
  `display`, `write`, `=`, etc. could be exposed.
- **Transducers** — `(map f)`, `(filter p)`, `(take n)` returning
  reducing-function transformers; `transduce` driver. ~200 LoC pure
  Eta; useful when combined with channels or large fact-table scans.
- **Destructuring `let`** — alist / vector / hash-map binding form
  desugared by the expander.

### Effort / risk table

| Phase | Effort (eng-weeks) | Risk | Unblocks |
|---|---|---|---|
| H1 Process & FS | 2 | Low | scripts, deploy, test runners that shell out |
| H1 shipped: `std.os` + `std.fs` + `std.process` (env, argv, cwd, exit, paths, temp, stat, subprocess run/spawn/lifecycle). |
| H2 Hashmap / Set / Atom | 3 | Medium (GC barrier on Atom; HAMT later) | every dict-shaped workload |
| H3 JSON / Format / Log | 2 | Low | configs, observability, REST consumers |
| H3 already shipped: `std.json` (RFC 8259, hash-map / vector decode, integer-exact mode) and `std.log` (levels, filters, sinks, patterns, custom formatters). `std.format` remains. |
| H4 HTTP / FFI | 3 | Medium (HTTP TLS; FFI safety) | webhook receivers, telemetry, third-party C libs |
| H5 Conditions / Protocols / Transducers | 3 | Medium (restart frames touch the VM stack) | mature Lisp surface |
| **Total** | **~13 weeks** | | |

### Non-goals (deferred deliberately)

- **`call/cc`** — not on the path; full first-class continuations
  interact poorly with the existing trail / unwind discipline.
  Consider only if a concrete user lands.
- **Lazy sequences / streams** — partially covered by tabled relations
  in `std.db`; full SRFI-41 streams not justified.
- **JIT** — not on the runway; `etac -O` plus PrimitiveSpecialisation
  buys most of what a baseline JIT would.

---

## Recently Completed (was on this list, now shipped)

- ✅ Hosted-platform Phase H3 (slice 2) — `std.log`: structured logger
  with `trace`/`debug`/`info`/`warn`/`error`/`critical` levels,
  per-logger level filters, multiple sink kinds (stdout, stderr, file,
  size-based rotating, daily, port), configurable pattern + custom
  formatter callbacks, `log:flush!` / `log:flush-on!` controls, and
  `log:shutdown!` for clean teardown. Backed by `%log-*` runtime
  primitives. Tests in `stdlib/tests/log.test.eta`; reference at
  [`log.md`](guide/reference/log.md).
- ✅ Hosted-platform Phase H3 (slice 1) — `std.json`: native JSON
  reader / writer implemented in-tree (`eta/core/src/eta/util/json.h`,
  no third-party dependency), hash-map / vector decode with optional
  integer-exact mode, port and string variants
  (`json:read`, `json:read-string`, `json:write`, `json:write-string`).
  Auto-imported by `std.prelude`. Tests in
  `stdlib/tests/json.test.eta`; reference at
  [`json.md`](guide/reference/json.md).
- ✅ Hosted-platform Phase H1 - Subprocess support (`std.process`):
  native `%process-*` builtins (`%process-run`, `%process-spawn`,
  `%process-wait`, `%process-kill`, `%process-terminate`,
  `%process-pid`, `%process-alive?`, `%process-exit-code`,
  `%process-handle?`, `%process-stdin-port`, `%process-stdout-port`,
  `%process-stderr-port`), stdlib wrapper module
  (`stdlib/std/process.eta`), stdlib tests
  (`stdlib/tests/process.test.eta`), C++ runtime tests
  (`eta/qa/test/src/process_primitives_tests.cpp`), examples
  (`cookbook/process/process-shellout.eta`, `cookbook/process/process-pipeline.eta`),
  and reference docs (`guide/reference/process.md`).
- ✅ Hosted-platform Phase H1 — Filesystem + OS primitives: native
  builtins (`file-exists?`, `directory?`, `delete-file`,
  `make-directory`, `list-directory`, `path-join`, `path-split`,
  `path-normalize`, `temp-file`, `temp-directory`,
  `file-modification-time`, `file-size`, `getenv`, `setenv!`,
  `unsetenv!`, `environment-variables`, `command-line-arguments`,
  `exit`, `current-directory`, `change-directory!`) plus the
  `std.fs` and `std.os` stdlib wrappers and reference docs
  ([`fs.md`](guide/reference/fs.md), [`os.md`](guide/reference/os.md)).
  See [release-notes.md](release-notes.md#2026-04-29).
- ✅ Hash map / hash set runtime delivery: native `HashMap` / `HashSet`
  object kinds, core builtins, stdlib wrappers (`std.hashmap`,
  `std.hashset`), docs, snippets, and test coverage across
  `eta_core_test` and `eta_stdlib_tests`. See
  [release-notes.md](release-notes.md#2026-04-28).
- ✅ Optimisation pipeline expansion: `PrimitiveSpecialisation` lowers
  proven builtin call sites to dedicated VM opcodes; self-recursive
  tail calls (unary) lower to backward `Jump`; lock-free function
  registry read path; zero-copy primitive arg dispatch via
  `std::span<const LispVal>`. See [optimisations.md](optimisations.md).
- ✅ VS Code / DAP polish round 1: inline-values provider,
  evaluatable-expression hover, code-lens, document links,
  jump-to-callee on `Call`/`TailCall` instructions, child-processes
  tree view.
- ✅ Release-bundle DLL sanity check now matches both the vcpkg
  (`xeus.dll`) and FetchContent (`libxeus.dll`) naming conventions
  on Windows; false-positive "missing DLL" warnings removed.
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

