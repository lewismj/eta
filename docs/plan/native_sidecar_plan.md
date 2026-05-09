# Native Sidecar Plan

[Back to README](../../README.md) |
[Packaging](../packaging.md) |
[Package Commands](../guide/packages.md) |
[Next Steps](../next-steps.md)

Status: NS0-NS8 implemented (2026-05-09).

---

## 1) Objective

Move C++-backed functionality (currently linked and registered as builtins)
to package-managed native sidecars, while keeping:

1. deterministic builds and runtime behavior,
2. clear compatibility checks,
3. simple migration for existing `std.torch`, `std.net`/nng, `std.stats`,
   and `std.log`.

End state: native capabilities are delivered by packages, not by hard-linked
`eta/builtins/*` targets.

---

## 2) Current state (May 2026)

1. First-party native bindings are built as interface targets under
   `eta/builtins/{torch,nng,stats,log}` and linked into all runtime tools
   (`etai`, `eta_repl`, `etac`, `eta_lsp`, `eta_dap`, `eta_jupyter`,
   `eta_test`).
2. Driver registration is centralized in:
   - `eta/tools/interpreter/src/eta/interpreter/all_primitives.h`
   - `eta/session/src/eta/session/driver.h`
3. `.etac` freshness currently validates builtin count; mismatches surface as
   `BuiltinCountMismatch` (bytecode format is currently v5).
4. Workspace support is now shipped:
   - context discovery (`StandalonePackage`, `WorkspaceRoot`,
     `WorkspaceMember`, `WorkspaceNonMember`) in
     `eta/core/src/eta/package/discovery.*`,
   - workspace member expansion and dependency union in
     `eta/core/src/eta/package/resolver.*`,
   - shared workspace artifact layout:
     `.eta/target/<profile>/<member>/...` and workspace-root `.eta/modules`.
5. Lockfile package sources already encode workspace/non-workspace origin via
   `source = "root" | "workspace+<rel>" | "path+..." | "git+..." | "tarball+..."`.
6. Tooling paths are workspace-aware through `ModulePathResolver` and
   tool-specific context plumbing (CLI/LSP/DAP/Jupyter), but none of these paths
   model native sidecar artifacts yet.
7. Packaging S0-S7 is shipped (`eta.toml`, `eta.lock`, resolver, vendor/install,
   tooling integration), but manifest/resolver/lockfile schemas do not yet model
   native sidecars.

---

## 3) Design decisions

### 3.1 Keep two primitive domains

Separate primitive registration into:

1. **Core builtins**: stable, shipped with runtime (`builtin_count` remains
   meaningful and low churn).
2. **Extension primitives**: loaded from sidecars, package-driven.

Do not fold sidecar primitives into the core builtin count.

### 3.2 Package-managed sidecars

Native binaries are artifacts attached to packages and selected by target triple
at install/vendor/build time.

### 3.3 Explicit runtime ABI

Introduce a versioned native extension ABI (`eta-native-v1`) with:

1. required entrypoint,
2. extension metadata handshake,
3. runtime-provided registration API,
4. deterministic symbol registration and conflict checks.

### 3.4 Deterministic extension environment hash

Compile and load against an **extension symbol hash** derived from lockfile
order + symbol metadata, instead of relying on builtin slot count churn.

### 3.5 Workspace invariants (must not regress)

1. Keep `discover_manifest_context` as the single source of truth for selecting
   `lockfile_root` and `modules_root`.
2. Keep workspace behavior unchanged:
   - one shared workspace lockfile (`<workspace>/eta.lock`),
   - one shared workspace modules cache (`<workspace>/.eta/modules`),
   - no per-member `.eta/modules` in workspace mode.
3. Keep current workspace package-source semantics (`workspace+<rel>`), including
   path containment checks under workspace root.
4. Keep command-selection semantics unchanged (`--workspace`, `-p/--package`,
   `--exclude`, `--manifest-path`) for aggregate and single-target commands.
5. Sidecar loading must be additive: no behavior changes for projects without
   `[native]` metadata.

---

## 4) Package and artifact model

## 4.1 New package shape

Add native sidecar metadata to `eta.toml`:

```toml
[package]
name = "eta-torch-sidecar"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.7, <0.8"

[native]
kind = "sidecar"
abi = "eta-native-v1"
id = "torch"
entry = "eta_register_extension_v1"

[[native.targets]]
triple = "x86_64-pc-windows-msvc"
artifact = "native/windows-x64/eta_torch_sidecar.dll"
sha256 = "..."

[[native.targets]]
triple = "x86_64-unknown-linux-gnu"
artifact = "native/linux-x64/libeta_torch_sidecar.so"
sha256 = "..."
```

