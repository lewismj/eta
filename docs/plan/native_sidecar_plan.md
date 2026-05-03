# Native Sidecar Plan

[Back to README](../../README.md) ·
[Packaging](../packaging.md) ·
[Package Commands](../guide/packages.md) ·
[Next Steps](../next-steps.md)

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
   `eta/builtins/{torch,nng,stats,log}` and linked into tools.
2. Driver registration is centralized in:
   - `eta/tools/interpreter/src/eta/interpreter/all_primitives.h`
   - `eta/session/src/eta/session/driver.h`
3. `.etac` freshness currently validates builtin count; mismatches surface as
   `BuiltinCountMismatch`.
4. Packaging S0-S7 is shipped (`eta.toml`, `eta.lock`, resolver, vendor/install,
   tooling integration), but manifest/resolver do not yet model native sidecars.

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

1. resolve target artifact from lockfile,
2. verify file checksum before load,
3. load dynamic library,
4. run ABI handshake and metadata validation,
5. register extension symbols into extension environment,
6. keep handles alive until process exit.

## 5.3 Safety checks at load

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
2. discover package context (`eta.toml` + `eta.lock`) when present,
3. load sidecars from resolved lockfile packages in deterministic order,
4. register extension primitives,
5. compile/execute with core + extension environments.

## 6.3 Extension symbol hashing

Compute deterministic hash from:

1. lockfile package order,
2. extension id + version,
3. exported symbol descriptors `(name, arity, has_rest)`.

This hash is embedded in `.etac` metadata and validated at load.

---

## 7) Bytecode and freshness policy

## 7.1 Extend metadata (v5)

Bump bytecode format to v5 and add:

1. `core_builtin_count` (core only),
2. `extension_env_hash`,
3. optional list of required extension ids (diagnostics).

## 7.2 Freshness decisions

During `.etac` load:

1. fail/refresh on core builtin mismatch (as today),
2. fail/refresh on extension hash mismatch,
3. keep existing compiler id/source/manifest/dependency checks.

Diagnostic text should explicitly name missing/mismatched extension ids.

---

## 8) CLI and packaging updates

## 8.1 Manifest parser

Extend:

- `eta/core/src/eta/package/manifest.h/.cpp`

to parse/validate `[native]` and `[[native.targets]]`.

## 8.2 Lockfile parser/writer

Extend:

- `eta/core/src/eta/package/lockfile.h/.cpp`

for sidecar fields with deterministic ordering and stable output.

## 8.3 Resolver/materializer

Extend:

- `eta/core/src/eta/package/resolver.h/.cpp`
- vendor/install flows in CLI

to:

1. carry sidecar metadata through resolved graph,
2. select target triple,
3. materialize native artifacts into modules cache,
4. verify checksums during materialization.

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

## 9.3 Temporary compatibility flag

Add temporary build flag:

- `ETA_NATIVE_BUILTIN_FALLBACK=ON` (default ON during migration)

Behavior:

1. if sidecar load fails, optionally fall back to linked builtin registration,
2. CI lane with fallback OFF to enforce sidecar correctness,
3. remove fallback at end of migration.

---

## 10) Detailed staged roadmap

Each stage is mergeable and has explicit test gates.

### NS0 - Freeze contracts and add harness

Scope:

1. add baseline tests for primitive registration order and tool startup,
2. add fixture projects with mock sidecar metadata (no loader yet).

Gate:

1. existing runtime/package suites green,
2. new harness builds on Windows/Linux/macOS.

### NS1 - Sidecar ABI and loader skeleton

Scope:

1. add SDK header (`eta-native-v1`),
2. add loader abstraction + platform backends,
3. add mocked sidecar load tests (fake DLL/SO with test entrypoint).

Gate:

1. unit tests for load/open/symbol lookup and error mapping.

### NS2 - Extension registry in runtime

Scope:

1. add extension primitive environment,
2. install extension primitives into VM globals after core builtins,
3. semantic analyzer support for both environments.

Gate:

1. compile/execute tests with synthetic extension primitive set.

### NS3 - Manifest and lockfile schema extensions

Scope:

1. parse/validate `[native]` and target tables,
2. persist native fields in `eta.lock`,
3. deterministic read/write + validation errors.

Gate:

1. package parser tests covering malformed metadata and missing required fields.

### NS4 - Resolver and materialization for sidecars

Scope:

1. target triple selection,
2. materialize native artifacts under `.eta/modules`,
3. checksum verification at materialization and load.

Gate:

1. CLI integration tests (`eta vendor`, `eta build`, `eta run`) with fixture sidecars.

### NS5 - Bytecode v5 extension metadata

Scope:

1. add `extension_env_hash` to serializer/deserializer,
2. freshness checks and diagnostics,
3. backward-compatible read path for v4.

Gate:

1. serializer tests for mismatch cases and compatibility paths.

### NS6 - Driver/package-aware sidecar loading

Scope:

1. load sidecars from lockfile before compile/run in package context,
2. plumb into REPL/LSP/DAP/Jupyter package discovery path.

Gate:

1. package-aware tool integration tests with native fixture.

### NS7 - Migrate `log` to sidecar

Scope:

1. package `eta-log-sidecar`,
2. stdlib wrapper dependency wiring,
3. remove direct `log` registration from `register_all_primitives` when fallback OFF.

Gate:

1. `log` unit + stdlib tests pass with fallback OFF.

### NS8 - Migrate `stats`, `nng`, `torch`

Scope:

1. repeat migration pattern for remaining modules,
2. ensure platform runtime dependencies are packaged with sidecars.

Gate:

1. full stdlib + cookbook + torch/nng/stats C++ suites pass on CI matrix.

### NS9 - Remove hard links and fallback

Scope:

1. stop linking `eta_torch`, `eta_nng`, `eta_stats`, `eta_log` into tools,
2. remove fallback mode and old registration paths,
3. keep compatibility docs for one release.

Gate:

1. clean install from release bundle works via sidecars only.

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
2. **Package contract tests**:
   - lockfile native fields
   - target triple selection
3. **Runtime integration tests**:
   - compile and run with sidecar loaded
   - fail with actionable error when sidecar missing
   - extension hash mismatch fallback/rebuild
4. **Tooling tests**:
   - package-aware REPL/LSP/DAP/Jupyter with sidecars
5. **Migration parity tests**:
   - behavior parity between builtin and sidecar modes during transition.

## 11.2 CI matrix

Run sidecar scenarios on:

1. Windows x64
2. Linux x86_64
3. macOS arm64/x86_64 (as available)

with fallback ON and OFF until NS9.

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

---

## 14) Acceptance criteria

Native sidecar rollout is complete when:

1. Core tools run without hard-linked first-party native builtin libraries.
2. `std.torch`, `std.net`/nng, `std.stats`, `std.log` load through package-managed sidecars.
3. `.etac` freshness validates extension environment hash, not sidecar-driven builtin count changes.
4. Packaging workflows (`eta add/build/run/test/vendor/install`) handle sidecars deterministically.
5. CI passes sidecar-only lanes on supported platforms.
6. Docs include sidecar authoring, loading model, and migration notes.

