# Release Notes

[<- Back to README](../README.md)

---

## 2026-05-03 (later)

### Breaking: `std.prelude` On-Disk Layout

The standard library prelude source/bytecode artifacts now live under
`stdlib/std/`:

- `stdlib/std/prelude.eta`
- `stdlib/std/prelude.etac`

The legacy root-level layout (`stdlib/prelude.eta`,
`stdlib/prelude.etac`) is no longer resolved for `std.prelude`.

### Atom Support (`std.atom`)

Hosted-platform Phase H2 now includes Atom support: a mutable single-cell
reference with compare-and-set semantics.

Highlights:

- New runtime object kind and payload:
  - `ObjectKind::Atom`
  - `types::Atom` with atomic `LispVal` cell storage
- GC and runtime plumbing:
  - Atom cell is traced as a strong reference during mark
  - value formatter now prints atoms as `#<atom>`
  - factory helper added: `make_atom(...)`
- New core primitives:
  - `%atom-new`
  - `%atom?`
  - `%atom-deref`
  - `%atom-reset!`
  - `%atom-compare-and-set!`
  - `%atom-swap!`
- `%atom-swap!` semantics:
  - implemented as a CAS retry loop
  - supports VM callback path (`vm->call_value`) and primitive-only
    fallback when no VM context is available
  - uses external GC root frames for transient callback values during retries
- New stdlib module:
  - `std.atom` (`stdlib/std/atom.eta`)
  - collision-safe names: `atom:new`, `atom:atom?`, `atom:deref`,
    `atom:reset!`, `atom:swap!`, `atom:compare-and-set!`
  - opt-in aliases: `atom`, `atom?`, `deref`, `reset!`, `swap!`,
    `compare-and-set!`
  - note: `std.atom` is intentionally not re-exported by `std.prelude`
    due `std.core:atom?` compatibility
- New tests:
  - C++ runtime tests: `eta/qa/test/src/atom_tests.cpp`
  - stdlib tests: `stdlib/tests/atom.test.eta`

Documentation updates:

- Added [docs/guide/reference/atom.md](guide/reference/atom.md).
- Updated [docs/guide/reference/modules.md](guide/reference/modules.md)
  with `std.atom` and prelude opt-in notes.
- Updated [docs/guide/reference/README.md](guide/reference/README.md)
  reference index.
- Updated [docs/next-steps.md](next-steps.md) to mark Atom as delivered.

---

## 2026-05-03

### Packaging Plan Implementation (S0-S7)

The packaging plan in `docs/plan/eta_packaging_plan.md` is now
implemented through S7, including runtime, CLI, build, and tooling
integration.

Highlights:

- Added the package manifest/lockfile core:
  - `eta.toml` parsing/validation (`eta::package::Manifest`)
  - deterministic `eta.lock` parsing/serialization (`eta::package::Lockfile`)
  - resolver graph validation for path/git/tarball dependency specs
- Added and expanded the umbrella CLI:
  - S2 commands: `new`, `init`, `tree`, `run`
  - S6 commands: `add`, `remove`, `update`, `build`, `test`, `bench`,
    `vendor`, `install`, `clean`
- Runtime resolver integration (S3):
  - project-root `eta.toml` discovery
  - lockfile-ordered `.eta/modules/<name>-<version>/{target/release,src}`
    roots
  - per-root `.etac` before `.eta` resolution
  - strict duplicate detection via `--strict-shadows`
- `.etac` format v4 metadata + stale-artifact policy (S4):
  - compiler/package/dependency hash metadata in bytecode
  - deterministic freshness checks with source fallback when possible
- Precompiled prelude/stdlib delivery (S5):
  - embedded `prelude.etac` blob in runtime binaries
  - bundled stdlib `.etac` artifacts installed beside stdlib sources
  - `Driver::load_prelude()` preference order: embedded -> `.etac` -> source
- Tooling integration (S7):
  - package-aware REPL/LSP/DAP/Jupyter resolution
  - LSP `eta-manifest` / `eta-lockfile` diagnostics
  - `eta/lockfile/explain` custom LSP request
  - DAP `profile` launch support (default `debug`) and workspace-rooted resolution
- Added packaging fixtures and smoke targets:
  - `packages/example/hello-world/`
  - `cookbook/packaging/end-to-end/`
  - expanded `eta_pkg_test`, `eta_cli_test`, and integration coverage in
    `eta_core_test`

Documentation updates:

