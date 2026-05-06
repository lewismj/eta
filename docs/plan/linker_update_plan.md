# Linker Update Plan: Correct `.etac`-Only Linking and Slot Binding

## Background

Today, importing a module that was loaded from `.etac` depends on hydrating a
sibling `.eta` file in `Driver::hydrate_executed_module_source`:

```cpp
if (source_path.extension() == ".etac") {
    auto sibling_source = source_path;
    sibling_source.replace_extension(".eta");
    std::error_code ec;
    if (!fs::is_regular_file(sibling_source, ec) || ec) {
        return true; // no-op
    }
    source_path = sibling_source;
}
```

When only `.etac` exists, hydration no-ops. Then source compilation/linking
later fails because the compile-time linker does not know that module.

## Why the previous plan was incomplete

Two architecture constraints must be handled explicitly:

1. `ModuleLinker` is rebuilt from scratch on every `run_source_impl` pass.
   `index_modules(...)` clears internal state each time.
2. Semantic import slot assignment currently prefers slots from modules analyzed
   from source forms in the current pass. If an imported provider exists only
   as `.etac`, semantic analysis can fall back to allocating a fresh slot,
   which is wrong even if linker validation passes.

So this cannot be fixed by a one-off hydrate call alone. We need a persistent
compiled-module metadata path that is replayed on each compile pass, plus
slot-aware semantic fallback.

## Goal

Support true `.etac`-only distributions:

- Source modules can import compiled-only modules with no sibling `.eta`.
- Link-time module/name validation works.
- Imported names bind to the correct runtime global slots.
- Existing source-first developer flow remains unchanged.

## Non-Goals

- No `.etac` format change (keep format v5 as-is).
- No reconstruction of full source import clause shapes for compiled modules
  (rename/prefix/except used inside compiled modules are not needed for
  downstream linking correctness).

## Design Overview

### 1. Persist compiled module metadata in `Driver`

Add a driver-owned cache populated from executed `.etac` modules:

```cpp
struct CompiledModuleLinkInfo {
    std::string name;
    std::vector<std::string> exports; // from ModuleEntry::export_bindings
    fs::path artifact_path;           // optional, for diagnostics/debugging
};

std::unordered_map<std::string, CompiledModuleLinkInfo> compiled_link_modules_;
```

Notes:

- Keep `runtime_module_info_[module].export_slots` as the slot source of truth
  (already present today).
- Update cache when `.etac` execution succeeds (including modules from imported
  artifacts auto-loaded by `run_etac_file`).

### 2. Extend `ModuleLinker` with compiled-module indexing

Add a direct API for compiled providers that indexes **exports only**:

```cpp
LinkResult<void> index_compiled_module_exports(
    const std::string& name,
    std::span<const std::string> exports);
```

Behavior:

- Reject duplicate module names (`DuplicateModule`) the same way as source path.
- Populate `modules_[name].exports`; leave `pending_[name]` empty.
- Do not synthesize import clauses for compiled module internals.

Rationale:

- Downstream source imports only need provider export sets.
- Attempting to synthesize internal imports from `.etac` import bindings is
  lossy (no local alias names), and can introduce false conflicts.

### 3. Replay compiled metadata into each fresh linker pass

In `run_source_impl`, before `index_modules(accumulated_forms_)`:

1. Collect source-declared module names from `accumulated_forms_`.
2. Index compiled cached modules whose names are **not** declared in source
   forms.
3. Index source modules.
4. Run `link()`.

Why exclude source-declared names:

- Preserve existing source-first behavior when sibling `.eta` is available.
- Avoid duplicate-module noise for modules already represented by source forms.

### 4. Make semantic import slot resolution compiled-aware

Extend semantic analysis with an external export-slot resolver:

```cpp
using ExternalExportSlotResolver =
    std::function<std::optional<uint32_t>(std::string_view module,
                                          std::string_view export_name)>;

SemResult<std::vector<ModuleSemantics>> analyze_all(
    std::span<const SExprPtr> forms,
    const eta::reader::ModuleLinker& linker,
    const eta::runtime::BuiltinEnvironment& builtins,
    ExternalExportSlotResolver external_slots);
```