`stdlib/std/torch.eta` remains an Eta package/module wrapper. It depends on the
sidecar package through normal dependency edges.

## 4.2 Lockfile additions

Extend `eta.lock` entries with sidecar fields:

1. `native_id`
2. `native_abi`
3. `native_entry`
4. `native_target_triple`
5. `native_artifact_relpath`
6. `native_sha256`

These become the source of truth for runtime loading.

## 4.3 Materialization layout

Under `.eta/modules/<pkg>-<ver>/`:

1. `eta.toml`
2. `eta.lock`
3. Eta artifacts (`src/`, `target/release/*.etac`)
4. native artifacts under `native/<platform>/...`

Workspace clarification:

1. External dependencies (`path+`, `git+`, `tarball+`) continue to materialize
   under workspace-root `.eta/modules`.
2. Workspace members (`workspace+...`) are **not** copied into `.eta/modules`;
   their package roots stay in-place under the workspace tree.

## 4.4 Native artifact root resolution (workspace-safe)

Do not derive sidecar paths by string slicing lockfile `source` values in the
loader. Resolve package roots through existing package resolution APIs and then
append `native_artifact_relpath`.

Implementation shape:

1. Build a package-root map via existing resolver calls:
   - standalone: `resolve_dependencies(active_manifest, options)`
   - workspace: `resolve_workspace_members(...)` +
     `resolve_workspace_dependencies(..., options)`
2. For each lockfile package with native fields, map `package.name` to resolved
   `package_root` and compute:
   `artifact_abs = canonicalize(package_root / native_artifact_relpath)`.
3. Enforce `artifact_abs` stays inside `package_root` (`is_path_within`) before
   checksum/load.
4. Keep lockfile order as load order after filtering to the active dependency
   closure (see 6.2).

---

## 5) Native ABI and loader

## 5.1 ABI header

Add SDK header (initially in-repo):

- `eta/core/src/eta/native/sdk.h` (or `eta/native/sdk.h`)

Core structs:

1. `EtaNativeApiV1`
2. `EtaExtensionInfoV1`
3. function pointer types for primitive registration
4. error reporting callbacks

Required entrypoint exported by sidecar:

```c
ETA_NATIVE_EXPORT int eta_register_extension_v1(
    const EtaNativeApiV1* api,
    EtaExtensionInfoV1* out_info);
```

## 5.2 Runtime loader

Add cross-platform dynamic loader:

1. Windows: `LoadLibraryA` / `GetProcAddress`
2. Unix: `dlopen` / `dlsym`

New components:

1. `eta/core/src/eta/native/sidecar_loader.h/.cpp`
2. `eta/core/src/eta/native/extension_registry.h/.cpp`

Responsibilities:

1. discover package/workspace context using existing discovery rules,
2. resolve lockfile + package-root map using existing resolver APIs,
3. filter lockfile entries to the active package dependency closure,
4. resolve target artifact paths, then verify checksum before load,
5. load dynamic library and perform ABI handshake,
6. register extension symbols into extension environment,
7. keep handles alive until process exit.

## 5.3 Loader context plumbing

Add one internal context struct used by all sidecar-aware entrypoints:

1. `context_kind` (`StandalonePackage`, `WorkspaceRoot`,
   `WorkspaceMember`, `WorkspaceNonMember`)
2. `active_manifest_path`
3. `workspace_manifest_path` (optional)
4. `lockfile_root`
5. `modules_root`
6. `package_root_by_name` (from resolver graph)

Populate it through existing behavior already used by CLI/tools:

1. `eta::package::discover_manifest_context(...)`
2. `resolve_dependencies(...)` or `resolve_workspace_dependencies(...)`
3. lockfile at `<lockfile_root>/eta.lock`

Do not introduce duplicate parent-directory traversal logic in loader code.

## 5.4 Safety checks at load

Hard errors for:

1. missing artifact,
2. checksum mismatch,
3. missing entrypoint,
4. ABI version mismatch,
5. duplicate extension id,
6. duplicate symbol name across extensions.

---

## 6) Runtime and compiler integration

## 6.1 Primitive environments

Current `BuiltinEnvironment` remains core-only.

Add extension environment abstraction with same primitive contract:

1. name
2. arity
3. has_rest
4. callable function

Wire both environments into:

1. semantic analyzer seeding,
2. VM global installation,
3. diagnostics/completions where relevant.

## 6.2 Driver flow updates

`Driver` startup/load flow:

1. register core builtins (existing path),
2. build loader context via package/workspace discovery,
3. if no active package manifest or no lockfile is available, continue with
   core-only execution (no sidecar load attempt),
4. resolve dependency graph rooted at active package manifest,
5. intersect graph closure with lockfile packages that declare native fields,
6. load sidecars in lockfile order and register extension primitives,
7. compile/execute with core + extension environments.

## 6.3 Extension symbol hashing

Compute deterministic hash from:

1. loaded extension order (lockfile order after closure filtering),
2. extension id + version + ABI id,
3. exported symbol descriptors `(name, arity, has_rest)` sorted by symbol name.

This hash is embedded in `.etac` metadata and validated at load.

## 6.4 Workspace context behavior

1. `WorkspaceMember`:
   - `lockfile_root = workspace_root`
   - resolve closure from current member manifest
   - load only sidecars reachable from that member closure
2. `WorkspaceRoot` with rooted workspace package:
   - resolve closure from root package manifest
3. `WorkspaceRoot` virtual or `WorkspaceNonMember` with no selected package:
   - do not hard-fail startup due to unresolved package selection
   - run core-only until a concrete package context is available
4. Keep existing CLI/tool behavior for selection and working-directory routing.

---

## 7) Bytecode and freshness policy

## 7.1 Extend metadata (v6)

Current runtime already uses `.etac` format v5. Bump to v6 and add:

1. `core_builtin_count` (core only),
2. `extension_env_hash`,
3. optional list of required extension ids (diagnostics).

## 7.2 Freshness decisions

During `.etac` load:

1. fail/refresh on core builtin mismatch (as today),
2. fail/refresh on extension hash mismatch,
3. keep existing compiler id/source/manifest/dependency checks,
4. preserve backward-compatible read path for v5/v4/v3 artifacts.

Diagnostic text should explicitly name missing/mismatched extension ids.

---

## 8) CLI and packaging updates

## 8.1 Manifest parser

Extend:

- `eta/core/src/eta/package/manifest.h/.cpp`

to parse/validate `[native]` and `[[native.targets]]`.

Validation rules:

1. `[native]` is valid only when `[package]` exists (workspace-only manifests
   cannot declare native sidecars).
2. `native.kind`, `native.abi`, `native.id`, `native.entry` are required.
3. each `[[native.targets]]` row requires `triple`, relative `artifact`, and
   `sha256`.
4. reject duplicate `native.id` within one package and duplicate target
   `triple` rows.

## 8.2 Lockfile parser/writer

Extend:

- `eta/core/src/eta/package/lockfile.h/.cpp`

for sidecar fields with deterministic ordering and stable output.

Implementation details:

1. add optional native fields on `LockfilePackage`.
2. preserve current deterministic package/dependency sort order.
3. keep schema version compatibility (accept current lockfile version while
   parsing/writing new optional fields).
4. include read/write tests for mixed graphs (native and non-native packages).

## 8.3 Resolver/materializer

Extend:

- `eta/core/src/eta/package/resolver.h/.cpp`
- vendor/install flows in CLI

to:

1. carry sidecar metadata through resolved graph and lockfile construction,
2. select target triple deterministically at lockfile/build time,
3. materialize native artifacts for non-workspace packages into `.eta/modules`,
4. keep existing workspace behavior (`workspace+` packages are not copied into
   `.eta/modules`),
5. verify checksums during materialization and again before load.

Guardrails:

1. keep existing lockfile root/modules root selection in
   `resolve_project_state(...)` (workspace-rooted when in workspace member mode).
2. reuse existing containment checks (`is_path_within`) before reading or
   loading native artifacts.
3. keep `materialize_modules_from_lockfile(...)` behavior for `root` and
   `workspace+` sources unchanged except for native-metadata awareness.

## 8.4 Diagnostics and commands

Add focused UX:

1. `eta tree --native` (show sidecar edges),
2. `eta doctor` native checks (missing binaries, checksum mismatch, ABI mismatch),
3. clearer build/run errors when required sidecar is absent.

---

## 9) Migration strategy for existing builtins

## 9.1 Migration order

1. `log` (smallest surface and lowest runtime coupling),
2. `stats`,
3. `nng`,
4. `torch` (largest API and most dependencies).

## 9.2 Per-module migration steps

For each current module (`eta/builtins/<name>`):

1. create sidecar package scaffold under `packages/stdlib/native/<name>/`,
2. export ABI entrypoint that registers the same primitive names,
3. make wrapper stdlib package depend on sidecar package,
4. add sidecar integration tests,
5. remove direct registration call from `all_primitives.h`,
6. remove hard link from tool targets after stabilization.