- Added stage landing notes under:
  - `docs/plan/eta_packaging_s0_baseline.md`
  - `docs/plan/eta_packaging_s1_manifest_lockfile.md`
  - `docs/plan/eta_packaging_s2_minimal_cli.md`
  - `docs/plan/eta_packaging_s3_runtime_resolver_integration.md`
  - `docs/plan/eta_packaging_s4_etac_v4_stale_policy.md`
  - `docs/plan/eta_packaging_s5_precompiled_prelude_stdlib_etac.md`
  - `docs/plan/eta_packaging_s6_dependency_workflows.md`
  - `docs/plan/eta_packaging_s7_tooling_integration.md`
- Added user packaging docs:
  - [docs/packaging.md](packaging.md)
  - [docs/guide/packages.md](guide/packages.md)
  - [docs/app/first_app.md](app/first_app.md)
  - [cookbook/packaging/end-to-end/README.md](../cookbook/packaging/end-to-end/README.md)

---

## 2026-05-02 (later)

### Causal Stack Completion (tag `v0.5.6`)

The causal roadmap landed additional implementation slices across model
backbones, heterogeneous-effect estimation, and policy evaluation.

Highlights:

- Added native regression-tree and random-forest modules:
  - `std.ml.tree`
  - `std.ml.forest`
- Added causal-forest API:
  - `std.causal.forest` (`forest:fit-causal-forest`,
    `forest:predict-cate`, `forest:local-aipw`,
    `forest:variable-importance`)
- Added cross-fitting/DML API:
  - `std.causal.crossfit` (`crossfit:dml-plr`, `crossfit:dml-irm`,
    fold/nuisance helpers, CI helpers)
- Added uplift/policy-evaluation API:
  - `std.causal.policy` (Qini/AUUC, IPW/AIPW policy value,
    ranking/diagnostic helpers)
- Added and expanded stdlib tests:
  - `stdlib/tests/ml-tree.test.eta`
  - `stdlib/tests/causal-forest.test.eta`
  - `stdlib/tests/causal-crossfit.test.eta`
  - `stdlib/tests/causal-policy.test.eta`

Documentation updates:

- Expanded [docs/guide/reference/causal.md](guide/reference/causal.md)
  with the new modules and APIs.
- Updated the portfolio/featured documentation to reflect the completed
  causal stack and workflows.

---

## 2026-05-02

### Subprocess Support (`std.process`)

Hosted-platform Phase H1 is now complete with subprocess execution and
process lifecycle control.

Highlights:

- New native builtins:
  - `%process-run`
  - `%process-spawn`
  - `%process-wait`
  - `%process-kill`
  - `%process-terminate`
  - `%process-pid`
  - `%process-alive?`
  - `%process-exit-code`
  - `%process-handle?`
  - `%process-stdin-port`
  - `%process-stdout-port`
  - `%process-stderr-port`
- `%process-run` now supports:
  - captured/inherited/null stdio modes
  - string or bytevector stdin input
  - merged stderr-to-stdout mode
  - timeout handling via `process-timeout:` errors
  - binary capture mode (`binary? #t`) returning bytevectors
- New stdlib module: `std.process`:
  - `process:run`, `process:run-string`
  - `process:spawn`, `process:wait`
  - `process:kill`, `process:terminate`
  - `process:pid`, `process:alive?`, `process:exit-code`
  - `process:handle?`
  - `process:stdin-port`, `process:stdout-port`, `process:stderr-port`
- New C++ runtime tests:
  - `eta/qa/test/src/process_primitives_tests.cpp`
- New stdlib tests:
  - `stdlib/tests/process.test.eta`
- New examples:
  - `cookbook/process/process-shellout.eta`
  - `cookbook/process/process-pipeline.eta`

Documentation updates:

- Added [docs/guide/reference/process.md](guide/reference/process.md).
- Updated [docs/guide/reference/modules.md](guide/reference/modules.md)
  with `std.process`.
- Added process cross-links in:
  - [docs/guide/reference/os.md](guide/reference/os.md)
  - [docs/guide/reference/fs.md](guide/reference/fs.md)
- Updated [docs/next-steps.md](next-steps.md) to mark subprocess as
  shipped.
- Added VS Code snippets for process import/run/spawn in
  [editors/vscode/snippets/eta.json](../editors/vscode/snippets/eta.json).

---

## 2026-04-29 (later)

### JSON (`std.json`)

The first slice of hosted-platform Phase H3 ships: a native JSON
codec implemented in-tree (`eta/core/src/eta/util/json.h`), with no
third-party dependency.

Highlights:

- New native builtins: `%json-read`, `%json-read-string`,
  `%json-write`, `%json-write-string`.
- New stdlib module `std.json` with the user-facing API:
  - `json:read` — read one JSON document from any input port.
  - `json:read-string` — parse a JSON string.
  - `json:write` — write a value as JSON to a port (defaults to
    `(current-output-port)`).
  - `json:write-string` — serialise to a string.
- Reader option `'keep-integers-exact?` (default `#f`) preserves
  integer-typed JSON numbers as fixnums; otherwise all numbers
  decode to flonums for predictable arithmetic.
- Type mapping: object → hash map, array → vector, `true`/`false` →
  `#t`/`#f`, `null` → `'()`. Decoded objects use the same `HashMap`
  runtime kind as `std.hashmap`, so all `hash-map-*` builtins apply
  without conversion.
- `std.json` is auto-imported by `std.prelude`.
- New stdlib tests in `stdlib/tests/json.test.eta` covering
  default and integer-exact decoding, port-based reads, and a
  hash-map round trip.

Documentation updates:

- New reference page [docs/guide/reference/json.md](guide/reference/json.md).
- [docs/language_guide.md](language_guide.md) — section 13
  ("I/O, Filesystem & OS") gains a JSON subsection alongside the
  filesystem / OS coverage.
- [docs/guide/reference/modules.md](guide/reference/modules.md) — new
  `std.json` module entry; prelude re-export list updated.
- [docs/next-steps.md](next-steps.md) — Phase H3 capability matrix
  flipped to "Closed" for JSON; `std.format` and `std.log` remain
  as the unfinished H3 slices.

---

## 2026-04-29

### Filesystem & OS Primitives (`std.fs`, `std.os`)

The first half of hosted-platform Phase H1 ships: native filesystem
and operating-system builtins, with thin Eta stdlib wrappers and
reference documentation. Subprocess support (`std.process`) remains
outstanding.

Highlights:

- New native builtins (registered from
  `eta/core/src/eta/runtime/os_primitives.h`):
  - **Filesystem:** `file-exists?`, `directory?`, `delete-file`,
    `make-directory`, `list-directory`, `path-join`, `path-split`,
    `path-normalize`, `temp-file`, `temp-directory`,
    `file-modification-time`, `file-size`.
  - **OS / process:** `getenv`, `setenv!`, `unsetenv!`,
    `environment-variables`, `command-line-arguments`, `exit`,
    `current-directory`, `change-directory!`.
- New stdlib modules:
  - `std.fs` — re-exports the filesystem builtins under the `fs:`
    prefix.
  - `std.os` — re-exports the process / environment builtins under
    the `os:` prefix.
- Both modules are auto-imported by `std.prelude`, so the `fs:` and
  `os:` names are available with the standard `(import std.prelude)`
  pull-in. Direct import still works for users who prefer a smaller
  surface:

  ```scheme
  (import std.fs std.os)
  ```

- `command-line-arguments` is wired through `etai` and `etac` so
  scripts can read flags passed after the source file path.
