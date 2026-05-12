# `eta::session::Driver` refactor plan

Date: 2026-05-11

Target file reviewed: `eta/session/src/eta/session/driver.h`

Current state observed in the workspace: `driver.h` is still a large header-only orchestrator of roughly 3.5k lines. It owns runtime state, compilation state, REPL UX, `.etac` loading, package sidecar loading, native sidecar synchronization, NNG mailbox/process/thread-worker plumbing, debug file IDs, eval, display classification, and several utility functions that duplicate code elsewhere.

The refactor goal is not just “smaller files”. The goal is to make `Driver` the composition root for a session, with each domain living behind a narrow interface, and to remove code that already belongs in `eta/core`, shared `eta/session` utilities, or native sidecar packages such as `eta-nng`.

## Desired end state

`Driver` should become a small facade that wires together these components:

| Component | Proposed location | Responsibility |
|---|---|---|
| `Driver` | `eta/session/src/eta/session/driver.h/.cpp` | Public session API and dependency wiring only. Owns `Heap`, `InternTable`, `VM`, builtin/extension environments, diagnostics, and component instances. |
| `SourceFileRegistry` | `eta/session/src/eta/session/source_file_registry.h/.cpp` | File-id allocation, canonical path keys, `DiagnosticEngine` file resolver, valid breakpoint lines. |
| `RuntimePrimitiveInstaller` / `PrimitiveBootstrap` | `eta/session/src/eta/session/runtime_primitives.h/.cpp` or `eta/core/src/eta/runtime/primitive_bootstrap.*` | Install core + extension primitive objects into VM globals and record primitive debug names. |
| `CompilationSession` | `eta/session/src/eta/session/compilation_session.h/.cpp` | Incremental source compilation state: accumulated forms, executed modules, loaded files, runtime export maps, compiled-link replay, import auto-loading, source compile metadata. |
| `EtacLoader` | `eta/session/src/eta/session/etac_loader.h/.cpp` | `.etac` deserialization, freshness checks, embedded prelude load, relocation, compiled module execution. |
| `EvalEngine` | `eta/session/src/eta/session/eval_engine.h/.cpp` | Runtime `eval` builtin implementation, lexical binding capture, generated eval modules, eval invocation. |
| `ReplController` | `eta/session/src/eta/session/repl_controller.h/.cpp` | `eval_string`, `eval_to_display`, completions, hover, REPL history modules. |
| `DisplayClassifier` | `eta/session/src/eta/session/display_classifier.h/.cpp` | Jupyter MIME unpacking and display tag classification. |
| `NativeSidecarManager` | likely `eta/core/src/eta/native/sidecar_manager.h/.cpp` | Package/workspace sidecar discovery, bundled sidecar discovery, sidecar load sequencing, extension registry synchronization. |
| `NngSessionRuntime` / NNG sidecar adapter | `eta/nng` sidecar-facing code, not `driver.h` | Mailbox socket creation, actor process manager, `spawn-thread` worker factories, NNG primitive placeholder package metadata. |
| REPL parsing utilities | `eta/session/src/eta/session/repl_input.h/.cpp` or `eta/core/src/eta/repl/input.h/.cpp` | Shared `split_toplevel_forms` and complete-input scanning used by both CLI REPL and session frontends. |
| Module/path utilities | `eta/core/src/eta/interpreter/module_path.h` and/or `eta/core/src/eta/util/path.*` | Canonical path keys, module discovery, executable path helpers, manifest lookup. |

The final include graph should avoid `driver.h` including NNG headers directly. NNG-specific code should be reached through a small session/native interface or through sidecar runtime binding.

## Current responsibility map in `driver.h`

The current file contains these separable domains:

1. **Runtime composition and lifecycle**
   - `Heap`, `InternTable`, `VM`, `BuiltinEnvironment`, `ExtensionEnvironment`, `BytecodeFunctionRegistry`, diagnostics, GC root callback, output/error ports, interrupt, destructor log shutdown.

2. **Primitive registration/bootstrap**
   - `runtime::register_builtin_names`, `register_all_primitives`, NNG placeholder registration, `install_runtime_primitives`, `record_primitive_names`, `builtins_installed_`, extension primitive registration.

3. **File/source registry**
   - `ensure_file_id`, `file_id_for_path`, `path_for_file_id`, `file_resolver`, `valid_lines_for`, `canon_path_key`, `next_file_id_`, `file_id_to_path_`, `path_to_file_id_`.

4. **Source compilation session**
   - `run_file`, `compile_file`, `run_source`, `run_source_impl`, `auto_load_imports`, `clear_module_cache`, `hydrate_executed_module_source`, `append_etac_global_reservation`, module export maps, executed/loaded/indexed sets, global-name metadata.

5. **`.etac` loading/execution**
   - `run_etac_file`, `try_load_embedded_prelude`, `execute_deserialized_etac`, global-slot relocation, freshness hashing, compiled-link export replay.

6. **REPL/notebook UX**
   - `eval_string`, `eval_to_display`, `completions_at`, `hover_at`, `is_complete_expression`, `split_toplevel_forms`, `repl_counter_`, `repl_modules_`.