Concrete implementation notes per module:

1. keep primitive names/arity/rest flags byte-for-byte identical to avoid
   semantic analyzer/bytecode behavior drift.
2. preserve existing stdlib wrapper module public API (`stdlib/std/*.eta`) so
   user code does not change.
3. run parity tests in both modes (`ETA_NATIVE_BUILTIN_FALLBACK=ON/OFF`) before
   removing linked registration.
4. update `builtin_metadata`/docs surface only after sidecar-backed registration
   is proven in CI.

## 9.3 Temporary compatibility flag

Add temporary build flag:

- `ETA_NATIVE_BUILTIN_FALLBACK=ON` (default ON during migration)

Behavior:

1. if sidecar load fails, optionally fall back to linked builtin registration,
2. CI lane with fallback OFF to enforce sidecar correctness,
3. remove fallback at end of migration.

## 9.4 CMake/linkage transition details

During migration, keep tool build graph stable and remove links in one module at
a time:

1. current linked targets live in:
   - `eta/tools/interpreter/CMakeLists.txt`
   - `eta/tools/compiler/CMakeLists.txt`
   - `eta/tools/lsp/CMakeLists.txt`
   - `eta/tools/dap/CMakeLists.txt`
   - `eta/tools/jupyter/CMakeLists.txt`
   - `eta/tools/test_runner/CMakeLists.txt`
   - top-level `CMakeLists.txt` (`eta_all` aggregation)
2. move from hard link to sidecar load behind fallback flag first.
3. drop `eta_copy_*_dlls(...)` calls only after fallback-off CI is green for the
   migrated module.

---

## 10) Detailed staged roadmap

Each stage is mergeable and has explicit test gates.

### NS0 - Freeze contracts and add harness

Scope:

1. freeze current workspace behavior as a non-regression baseline:
   - lockfile/modules root selection,
   - workspace member/default-member selection,
   - module path ordering,
   - tool startup in workspace contexts.
2. add fixture projects with mock sidecar metadata (no loader yet), including:
   - standalone package fixture,
   - rooted workspace fixture,
   - virtual workspace fixture.

Gate:

1. existing runtime/package suites green,
2. `eta_pkg_test`, `eta_cli_test`, `module_path_tests`, `lsp_tests`,
   `dap_tests` remain green before any sidecar logic lands.

Implementation snapshot (2026-05-08):

1. Added mock native fixture projects for:
   - standalone package,
   - rooted workspace,
   - virtual workspace.
2. Added non-regression coverage in:
   - `eta/qa/pkg_test/src/eta_pkg_test.cpp` (manifest/discovery/workspace member resolution),
   - `eta/qa/cli_test/src/eta_cli_test.cpp` (workspace defaults and shared lockfile/modules roots),
   - `eta/qa/test/src/module_path_tests.cpp` (workspace member module-path ordering),
   - `eta/qa/test/src/lsp_tests.cpp` (workspace lockfile explain context),
   - `eta/qa/test/src/dap_tests.cpp` (workspace-member launch/startup).
3. Mock fixture metadata intentionally uses `[native]` keys only; target-table
   schema (`[[native.targets]]`) remains part of NS3.

### NS1 - Sidecar ABI and loader skeleton

Scope:

1. add SDK header (`eta-native-v1`),
2. add loader abstraction + platform backends,
3. add `NativeLoadContext` plumbing and package-root resolution helpers,
4. add mocked sidecar load tests (fake DLL/SO with test entrypoint).

Gate:

1. unit tests for load/open/symbol lookup and error mapping,
2. unit tests for artifact containment checks and lockfile-order determinism.

Implementation snapshot (2026-05-08):

1. Added ABI v1 SDK header at `eta/core/src/eta/native/sdk.h` with:
   - `EtaNativeApiV1`,
   - `EtaExtensionInfoV1`,
   - primitive registration and error reporting callback types,
   - exported entrypoint declaration `eta_register_extension_v1`.
2. Added native loader skeleton and extension registry:
   - `eta/core/src/eta/native/sidecar_loader.h/.cpp`,
   - `eta/core/src/eta/native/extension_registry.h/.cpp`,
   - platform dynamic library backends (`LoadLibrary`/`GetProcAddress` and
     `dlopen`/`dlsym`),
   - `NativeLoadContext` construction from existing package/workspace discovery
     and resolver APIs,
   - package-root based sidecar artifact resolution with containment checks and
     deterministic input-order preservation.