- Path values round-trip through `std::filesystem` and use the
  platform-preferred separator on output (`\` on Windows, `/`
  elsewhere).
- `temp-file` / `temp-directory` allocate unique paths under the
  system temp root and create the file/directory before returning;
  cleanup is the caller's responsibility.

Documentation updates:

- New reference pages: [docs/guide/reference/fs.md](guide/reference/fs.md)
  and [docs/guide/reference/os.md](guide/reference/os.md).
- [docs/language_guide.md](language_guide.md) — section 13
  renamed to "I/O, Filesystem & OS" with subsections covering
  `std.fs` and `std.os`.
- [docs/guide/reference/modules.md](guide/reference/modules.md) — new
  module entries for `std.fs` and `std.os`; opt-in module list
  updated.
- [docs/next-steps.md](next-steps.md) — Phase H1 capability matrix
  updated, FS + OS marked closed, subprocess called out as the
  remaining slice.

---

## 2026-04-28

### Hash Maps and Hash Sets

Eta now ships native hash-map/hash-set runtime values and builtins in the core runtime.

Highlights:

- New runtime object kinds: `HashMap` and `HashSet`, including GC traversal and sandbox equality support.
- New core builtins:
  - Hash maps: `hash-map`, `make-hash-map`, `hash-map?`, `hash-map-ref`,
    `hash-map-assoc`, `hash-map-dissoc`, `hash-map-keys`, `hash-map-values`,
    `hash-map-size`, `hash-map->list`, `list->hash-map`, `hash-map-fold`, `hash`.
  - Hash sets: `make-hash-set`, `hash-set`, `hash-set?`, `hash-set-add`,
    `hash-set-remove`, `hash-set-contains?`, `hash-set-union`,
    `hash-set-intersect`, `hash-set-diff`, `hash-set->list`, `list->hash-set`.
- Value formatting now prints `#hashmap{...}` and `#hashset{...}`.
- Jupyter rich display now includes `HashMap` HTML/JSON previews
  (`application/vnd.eta.hashmap+json`) with capped first-N rendering.
- New stdlib modules:
  - `std.hashmap`
  - `std.hashset`
- `std.db` tabled relation caches now use hash maps for status/answer lookup.
- `std.regex` adds `regex:match-groups-hash` for named-group map access.
- New stdlib tests:
  - `stdlib/tests/hashmap.test.eta`
  - `stdlib/tests/hashset.test.eta`
  - `stdlib/tests/regex.test.eta` coverage for `regex:match-groups-hash`
- New C++ runtime tests:
  - `eta/qa/test/src/hash_map_tests.cpp`

Documentation updates:

- Added [docs/guide/reference/hashmap.md](guide/reference/hashmap.md).
- Added module reference entries in
  [docs/guide/reference/modules.md](guide/reference/modules.md).

---

## 2026-04-26

### Jupyter Kernel (`eta_jupyter`)

A native xeus-based Jupyter kernel now ships alongside the existing
executables. The kernel embeds the same `Driver` used by `etai` and
`eta_repl`, so notebook cells share the language semantics — modules,
macros, AAD, libtorch, CLP, causal, actors — with no FFI hop.

**Highlights:**

- New executable `eta_jupyter` built from `eta/tools/jupyter/` against
  `xeus` + `xeus-zmq` (vendored via `cmake/FetchXeus.cmake`); listed in
  the standard `eta_all` target.
- Runtime kernelspec installation:

  ```powershell
  eta_jupyter --install --user
  eta_jupyter --install --sys-prefix
  eta_jupyter --install --prefix <path>
  ```

- Rich display MIME bundles (with `text/plain` fallback) for tensors,
  fact tables, and heap snapshots:
  `application/vnd.eta.tensor+json`,
  `application/vnd.eta.facttable+json`,
  `application/vnd.eta.heap+json`.
- `(import std.jupyter)` exposes `jupyter:table`, `jupyter:plot`, and
  `jupyter:dag` rich-display helpers used by the showcase notebooks.
- Three showcase notebooks under `cookbook/notebooks/`:
  `LanguageBasics.ipynb`, `AAD.ipynb`, `Portfolio.ipynb`.
- Docs: [docs/guide/reference/jupyter.md](guide/reference/jupyter.md).

### Documentation

- Top-level `README.md` re-pitched: notebook links surfaced under
  Featured Examples, install / REPL / VS Code / build sections folded
  into `<details>` blocks, duplicated directory trees moved to
  `docs/quickstart.md` and `docs/build.md`, doc table trimmed to top
  hits with the full index living in `docs/index.md`.
- `docs/aad.md` and `docs/examples.md` link directly to their
  notebook equivalents.

---

## 2026-04-20

### CLP(R) Convex QP Stage 6 Rollout Gate

- Added a baseline benchmark executable: `eta_qp_bench`.
- Added benchmark runner scripts:
  - `scripts/qp-benchmark.ps1`
  - `scripts/qp-benchmark.sh`
- Benchmark output now compares LP-proxy and QP solve paths for representative
  sizes (`8,16,24,32`) with:
  - objective quality (`true_qp - true_lp`)
  - runtime (`lp_ms`, `qp_ms`, `qp/lp`)
  - numerical stability (`qp_obj_drift`, `qp_weight_drift`)
- Added optional rollout gating with `--gate` to enforce parity/stability
  thresholds before enabling or promoting changes.
- Documented convex QP optimization and backend flag behavior in:
  - `docs/clp.md`
  - `docs/portfolio.md`

### Commands

Windows PowerShell:

```powershell
.\scripts\qp-benchmark.ps1 -BuildDir C:\Users\lewis\develop\eta\out\msvc-release -Gate
```

Linux/macOS:

```bash
BUILD_DIR=./out/wsl-clang-release GATE=1 ./scripts/qp-benchmark.sh
```

### Backend Default

- `ETA_CLP_QP_BACKEND` is now compiled in unconditionally in
  `eta/core/CMakeLists.txt`.