Import slot lookup order when wiring imports:

1. Current-pass source export slot table.
2. `external_slots(module, export_name)` backed by `Driver::runtime_module_info_`.
3. Existing fallback (`add_import` fresh slot) only if both are missing.

This is required to make compiled-only imports bind to actual runtime values.

### 5. Keep source hydration as a preference, not a requirement

`hydrate_executed_module_source` remains useful when sibling `.eta` exists
(better spans/diagnostics), but correctness must no longer depend on it.

For `.etac` without sibling `.eta`, the linker/semantic path should still work
because compiled metadata + runtime slot map are already available.

## File-Level Change Plan

### `eta/core/src/eta/reader/module_linker.{h,cpp}`

- Add `index_compiled_module_exports(...)`.
- Ensure duplicate checks are shared with source indexing behavior.
- Keep compiled indexed modules compatible with existing `link()` logic.

### `eta/core/src/eta/semantics/semantic_analyzer.{h,cpp}`

- Add overload/parameter for external slot resolver.
- Use resolver in import wiring path before allocating fallback slots.
- Keep existing signatures as convenience wrappers where practical.

### `eta/session/src/eta/session/driver.h`

- Add `compiled_link_modules_` cache.
- Populate cache in `.etac` execution flow from `ModuleEntry::export_bindings`.
- In `run_source_impl`, replay compiled module exports into a fresh linker
  before source indexing.
- Pass external slot resolver backed by `runtime_module_info_` into semantic
  analyzer.

## Testing Plan

### Unit: linker

`module_linker_tests.cpp`

- Compiled provider indexed via `index_compiled_module_exports` + source importer:
  `link()` succeeds and imported names appear in target visibility/provenance.
- Duplicate name between compiled and source module -> `DuplicateModule`.

### Unit: semantics

`semantic_analyzer` tests (new or extended)

- Import from module absent in source forms but present in external slot resolver
  uses resolver slot (not fresh allocation).
- Resolver miss preserves existing fallback behavior.

### Driver/contract

`packaging_contract_tests.cpp`

- Compile module to `.etac`, remove sibling `.eta`, then import from source:
  succeeds and runtime value is correct.
- Re-export chain through compiled-only module remains correct for importer.
- Existing sibling-source hydration behavior still passes unchanged.
- `.etac`-only stdlib smoke stays green without `unknown module in import`.

## Rollout Steps

1. Add linker compiled-index API + tests.
2. Add semantic external-slot resolver + tests.
3. Wire driver cache/replay and resolver plumbing.
4. Add/adjust packaging contract coverage for `.etac`-only correctness.
5. Run full QA suite.

## Implementation Status (2026-05-06)

Implemented:

- `ModuleLinker` now supports indexing compiled-only providers via
  `index_compiled_module_exports(...)`.
- `SemanticAnalyzer` now accepts an external export-slot resolver and uses it
  before fresh-slot fallback for import binding.
- `Driver` now persists compiled module link metadata, replays compiled
  providers into each source linker pass, and passes a runtime-slot resolver
  backed by `runtime_module_info_`.
- Packaging contract coverage now includes:
  - source import from `.etac`-only module with no sibling `.eta`
  - compiled-only re-export chain import correctness
  - explicit guard that `.etac` stdlib-root source runs avoid
    `unknown module in import` regression diagnostics

## Risks / Mitigations

- **Risk:** duplicate module identity between compiled cache and source forms.
  **Mitigation:** source-name exclusion during replay + explicit duplicate tests.

- **Risk:** silent fallback to fresh slots hides integration bugs.
  **Mitigation:** add assertions/tests for slot-equality against
  `runtime_module_info_`; consider debug log when fallback path is taken.

- **Risk:** legacy v3/v4 artifacts lack v5 relocation metadata.
  **Mitigation:** keep current legacy behavior; `.etac`-only linking guarantee is
  for v5 artifacts.

## Expected Outcome

`.etac` artifacts become first-class link/semantic providers even without
sibling source files. Source imports of compiled-only modules will both validate
and bind correctly at runtime, enabling real binary-only stdlib and package
distribution.