3. Added NS1 unit coverage in `eta/qa/test/src/native_sidecar_loader_tests.cpp`
   with a mock sidecar module target for:
   - library open failure mapping,
   - missing entrypoint symbol mapping,
   - ABI mismatch mapping,
   - sidecar registration failure mapping,
   - successful metadata/symbol registration path,
   - artifact escape rejection and lockfile-order deterministic resolution.

### NS2 - Extension registry in runtime

Scope:

1. add extension primitive environment,
2. install extension primitives into VM globals after core builtins,
3. semantic analyzer support for core + extension environments,
4. split analysis registration into core-only vs extension-aware paths
   (so tools can run without native artifacts).

Gate:

1. compile/execute tests with synthetic extension primitive set,
2. existing core-only projects still compile/execute unchanged.

Implementation snapshot (2026-05-08):

1. Added extension primitive environment support in
   `eta/core/src/eta/runtime/extension_env.h` with:
   - extension primitive descriptors matching builtin primitive contract,
   - deterministic slot-offset installation into VM globals.
2. Extended semantic analysis APIs in
   `eta/core/src/eta/semantics/semantic_analyzer.h/.cpp` to support:
   - core-only analysis overloads (unchanged call path),
   - extension-aware analysis overloads that seed core slots first and
     extension slots immediately after.
3. Updated runtime driver plumbing in
   `eta/session/src/eta/session/driver.h` to:
   - maintain a separate extension primitive environment,
   - install core + extension primitives in fixed global slots before execute,
   - apply primitive-slot boundaries consistently in source compile, `.etac`
     execution, relocation planning, child closure bootstrap, and global-name
     completion metadata.
4. Added NS2 coverage with synthetic extension primitives:
   - `eta/qa/test/src/semantics_tests.cpp` (core+extension slot seeding order),
   - `eta/qa/test/src/driver_jupyter_test.cpp` (driver compile/execute and
     completion visibility for extension primitives).

### NS3 - Manifest and lockfile schema extensions

Scope:

1. parse/validate `[native]` and target tables,
2. persist native fields in `eta.lock`,
3. enforce package-only `[native]` usage (workspace-only manifests reject it),
4. deterministic read/write + validation errors.

Gate:

1. package parser tests covering malformed metadata and missing required fields,
2. lockfile round-trip tests for mixed native/non-native package graphs.

Implementation snapshot (2026-05-09):

1. Extended manifest schema in `eta/core/src/eta/package/manifest.h/.cpp` with:
   - `[native]` parsing/validation for `kind`, `abi`, `id`, `entry`,
   - `[[native.targets]]` parsing/validation for `triple`, relative `artifact`,
     and `sha256`,
   - package-only enforcement for `[native]` (workspace-only manifests reject it),
   - deterministic target ordering in serialized manifests.
2. Extended lockfile schema in `eta/core/src/eta/package/lockfile.h/.cpp` with
   optional native fields:
   - `native_id`,
   - `native_abi`,
   - `native_entry`,
   - `native_target_triple`,
   - `native_artifact_relpath`,
   - `native_sha256`,
   plus deterministic read/write and validation for complete native metadata.
3. Updated package fixtures and parser/lockfile coverage in
   `eta/qa/pkg_test/src/eta_pkg_test.cpp` and sidecar fixture manifests to cover:
   - malformed native metadata and missing required fields,
   - duplicate native target triples,
   - lockfile native metadata round-trip for mixed native/non-native graphs.

### NS4 - Resolver and materialization for sidecars

Scope:

1. target triple selection,
2. materialize native artifacts under `.eta/modules` for non-workspace packages,
3. keep `workspace+` package materialization semantics unchanged,
4. checksum verification at materialization and load.

Gate:

1. CLI integration tests (`eta vendor`, `eta build`, `eta run`) with fixture sidecars,
2. workspace-member build/run tests remain green with shared workspace
   lockfile/modules roots.

Implementation snapshot (2026-05-09):

1. Extended resolver graph + lockfile construction in
   `eta/core/src/eta/package/resolver.h/.cpp` to:
   - select one `[[native.targets]]` row by host target triple,
   - persist selected native fields into lockfile package rows,
   - fail resolution when a native package has no target for the selected triple.
2. Extended module materialization in `eta/cli/src/eta/cli/main_eta.cpp` to:
   - keep existing `root`/`workspace+` behavior unchanged,
   - verify `native_sha256` for materialized non-workspace packages before
     command execution proceeds.
3. Extended sidecar loader checks in
   `eta/core/src/eta/native/sidecar_loader.h/.cpp` with optional load-time
   SHA-256 verification (`expected_sha256`) and checksum mismatch error mapping.