7. **Runtime `eval` builtin**
   - Constructor lambda installed with `builtins_.overwrite_func("eval", ...)`, `compile_eval_lambda`, `invoke_eval_lambda`, lexical binding capture, synthetic-name filtering, `eval_counter_`, `active_module_init_stack_`.

8. **Native sidecar loading**
   - Package/lockfile discovery, bundled sidecar scanning, target-triple selection, sidecar spec construction, sidecar artifact resolving/loading, registry-to-extension synchronization.

9. **NNG actor/mailbox runtime**
   - Direct NNG includes, `ProcessManager`, `install_mailbox`, mailbox value, `install_actor_worker_factories`, spawn-thread child `Driver` creation, spawn closure deserialization.

10. **Miscellaneous utilities**
	- Heap env var parsing, executable path detection, host target triple, manifest lookup, diagnostics string formatting, display MIME unpacking.

## Duplicated or misplaced logic to remove

These are the main duplications/misplacements found during review:

| In `driver.h` | Existing/better home | Refactor direction |
|---|---|---|
| `canon_path_key` | `ModulePathResolver::canonical_key` and `native::sidecar_loader.cpp::path_key` use similar canonical/lowercase behavior | Expose one shared path utility from `eta/core/src/eta/util/path.*`, or make resolver path-key public and reuse it. |
| `find_nearest_manifest_path` | `ModulePathResolver::find_manifest_path` and `package::discover_manifest_context` overlap | Prefer `package::discover_manifest_context` for package-aware lookup; expose a small manifest helper if raw parent walking is still needed. |
| `detect_etai_path` | `ModulePathResolver::bundled_stdlib_dir`, `main_eta.cpp`, `main_etai.cpp` all inspect executable path | Add `eta::util::current_executable_path()` / `sibling_executable(name)` in core util and reuse everywhere. |
| `discover_module_names` | Same filesystem traversal concept as `ModulePathResolver` | Add `ModulePathResolver::discover_module_names()` so REPL/LSP/session share module discovery. |
| `split_toplevel_forms` | Duplicated in `tools/interpreter/main_repl.cpp` | Move to an `eta/session` or `eta/core` shared REPL input utility; both CLI REPL and `ReplController` call it. |
| `is_complete_expression` | `main_repl.cpp::is_balanced` is a weaker duplicate | Move the stronger scanner to the same shared REPL input utility and replace `is_balanced`. |
| `collect_imported_modules`, `collect_declared_module_names`, `form_declares_module` | `reader::ModuleLinker` already parses module/import forms internally | Expose read-only module-form scanning helpers from `eta/reader` to avoid session-owned AST parsing rules. |
| `compute_extension_env_hash` | Reads only `runtime::ExtensionEnvironment::specs()` | Move to `ExtensionEnvironment::fingerprint()` or `hash()` in `eta/core/src/eta/runtime/extension_env.h`. |
| Sidecar target-triple and lockfile/native metadata helpers | `eta/native/sidecar_loader.*`, `eta/package/*`, tests duplicate host triple too | Move host target and native sidecar selection into `eta/native` or `eta/package`. |
| `ensure_package_sidecars_loaded` and bundled sidecar loading | `native::build_native_load_context` and `resolve_native_sidecars` already exist | Create `NativeSidecarManager` in core/native and remove package-loading orchestration from session `Driver`. |
| NNG primitive names and missing-sidecar placeholders | `all_primitives.h` already has torch/stats/log sidecar placeholder machinery | Generalize sidecar placeholder registration metadata; do not special-case NNG in `Driver`. |
| `install_mailbox`, NNG sockets, actor worker factories | Belongs to NNG runtime/sidecar package | Move behind `eta-nng` sidecar/session adapter. `Driver` should not include `<nng/...>` or `eta/nng/...` headers. |
| Child spawn capture runtime setup | Currently private primitive/global install logic is reached from NNG worker lambdas | Provide a small public `DriverChildRuntime`/`SessionRuntimeHost` API or move child execution into an NNG-owned adapter that depends on a `DriverFactory`. |
| `parse_heap_env_var` | Used by `main_repl.cpp`, `main_etai.cpp`, test runner | Move to `eta/session/runtime_config.h` or `eta/core/util/env.h`. Keep a compatibility wrapper during migration. |

## Refactor rules

Apply these rules in every stage:

1. **No behavior changes without tests.** Each stage must compile and run a relevant test slice before continuing.
2. **Keep `Driver` API stable until the last stage.** External tools include `eta/session/driver.h`; add forwarding methods before removing anything.
3. **Prefer extraction over redesign first.** Move code behind a class with the same behavior, then simplify dependencies in a follow-up commit.
4. **Do not introduce callback soup.** If a new class needs many callbacks, that is a sign a proper host/context interface or owned state object is missing.
5. **Move duplicated logic to the lowest sensible layer.** Shared path/package/runtime code belongs in `eta/core`; shared REPL text handling belongs in `eta/session` or `eta/core` (not tool-specific binaries); NNG belongs in NNG sidecar/runtime code.
6. **Remove direct sidecar-specific includes from `driver.h`.** The session layer may know about a generic native sidecar mechanism, but should not be hardwired to one sidecar package.