4. Added NS4 coverage in:
   - `eta/qa/pkg_test/src/eta_pkg_test.cpp` (target-triple selection and
     missing-target errors),
   - `eta/qa/cli_test/src/eta_cli_test.cpp` (`vendor`/`build`/`run` flow with
     native fixture dependency and checksum mismatch failure),
   - `eta/qa/test/src/native_sidecar_loader_tests.cpp` (load-time checksum mismatch).
5. Updated native sidecar fixture manifests with multi-target entries so
   workspace/standalone sidecar fixture flows remain platform-agnostic.

### NS5 - Bytecode v6 extension metadata

Scope:

1. add `extension_env_hash` to serializer/deserializer,
2. freshness checks and diagnostics,
3. backward-compatible read path for v5/v4/v3.

Gate:

1. serializer tests for mismatch cases and compatibility paths.

Implementation snapshot (2026-05-09):

1. Extended bytecode format metadata in
   `eta/core/src/eta/runtime/vm/bytecode_serializer.h/.cpp` to:
   - bump `.etac` format from v5 to v6,
   - add `extension_env_hash` to serialized header metadata,
   - preserve backward-compatible deserialization for v5/v4/v3,
   - keep v5 relocation metadata handling on the modern relocation path.
2. Extended freshness policy in
   `eta/core/src/eta/runtime/vm/bytecode_serializer.h/.cpp` with:
   - `expected_extension_env_hash` in `FreshnessContext`,
   - extension-hash mismatch and missing-hash freshness diagnostics,
   - compatibility behavior for legacy v5/v4/v3 artifacts when runtime
     expects zero extension hash.
3. Wired runtime freshness input in `eta/session/src/eta/session/driver.h` to:
   - compute deterministic extension-environment hash from registered
     extension primitive descriptors,
   - include that hash in `.etac` freshness checks.
4. Updated `.etac` emit paths to include extension hash metadata:
   - `eta/tools/compiler/src/eta/compiler/main_etac.cpp`,
   - `eta/qa/test/src/packaging_contract_tests.cpp`,
   - `eta/qa/test/src/cookbook/compiled_example_tests.cpp`.
5. Added NS5 coverage in:
   - `eta/qa/test/src/bytecode_serializer_tests.cpp`
     (v6 extension hash round-trip, freshness mismatch and missing-hash
     cases, and v5/v4/v3 compatibility deserialization),
   - `eta/qa/test/src/packaging_contract_tests.cpp`
     (driver stale `.etac` fallback triggered by extension-hash mismatch).

### NS6 - Driver/package-aware sidecar loading

Scope:

1. load sidecars from lockfile before compile/run in package context,
2. apply dependency-closure filtering so unrelated workspace members do not
   block execution,
3. plumb into REPL/LSP/DAP/Jupyter package discovery path.

Gate:

1. package-aware tool integration tests with native fixture,
2. workspace virtual-root/non-member tool startup does not hard-fail when no
   package is selected.

Implementation snapshot (2026-05-09):

1. Extended `eta/session/src/eta/session/driver.h` to load lockfile-selected
   native sidecars before compile/run in package contexts by:
   - discovering package/workspace context from the active start directory,
   - traversing dependency closure from the active package row in `eta.lock`,
   - filtering lockfile native rows to only closure-reachable packages, and
   - loading sidecars in lockfile order before semantic analysis/execution.
2. Added driver-side extension registration plumbing that maps loaded sidecar
   symbol metadata into the extension primitive environment with deterministic
   symbol ordering, preserving core-only behavior when no package context or
   lockfile is available.
3. Added public sidecar SHA-256 helper in
   `eta/core/src/eta/native/sidecar_loader.h/.cpp` for package-aware fixture
   setup and lockfile-native test construction.
4. Updated compiler startup in
   `eta/tools/compiler/src/eta/compiler/main_etac.cpp` to load package sidecars
   from the input file context before optional prelude loading.
5. Added NS6 coverage in `eta/qa/test/src/driver_jupyter_test.cpp` for:
   - workspace-member sidecar loading with dependency-closure filtering so
     unrelated workspace members with broken sidecar metadata do not block
     execution, and
   - workspace virtual-root core-only startup without hard-failing on
     lockfile-native metadata when no package is selected.

### NS7 - Migrate `log` to sidecar

Scope:

1. package `eta-log-sidecar`,
2. stdlib wrapper dependency wiring,
3. remove direct `log` registration from `register_all_primitives` when fallback OFF.

Gate:

1. `log` unit + stdlib tests pass with fallback OFF.

Implementation snapshot (2026-05-09):

1. Added `eta-log-sidecar` package scaffold at
   `packages/stdlib/native/log/eta.toml` with native metadata:
   - `kind = "sidecar"`,
   - `abi = "eta-native-v1"`,
   - `id = "eta.log.sidecar"`,
   - `entry = "eta_register_log_extension_v1"`,
   and platform target rows for Windows/Linux/macOS artifacts.
2. Extended primitive registration in
   `eta/tools/interpreter/src/eta/interpreter/all_primitives.h` to support
   native fallback toggling:
   - `ETA_NATIVE_BUILTIN_FALLBACK=ON` keeps linked `%log-*` registration,
   - `ETA_NATIVE_BUILTIN_FALLBACK=OFF` installs `%log-*` placeholders that
     require the `eta-log-sidecar` package.
3. Extended sidecar activation in `eta/session/src/eta/session/driver.h` so
   extension id `eta.log.sidecar`:
   - is treated as a runtime activation marker (not metadata-only placeholders),
   - enables `%log-*` implementations from `eta::log::register_log_primitives`
     when fallback is OFF, preserving sidecar-first behavior without duplicate
     extension symbol shadowing.
4. Extended native sidecar test module in
   `eta/qa/test/src/native_sidecar_test_extension.cpp` with
   `eta_register_log_extension_v1`, exporting the `%log-*` symbol metadata set.
5. Added NS7 coverage in `eta/qa/test/src/driver_jupyter_test.cpp` for:
   - fallback OFF failure mode when `std.log` is used without sidecar context,
   - fallback OFF success path where a lockfile-managed `eta.log.sidecar`
     package activates stdlib `std.log` wrappers in package context.

### NS8 - Migrate `stats`, `nng`, `torch`

Scope:

1. repeat migration pattern for remaining modules,
2. ensure platform runtime dependencies are packaged with sidecars.

Gate:

1. full stdlib + cookbook + torch/nng/stats C++ suites pass on CI matrix,
2. workspace fixtures using migrated modules pass in both fallback modes.

Implementation snapshot (2026-05-09):

1. Added package scaffolds:
   - `packages/stdlib/native/stats/eta.toml` (`eta.stats.sidecar`),
   - `packages/stdlib/native/nng/eta.toml` (`eta.nng.sidecar`),
   - `packages/stdlib/native/torch/eta.toml` (`eta.torch.sidecar`),
   each with `kind = "sidecar"`, `abi = "eta-native-v1"`, named entrypoints,
   and target rows for Windows/Linux/macOS artifacts.
2. Extended fallback handling for `stats`, `torch`, and `nng`:
   - `ETA_NATIVE_BUILTIN_FALLBACK=ON` keeps linked primitive registration,
   - `ETA_NATIVE_BUILTIN_FALLBACK=OFF` installs placeholders that require
     `eta-stats-sidecar`, `eta-torch-sidecar`, and `eta-nng-sidecar`.
3. Extended sidecar activation in `eta/session/src/eta/session/driver.h` so
   extension ids `eta.stats.sidecar`, `eta.torch.sidecar`, and
   `eta.nng.sidecar` activate their builtin primitive implementations at
   runtime (mirroring `eta.log.sidecar`) when fallback is OFF.
4. Extended native sidecar fixture exports in
   `eta/qa/test/src/native_sidecar_test_extension.cpp` with:
   - `eta_register_stats_extension_v1`,
   - `eta_register_torch_extension_v1`,
   - `eta_register_nng_extension_v1`,
   publishing symbol metadata from builtin definitions.
5. Added NS8 coverage:
   - `eta/qa/test/src/driver_jupyter_test.cpp` now validates fallback OFF
     failure and sidecar-activation success for `std.stats`, `std.net`/nng,
     and `std.torch`,
   - `eta/qa/test/src/native_sidecar_loader_tests.cpp` verifies loader
     registration for the new NS8 sidecar entrypoints.

### NS9 - Remove hard links and fallback

Scope:

1. stop linking `eta_torch`, `eta_nng`, `eta_stats`, `eta_log` into tools,
2. remove fallback mode and old registration paths,
3. keep compatibility docs for one release.

Gate:

1. clean install from release bundle works via sidecars only,
2. no runtime tool target links first-party builtin native libs.

Implementation snapshot (2026-05-09):