## Baseline commands

Use the actual configured build directory for the machine. The examples below assume `build` under the repository root.

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
ctest --test-dir C:\Users\lewis\develop\eta\build -C Debug --output-on-failure -R eta_core_test
```

Useful narrower runs once `eta_core_test` is built:

```powershell
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=module_path_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=native_sidecar_loader_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=repl_redefine_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=bytecode_serializer_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=driver_jupyter_test
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=nng_tests
```

Adjust executable paths for Release or for a different generator layout.

## Stage 0: Characterization and safety net

**Goal:** Lock down current behavior before moving code.

**Changes:**

- Add/expand focused tests if any behavior is under-tested:
  - `SourceFileRegistry`-equivalent path canonicalization behavior on Windows-style/case-varied paths.
  - REPL input splitting and complete-expression scanning, including strings, line comments, nested block comments, bare atoms, dotted continuations.
  - `.etac` freshness fallback and embedded prelude fallback behavior.
  - Sidecar load order and package/workspace lockfile closure selection.
  - NNG mailbox/spawn-thread smoke tests remain green before extraction.
- Record current public `Driver` API consumers: CLI `etai`, `eta_repl`, compiler `etac`, DAP, LSP, Jupyter, tests.

### Stage 0 consumer snapshot (2026-05-11)

Direct includes/usages of `eta/session/driver.h` in the current tree:

- Interpreter tools:
  - `eta/tools/interpreter/src/eta/interpreter/main_etai.cpp`
  - `eta/tools/interpreter/src/eta/interpreter/main_repl.cpp`
- Compiler tool:
  - `eta/tools/compiler/src/eta/compiler/main_etac.cpp`
- Debug/notebook tooling:
  - `eta/tools/dap/src/eta/dap/dap_server.cpp`
  - `eta/tools/jupyter/src/eta/jupyter/eta_interpreter.h`
  - `eta/tools/jupyter/src/eta/jupyter/display.cpp`
  - `eta/tools/jupyter/src/eta/jupyter/comm/*.cpp`
- Test runner:
  - `eta/tools/test_runner/src/main_test_runner.cpp`
- C++ test suites (non-exhaustive high-signal files):
  - `eta/qa/test/src/driver_jupyter_test.cpp`
  - `eta/qa/test/src/packaging_contract_tests.cpp`
  - `eta/qa/test/src/repl_redefine_tests.cpp`
  - `eta/qa/test/src/dap_tests.cpp`
  - `eta/qa/test/src/nng_tests.cpp`
  - `eta/qa/test/src/cookbook/*.cpp`

Notes:

- `eta/tools/lsp/src/eta/lsp/lsp_server.cpp` currently does not include `driver.h` directly.
- `eta/cli/src/eta/cli/main_eta.cpp` does not directly construct `Driver`; it delegates to tool binaries.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
ctest --test-dir C:\Users\lewis\develop\eta\build -C Debug --output-on-failure -R eta_core_test
```

**Acceptance criteria:**

- Baseline is known: either all tests pass, or existing failures are documented before refactoring starts.
- No production code moved yet.

## Stage 1: Move shared path/environment utilities out of `Driver`

**Goal:** Remove the easiest duplication first and reduce future extraction friction.

**Changes:**

- Add `eta/core/src/eta/util/path.h/.cpp` or equivalent with:
  - `canonicalize_path(fs::path)`
  - `canonical_path_key(fs::path)`
  - `current_executable_path()`
  - `sibling_executable_path(std::string_view basename)`
- Update `ModulePathResolver`, `native::sidecar_loader.cpp`, CLI executable path code, and `Driver` to reuse the shared utilities.
- Expose package-aware manifest lookup either through `package::discover_manifest_context` helper or a small public resolver function. Remove raw parent-walking copies from `Driver` where possible.
- Move `Driver::parse_heap_env_var` to `eta/session/runtime_config.h` or `eta/core/util/env.h`; keep `Driver::parse_heap_env_var` as a deprecated forwarding wrapper for one release/transition.

**Files likely touched:**

- `eta/core/src/eta/util/path.h/.cpp`
- `eta/core/src/eta/interpreter/module_path.h`
- `eta/core/src/eta/native/sidecar_loader.cpp`
- `eta/session/src/eta/session/driver.h`
- `eta/tools/interpreter/src/eta/interpreter/main_etai.cpp`
- `eta/tools/interpreter/src/eta/interpreter/main_repl.cpp`
- `eta/cli/src/eta/cli/main_eta.cpp`

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=module_path_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=native_sidecar_loader_tests
```

**Acceptance criteria:**

- `Driver` no longer owns `canon_path_key`, `canonicalize_runtime_path`, or executable path probing.
- Shared path behavior remains stable on Windows and POSIX.

### Stage 1 status (2026-05-12)

- Added shared path helpers in `eta/core/src/eta/util/path.h/.cpp`.
- Updated `Driver`, `ModulePathResolver`, native sidecar loader, and `eta` CLI path handling to use shared utilities.
- Added package helper `package::find_nearest_manifest_path(...)` and switched `Driver` manifest freshness lookup to it.
- Moved heap env parsing into `eta/session/runtime_config.h` and kept `Driver::parse_heap_env_var(...)` as a forwarding wrapper.
- Added focused tests in `eta/qa/test/src/path_util_tests.cpp` for shared path helpers and heap env parsing.

## Stage 2: Extract `SourceFileRegistry`

**Goal:** Separate debugger/diagnostic file ID bookkeeping from compilation/runtime state.

**Changes:**

- Create `eta/session/src/eta/session/source_file_registry.h/.cpp`.
- Move:
  - `next_file_id_`
  - `file_id_to_path_`
  - `path_to_file_id_`
  - `ensure_file_id`
  - `file_id_for_path`
  - `path_for_file_id`
  - `file_resolver`
  - `valid_lines_for`
  - `allocate_file_id`
- `valid_lines_for` should accept a `BytecodeFunctionRegistry` reference or callback, so the registry does not own VM/compiler state.
- Keep forwarding methods on `Driver` to preserve API.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=diagnostic_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=dap_tests
```

**Acceptance criteria:**

- `Driver` owns `SourceFileRegistry source_files_;` and delegates.
- No file-id maps remain in `Driver`.

### Stage 2 status (2026-05-12)

- Added `eta/session/src/eta/session/source_file_registry.h/.cpp`.
- Moved file-id bookkeeping and helpers (`ensure_file_id`, `file_id_for_path`, `path_for_file_id`, `file_resolver`, `valid_lines_for`, `allocate_file_id`) into `SourceFileRegistry`.
- Updated `Driver` to delegate through `source_files_` while preserving the existing public API.
- Added focused unit coverage in `eta/qa/test/src/source_file_registry_tests.cpp`.

## Stage 3: Share REPL input utilities and extract `ReplController`

**Goal:** Remove REPL/notebook presentation logic from `Driver` and de-duplicate CLI REPL parsing.

**Changes:**

- Move the stronger `Driver::is_complete_expression` scanner into `eta/session/src/eta/session/repl_input.h/.cpp` (or `eta/core` shared REPL input utility) as `is_complete_repl_input`.
- Move `Driver::split_toplevel_forms` into the same shared REPL utility.
- Replace `main_repl.cpp::is_balanced` and `main_repl.cpp::split_toplevel_forms` with calls to those shared functions.
- Add `eta/session/src/eta/session/repl_controller.h/.cpp`.
- Define a narrow `ReplRuntime` interface with:
  - `global_names()`
  - `run_source(...)`
  - `has_module(...)`
  - `diagnostics_to_string()`
  - `format_value(...)`
- Move from `Driver`:
  - `eval_string`
  - `eval_to_display`
  - `completions_at`
  - `hover_at`
  - `repl_counter_`
  - `repl_modules_`
  - display classification only if `DisplayClassifier` is not extracted first.
- Keep forwarding methods on `Driver`.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=repl_redefine_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=driver_jupyter_test
```

**Acceptance criteria:**

- `Driver` has no REPL text scanning implementation.
- CLI REPL and session REPL use the same splitter/completeness logic.
- `Driver` contains no `repl_counter_` or `repl_modules_`.

### Stage 3 status (2026-05-12)

- Added shared REPL input helpers in `eta/session/src/eta/session/repl_input.h/.cpp` with `is_complete_repl_input(...)` and `split_toplevel_forms(...)`.
- Added `eta/session/src/eta/session/repl_controller.h/.cpp` and moved `eval_string`, `eval_to_display`, `completions_at`, `hover_at`, `repl_counter_`, and REPL module replay state into `ReplController`.
- Updated `Driver` to delegate REPL-facing APIs through `repl_controller_` while preserving the public API surface.
- Updated `eta/tools/interpreter/src/eta/interpreter/main_repl.cpp` to use shared REPL input helpers instead of local duplicate scanners.
- Added focused unit coverage in `eta/qa/test/src/repl_input_tests.cpp`.

## Stage 4: Extract display classification

**Goal:** Keep notebook display concerns independent from `Driver` and reusable by Jupyter-specific code.

**Changes:**

- Create `eta/session/src/eta/session/display_classifier.h/.cpp`.
- Move:
  - `classify_display_tag`
  - `display_tag_for_mime`
  - `try_decode_string`
  - `try_unpack_jupyter_display`
- `DisplayClassifier` depends on `Heap` and `InternTable` only.
- `ReplController::eval_to_display` uses `DisplayClassifier`.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=driver_jupyter_test
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=jupyter_magics_test
```

**Acceptance criteria:**

- Display MIME/vector unpacking is not in `Driver`.
- `DisplayClassifier` is easy to unit test without constructing a full `Driver`.

### Stage 4 status (2026-05-12)

- Added `eta/session/src/eta/session/display_classifier.h/.cpp` and moved `classify_display_tag`, `display_tag_for_mime`, `try_decode_string`, and `try_unpack_jupyter_display` into `DisplayClassifier`.
- Updated `eta/session/src/eta/session/repl_controller.h/.cpp` so `eval_to_display` uses `DisplayClassifier`.
- Updated `eta/session/src/eta/session/driver.h` to own `display_classifier_`, pass it into `ReplController`, and remove display MIME/vector unpacking helpers from `Driver`.
- Added focused unit coverage in `eta/qa/test/src/display_classifier_tests.cpp`.

## Stage 5: Move extension fingerprinting and primitive bootstrap out of `Driver`

**Goal:** Make primitive slot installation and extension hashing first-class runtime concepts.

**Changes:**

- Add `ExtensionEnvironment::fingerprint()` in `eta/core/src/eta/runtime/extension_env.h`.
- Replace `Driver::compute_extension_env_hash()` with `extensions_.fingerprint()`.
- Create `RuntimePrimitiveInstaller` or `PrimitiveBootstrap` with:
  - `total_primitive_count()`
  - `install_into(globals, total_globals)`
  - `record_names(global_names)`
  - optional `invalidate()` when extensions change.
- Move `install_runtime_primitives`, `record_primitive_names`, `builtins_installed_` logic behind this component.
- Keep `Driver::extension_env_hash()` and `Driver::builtin_count()` forwarding for compiler/serializer callers.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=builtin_sync_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=bytecode_serializer_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=eval_tests
```

**Acceptance criteria:**

- `Driver` no longer contains FNV/hash code.
- Primitive install/reinstall behavior is centralized and no longer copied in `.eta`, legacy `.etac`, V5 `.etac`, and spawn-thread code paths.

### Stage 5 status (2026-05-12)

- Added `ExtensionEnvironment::fingerprint()` in `eta/core/src/eta/runtime/extension_env.h`.
- Added `eta/session/src/eta/session/runtime_primitives.h/.cpp` with `RuntimePrimitiveInstaller`.
- Updated `Driver` to delegate primitive install/name recording/invalidation through `RuntimePrimitiveInstaller`.
- Replaced `Driver` extension hash computation with `extensions_.fingerprint()` while preserving `builtin_count()` and `extension_env_hash()` APIs.
- Added focused unit coverage in `eta/qa/test/src/runtime_primitives_tests.cpp`.

## Stage 6: Extract `CompilationSession`

**Goal:** Move incremental compiler/linker/session state into a coherent owner.

**Changes:**

- Create `eta/session/src/eta/session/compilation_session.h/.cpp`.
- Move state:
  - `accumulated_forms_`
  - `executed_modules_`
  - `loaded_files_`
  - `indexed_source_files_`
  - `prelude_origin_path_` if kept as compile/load state
  - `loading_modules_`
  - `global_names_`
  - `runtime_module_info_`
  - `compiled_link_modules_`
  - `etac_reserve_counter_`
  - `etac_module_reservations_`
  - `active_module_init_stack_`
- Move methods:
  - `run_source_impl`
  - `auto_load_imports`
  - `clear_module_cache`
  - `hydrate_executed_module_source`
  - `append_etac_global_reservation`
  - `release_etac_global_reservation`
  - `drop_etac_reservation_modules`
  - runtime/compiled export recording helpers
- Introduce `CompilationSession::Host` instead of per-call lambdas:
  - `resolve_import_path(module, shadow_conflict)`
  - `run_module_file(path)`
  - `emit_link_error`
  - `emit_runtime_error`
  - accessors for `VM`, `Heap`, `InternTable`, `BytecodeFunctionRegistry`, builtin/extension envs, primitive installer, diagnostics, optimization pipeline.
- Move AST scanning helpers to `eta/reader` first if possible, then `CompilationSession` uses them.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=functional_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=module_linker_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=semantics_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=emitter_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=repl_redefine_tests
```

**Acceptance criteria:**

- `CompilationSession` owns its state. Do not create a `SessionState` bag in `Driver` and pass references back into the session.
- `Driver::run_source`, `run_file`, `compile_file`, and `clear_module_cache` are thin delegates.
- `global_names()` delegates to session-owned global-name metadata.

### Stage 6 status (2026-05-12)

- Added `eta/session/src/eta/session/compilation_session.h/.cpp` with `CompilationSession` and `CompilationSession::Host`.
- Moved incremental compilation/link state and behavior out of `Driver`, including `run_source_impl`, import auto-loading, module cache clearing, source hydration, and `.etac` reservation bookkeeping.
- Updated `Driver` to implement the host interface and delegate `run_source`, `run_file`, `compile_file`, `clear_module_cache`, and global-name state to `CompilationSession`.
- Added focused unit coverage in `eta/qa/test/src/compilation_session_tests.cpp`.

## Stage 7: Extract `EtacLoader`

**Goal:** Make compiled artifact loading a cohesive subsystem and remove relocation/freshness code from `Driver`.

**Changes:**

- Create `eta/session/src/eta/session/etac_loader.h/.cpp`.
- Move:
  - `run_etac_file`
  - `try_load_embedded_prelude`
  - `embedded_prelude_marker_path`
  - `execute_deserialized_etac`
  - `relocate_function_global_slots`
  - `hash_file_for_etac_freshness`
  - `.etac` freshness context creation
- `EtacLoader` depends on `CompilationSession` for module execution state, compiled-link export recording, reservations, runtime export slots, and global names.
- `EtacLoader::Host` should provide only operations that truly cross subsystem boundaries:
  - package sidecars loaded for artifact directory
  - import path resolution
  - source fallback execution
  - diagnostics/runtime error emission
  - access to VM/registry/serializer primitives.
- Keep legacy V3/V4 behavior and V5 relocation behavior byte-for-byte equivalent initially.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=bytecode_serializer_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=cookbook/compiled_example_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=driver_jupyter_test
```

**Acceptance criteria:**

- No `.etac` deserialization, freshness, or relocation code remains in `Driver`.
- `load_prelude()` delegates embedded/artifact loading to `EtacLoader`.

### Stage 7 status (2026-05-12)

- Added `eta/session/src/eta/session/etac_loader.h/.cpp` with `EtacLoader` and `EtacLoader::Host`.
- Moved `.etac` deserialization, freshness checks, embedded prelude loading, and relocation execution logic out of `Driver` and into `EtacLoader`.
- Updated `Driver` to implement `EtacLoader::Host`, delegate `run_etac_file()` and embedded prelude loading through `etac_loader_`, and remove in-class `.etac` helpers.
- Added focused unit coverage in `eta/qa/test/src/etac_loader_tests.cpp`.

## Stage 8: Extract `EvalEngine`

**Goal:** Move runtime `eval` out of constructor and make eval module generation/testability explicit.

**Changes:**

- Create `eta/session/src/eta/session/eval_engine.h/.cpp`.
- Move:
  - `EvalBinding`
  - `compile_eval_lambda`
  - `invoke_eval_lambda`
  - `collect_eval_lexical_bindings`
  - `is_synthetic_eval_binding_name`
  - `eval_counter_`
- `EvalEngine` depends on `CompilationSession` and `VM`.
- Replace constructor inline lambda with a small call:
  - `eval_engine_.install_builtin(builtins_)`, or
  - `builtins_.overwrite_func("eval", eval_engine_.make_builtin())`.
- `CompilationSession` exposes active-module-init guard/state to `EvalEngine` through a narrow method, not by sharing raw vectors.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=eval_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=higher_order_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=functional_tests
```

**Acceptance criteria:**

- Constructor no longer contains a 40+ line `eval` builtin body.
- Eval state is owned by `EvalEngine`.

### Stage 8 status (2026-05-12)

- Added `eta/session/src/eta/session/eval_engine.h/.cpp` with `EvalEngine` and `EvalEngine::Host`.
- Moved runtime `eval` builtin behavior out of `Driver` into `EvalEngine`, including lexical binding capture, eval lambda compile/invoke, synthetic binding filtering, and eval module counters.
- Updated `Driver` to construct `eval_engine_` and install the `eval` builtin via `eval_engine_.install_builtin(builtins_)`.
- Added a narrow `CompilationSession` active-module execution guard API used by `EvalEngine` during eval compilation.
- Added focused unit coverage in `eta/qa/test/src/eval_engine_tests.cpp`.

## Stage 9: Move package/native sidecar orchestration into `eta::native`

**Goal:** Remove package sidecar discovery/loading from `Driver` and de-duplicate existing native loader utilities.

**Changes:**

- Create `eta/core/src/eta/native/sidecar_manager.h/.cpp`.
- Move or redesign from `Driver`:
  - `ensure_package_sidecars_loaded`
  - `ensure_bundled_sidecars_loaded`
  - `sync_sidecar_extensions_into_environment`
  - `make_registered_sidecar_primitive`
  - sidecar manifest key and loaded counters
  - bundled sidecar package list handling
  - host target triple selection
  - lockfile/native metadata helpers
- Reuse and possibly extend existing:
  - `native::build_native_load_context`
  - `native::resolve_native_sidecars`
  - `package::discover_manifest_context`
  - `package::read_lockfile`
  - `package::read_manifest`
- Introduce a `NativeSidecarHost` interface for session-specific effects:
  - register/overwrite primitive function
  - check whether extension primitives can still be registered
  - invalidate primitive installer
  - emit diagnostic text.
- Consider moving builtin sidecar package metadata into `builtin_metadata` or a new `sidecar_builtin_metadata` table:
  - primitive name/prefix
  - sidecar package name
  - extension id if known
  - missing-sidecar error behavior.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=native_sidecar_loader_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=packaging_contract_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=driver_jupyter_test
```

**Acceptance criteria:**

- `Driver` no longer reads lockfiles/manifests directly for sidecar loading.
- Native sidecar discovery/loading is testable without a full session driver.
- `Driver::load_package_sidecars` remains as a thin forwarding API.

### Stage 9 status (2026-05-12)

- Added `eta/core/src/eta/native/sidecar_manager.h/.cpp` with `NativeSidecarManager` and a host callback interface for session-owned primitive registration/diagnostics effects.
- Moved package and bundled sidecar orchestration from `Driver` into `NativeSidecarManager`, including lockfile closure selection, bundled sidecar discovery, sidecar loader sequencing, and extension synchronization.
- Updated `Driver` to keep `load_package_sidecars(...)` as a thin forwarding API and delegate all sidecar orchestration through `sidecar_manager_`.
- Added focused unit coverage in `eta/qa/test/src/native_sidecar_manager_tests.cpp`.

## Stage 10: Isolate then move NNG functionality to NNG-owned code

**Goal:** Remove NNG-specific runtime details from `Driver` while preserving actor/spawn behavior.

This should be done in two sub-stages to reduce risk.

### Stage 10A: Session-local extraction with no behavior change

**Changes:**

- Create `eta/session/src/eta/session/actor_thread_factory.h/.cpp` or `eta/session/src/eta/session/nng_session_runtime.h/.cpp`.
- Move from `Driver`:
  - `ActorEvent`
  - `on_actor_lifecycle` adapter logic
  - `install_actor_worker_factories`
  - child `Driver` spawn/bootstrap logic
  - closure spawn capture setup
- Keep `install_mailbox` and `process_manager()` temporarily forwarding through this component.
- Replace direct lambdas in `Driver` with `actor_runtime_.install_worker_factories(...)`.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=nng_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=dap_tests
```

**Acceptance criteria:**

- NNG worker lambdas are no longer in `driver.h`.
- Behavior is unchanged; `Driver` may still include NNG headers at this sub-stage.

### Stage 10A status (2026-05-12)

- Added `eta/session/src/eta/session/nng_session_runtime.h/.cpp` with `NngSessionRuntime`.
- Moved actor lifecycle adaptation, mailbox installation, worker factory installation, child session bootstrap, and spawn-thread capture hydration from `Driver` into `NngSessionRuntime`.
- Updated `Driver` to use `actor_runtime_` and keep `install_mailbox(...)`, `process_manager()`, and `on_actor_lifecycle(...)` as thin forwarding APIs.
- Added focused unit coverage in `eta/qa/test/src/nng_session_runtime_tests.cpp`.

### Stage 10B: Move NNG ownership to the NNG sidecar/runtime package

**Changes:**

- Define a generic session-side interface exposed to native sidecars, for example:
  - `SessionRuntimeBinding` with heap, intern table, VM, registry, global access, diagnostics, child-driver factory, stream sinks.
  - Or extend `SidecarRuntimeBindingV1` carefully with versioning.
- Move NNG-specific mailbox socket creation and process manager implementation behind `eta/nng` code.
- `Driver` should hold only an abstract actor/mailbox service pointer if the NNG sidecar is loaded.
- `current-mailbox`, `spawn`, `spawn-thread`, `send!`, `recv!`, etc. should be registered/overwritten by the NNG sidecar, not hard-coded in `Driver`.
- Generalize missing-sidecar placeholder registration so NNG is handled like torch/stats/log rather than with `Driver::is_nng_primitive_name`.
- Remove direct includes from `driver.h`:
  - `<nng/nng.h>`
  - `<nng/protocol/pair0/pair.h>`
  - `<eta/nng/nng_socket_ptr.h>`
  - `<eta/nng/nng_factory.h>`
  - `<eta/nng/process_mgr.h>`
  - `<eta/nng/spawn_capture_format.h>`

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=nng_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=builtin_sync_tests
C:\Users\lewis\develop\eta\build\eta\qa\test\Debug\eta_core_test.exe --run_test=dap_tests
```

**Acceptance criteria:**

- `driver.h` has no direct dependency on NNG headers or concrete `eta::nng::ProcessManager`.
- NNG-specific behavior is owned by NNG code or the NNG native sidecar.
- Core/session can compile conceptually without NNG implementation details in the public session header.

### Stage 10B status (2026-05-12)

- Added generic actor runtime interfaces in `eta/core/src/eta/native/actor_runtime.h` and extended sidecar runtime bindings to expose both a typed actor manager pointer and a legacy raw process-manager handle.
- Moved session actor/mailbox runtime ownership into the NNG package at:
  - `packages/stdlib/native/nng/src/eta/nng/session_actor_runtime.h`
  - `packages/stdlib/native/nng/src/eta/nng/session_actor_runtime.cpp`
- Removed `eta/session/src/eta/session/nng_session_runtime.h/.cpp` and updated `Driver` to depend only on the abstract actor runtime interface plus the NNG factory (`eta::nng::make_session_actor_runtime()`).
- Removed direct NNG includes and concrete `ProcessManager` return types from `eta/session/src/eta/session/driver.h`; `Driver::process_manager()` now returns the abstract `eta::native::ActorProcessManager*`.
- Generalized sidecar placeholder ownership routing via `runtime::builtin_native_sidecar_package(...)`, then updated `register_all_primitives(...)` so NNG placeholders are installed through the same sidecar placeholder path as other native sidecar domains.
- Added/updated focused coverage:
  - `eta/qa/test/src/nng_session_runtime_tests.cpp` now validates the NNG package-owned session actor runtime class.
  - `eta/qa/test/src/builtin_sync_tests.cpp` verifies NNG placeholder registration through `register_all_primitives(...)`.
  - `eta/qa/test/src/native_sidecar_manager_tests.cpp` verifies sidecar package mapping for builtin symbols.

## Stage 11: Clean public `Driver` facade and move implementation to `.cpp`

**Goal:** Finish the design by making `driver.h` a public API header, not a source file in disguise.

**Changes:**

- Move method bodies from `driver.h` to `driver.cpp` except small trivial accessors.
- Use PIMPL or private component members to keep heavy includes out of the public header.
- Public `driver.h` should include minimal forward declarations where possible.
- Remove transitional wrappers if all call sites were migrated.
- Revisit ownership:
  - Prefer value members for non-optional components.
  - Use `unique_ptr` only for incomplete types, optional components, or lifetime constraints.
  - Avoid `SessionState`-style raw state bags outside the component that owns the behavior.

**Validation:**

```powershell
cmake --build C:\Users\lewis\develop\eta\build --target eta_core_test --config Debug
ctest --test-dir C:\Users\lewis\develop\eta\build -C Debug --output-on-failure -R eta_core_test
```

**Acceptance criteria:**

- `driver.h` is primarily declarations and documentation.
- Compile dependencies from including `eta/session/driver.h` are substantially reduced.
- Ownership boundaries are explicit and components can be tested independently.

### Stage 11 status (2026-05-12)

- Added `eta/session/src/eta/session/driver.cpp` and moved non-trivial `Driver` method bodies out of the public header.
- Reduced `eta/session/src/eta/session/driver.h` implementation dependencies by removing compile-time-only includes from the public facade.
- Kept `Driver` as an explicit composition root over owned components while retaining existing public API entry points.
- Added focused facade coverage in `eta/qa/test/src/driver_facade_tests.cpp`.

## Suggested implementation order summary

The safest order is:

1. Stage 0: tests and baseline.
2. Stage 1: common path/env utilities.
3. Stage 2: `SourceFileRegistry`.
4. Stage 3: shared REPL utilities + `ReplController`.
5. Stage 4: `DisplayClassifier`.
6. Stage 5: extension hash + primitive bootstrap.
7. Stage 6: `CompilationSession`.
8. Stage 7: `EtacLoader`.
9. Stage 8: `EvalEngine`.
10. Stage 9: `NativeSidecarManager`.
11. Stage 10A/10B: NNG isolation, then NNG sidecar ownership.
12. Stage 11: final public header cleanup.
13. Stage 12: clarity and contract cleanup (`std.prelude` messaging, module-path semantics, layout token removal, `eta --version` support).

This order avoids the biggest trap: extracting `EtacLoader` before `CompilationSession` owns the module/runtime state. If `.etac` loading is extracted first, it will need many callbacks back into `Driver`, creating a fake split rather than a clean boundary.

## Non-goals for the first pass

- Do not change module semantics, import order, or `.etac` file format during extraction.
- Do not change sidecar ABI without a separate compatibility plan.
- Do not delete public `Driver` methods until all tools/tests are migrated.
- Do not convert everything to interfaces. Use interfaces only where there is a real cross-domain boundary; otherwise prefer concrete components with clear ownership.

## Design checkpoints after each stage

Before merging each stage, answer these questions:

1. Did `Driver` lose responsibilities, or were they merely hidden behind callbacks?
2. Does the extracted class own the state it mutates?
3. Is any duplicate code still present in `Driver` and another core/tool location?
4. Did public behavior stay stable under the focused tests?
5. Did the include graph improve, especially for NNG and package sidecar dependencies?

If the answer to question 1 or 2 is “no”, stop and adjust the boundary before continuing.

## Stage 12: Clarity and contract cleanup

**Goal:** Finalize user-facing behavior/documentation alignment after the major structural refactor is complete.

1. **Remove obsolete `std.prelude` references**
   - Audit diagnostics, fallback paths, and module-name checks for stale `std.prelude` mentions.
   - Keep compatibility behavior where needed, but emitted errors/messages must not instruct users to load/import `std.prelude`.
   - Rename internal wording to neutral bootstrap terminology where relevant.

2. **Define module search path behavior for package-root entries**
   - Specify the canonical module resolution algorithm for each module-path entry.
   - Clarify support for adding a top-level `packages/` directory to module paths, including how package layouts are traversed during import resolution.
   - Add focused tests for path precedence, ambiguity, and cross-platform path normalization.

3. **Remove hardcoded layout tokens such as `release` or `target`**
   - Audit executable/artifact discovery and sidecar loading for hardcoded directory names.
   - Centralize layout resolution behind a single utility/config API so runtime behavior is not tied to specific build-folder literals.

4. **Restore documented `eta --version` behavior**
   - Ensure the `eta` CLI implements `--version` as documented.
   - Add a CLI contract test that verifies the flag and output format.

**Acceptance criteria:**
- No stale `std.prelude` references appear in user-facing diagnostics.
- Module-path/package-root behavior is documented and covered by tests.
- No runtime-critical path resolution depends on hardcoded `release`/`target` tokens.
- `eta --version` is implemented and tested.