1. Migrated first-party native builtin source ownership from
   `eta/builtins/{log,nng,stats,torch}` to package-sidecar roots:
   - `packages/stdlib/native/log/src/eta/log/*`,
   - `packages/stdlib/native/nng/src/eta/nng/*`,
   - `packages/stdlib/native/stats/src/eta/stats/*`,
   - `packages/stdlib/native/torch/src/eta/torch/*`.
2. Removed hard links from runtime tools/tests to
   `eta_torch`, `eta_nng`, `eta_stats`, and `eta_log`, and replaced that wiring
   with direct sidecar-runtime dependency linkage (libtorch, nng, spdlog) plus
   package-sidecar include roots.
3. Removed temporary `ETA_NATIVE_BUILTIN_FALLBACK` behavior:
   - core builtin registration now always installs sidecar placeholders for
     torch/stats/log and nng,
   - sidecar extension activation now always enables first-party primitive
     implementations when matching sidecar ids are loaded.
4. Updated driver-side integration tests to validate sidecar-only behavior and
   assert the legacy fallback env var no longer re-enables linked builtins.

### NS10 - External package hardening

Scope:

1. document sidecar authoring guide,
2. add ABI conformance tests,
3. prepare for registry/signing integration (S8 packaging follow-up).

Gate:

1. third-party sample sidecar package builds and runs via public SDK path.

---

## 11) Testing strategy

## 11.1 New test layers

1. **ABI/loader unit tests**:
   - entrypoint mismatch
   - ABI mismatch
   - checksum mismatch
   - duplicate symbol detection
   - artifact path containment (`artifact_relpath` cannot escape package root)
2. **Package contract tests**:
   - lockfile native fields
   - target triple selection
   - workspace manifest/lockfile validation with native metadata
3. **Runtime integration tests**:
   - compile and run with sidecar loaded
   - fail with actionable error when sidecar missing
   - extension hash mismatch fallback/rebuild
4. **Tooling tests**:
   - package-aware REPL/LSP/DAP/Jupyter with sidecars
   - workspace-member context sidecar resolution
   - virtual workspace context non-failure when no package selected
5. **Migration parity tests**:
   - behavior parity between builtin and sidecar modes during transition.

Target suites to extend:

1. `eta/qa/pkg_test/src/eta_pkg_test.cpp`
2. `eta/qa/cli_test/src/eta_cli_test.cpp`
3. `eta/qa/test/src/module_path_tests.cpp`
4. `eta/qa/test/src/bytecode_serializer_tests.cpp`
5. `eta/qa/test/src/lsp_tests.cpp`
6. `eta/qa/test/src/dap_tests.cpp`

## 11.2 CI matrix

Run sidecar scenarios on:

1. Windows x64
2. Linux x86_64
3. macOS arm64/x86_64 (as available)

Each platform should run:

1. standalone package lane,
2. workspace member lane,
3. workspace virtual-root lane (tool startup/diagnostics only),
4. sidecar-only lane (fallback removed in NS9).

---

## 12) Security and trust model

1. Sidecars are executable native code; loading is explicit via lockfile.
2. Require checksum verification before dynamic loading.
3. Do not auto-execute package scripts to build sidecars in v1 runtime path.
4. Treat ABI mismatch as hard error, not warning.
5. Integrate with future signing/registry work (packaging S8).

---

## 13) Risks and mitigations

1. **ABI drift across toolchains**
   - Mitigation: strict ABI versioning and CI conformance tests.
2. **Platform packaging complexity (DLL/SO deps)**
   - Mitigation: per-target artifact metadata and bundle validation scripts.
3. **Determinism regressions**
   - Mitigation: extension hash + lockfile-ordered load and test fixtures.
4. **Migration breakage for existing modules**
   - Mitigation: staged fallback mode and per-module parity gates.
5. **Debugging complexity**
   - Mitigation: `eta doctor` native diagnostics and explicit missing-extension errors.
6. **Workspace behavior regressions**
   - Mitigation: preserve existing discovery/selection/materialization rules and
     gate every stage with workspace-specific CLI/tool tests.

---

## 14) Acceptance criteria

Native sidecar rollout is complete when:

1. Core tools run without hard-linked first-party native builtin libraries.
2. `std.torch`, `std.net`/nng, `std.stats`, `std.log` load through package-managed sidecars.
3. `.etac` freshness validates extension environment hash, not sidecar-driven builtin count changes.
4. Packaging workflows (`eta add/build/run/test/vendor/install`) handle sidecars deterministically
   in standalone and workspace modes.
5. CI passes sidecar-only lanes on supported platforms.
6. Existing workspace selection/materialization behavior is unchanged for
   non-sidecar projects.
7. Docs include sidecar authoring, loading model, and migration notes.
