# Core Primitive De-duplication Plan

Status: **draft / proposed**
Primary goals:
1. Remove duplicated builtin registration metadata.
2. Reduce repetitive AAD wrapper logic in `core_primitives.h`.
3. Keep runtime/analyzer/docs behavior fully consistent.

---

## 0. Pre-flight and safety rails

Before Phase A:
1. Capture a baseline test run and keep it attached to the migration PR.
2. Do not run large refactors in one commit; keep each phase bisectable.
3. Require green tests at each phase boundary before merging to main.

Recommended baseline gate:
1. Build `eta_core_test`.
2. Run CTest target `eta_core_test` with `--output-on-failure`.
3. Run targeted smoke subsets while iterating:
   - `builtin_sync_tests`
   - `stdlib_doc_tests`
   - `runtime_primitives_tests`
   - `lsp_tests`
   - `native_sidecar_loader_tests`
   - `native_sidecar_manager_tests`

Example local commands (replace `<build_dir>`):

```bash
cmake --build <build_dir> --target eta_core_test
ctest --test-dir <build_dir> --output-on-failure -R eta_core_test
<build_dir>/eta/qa/test/eta_core_test --run_test=builtin_sync_tests
<build_dir>/eta/qa/test/eta_core_test --run_test=lsp_tests
```

## 1. Decision: can we remove `builtin_names.h` entirely?

Short answer: **yes, but not in one step**.

Today `builtin_names.h` is still a key compatibility layer used by:
1. Driver bootstrap (`register_builtin_names` before patching runtime impls).
2. LSP semantic analysis setup.
3. Metadata construction path (`builtin_metadata.cpp` currently seeds from `register_builtin_names_legacy`).

We can remove it once a new canonical builtin catalog replaces those call sites.

---

## 2. Target architecture (single source of truth)

Introduce a single canonical builtin catalog, for example:
1. `eta/core/src/eta/runtime/builtin_catalog.def` (X-macro style), or
2. `eta/core/src/eta/runtime/builtin_catalog.h` with one constexpr array.

Each builtin entry should include at least:
1. `name`
2. `arity`
3. `has_rest`
4. `owner` (`core`, `sidecar:eta-torch`, `sidecar:eta-log`, etc.)
5. Optional doc overrides (`category`, `signature`, `summary`) where heuristics are insufficient.

All other consumers derive from this catalog.

---

## 3. Migration phases

### Phase A: Introduce catalog without behavior change

1. Add the new catalog file and API:
   - `builtin_catalog() -> span<const BuiltinCatalogEntry>`
2. Add adapter function:
   - `register_builtin_specs(BuiltinEnvironment&)`
   - Registers `name/arity/has_rest` with null funcs.
3. Keep `builtin_names.h` intact temporarily, but make it delegate to catalog data.

Exit criteria:
1. Driver/LSP/tests unchanged behavior.
2. Existing sync tests pass.

Implementation checklist:
1. Add:
   - `eta/core/src/eta/runtime/builtin_catalog.h`
   - `eta/core/src/eta/runtime/builtin_catalog.cpp` (or header-only constexpr table)
2. Define `BuiltinCatalogEntry` with:
   - `name`, `arity`, `has_rest`
   - provisional `owner` field (can be defaulted in Phase A)
3. Add adapter:
   - `register_builtin_specs(BuiltinEnvironment&)`
4. Refactor `builtin_names.h` internals to call `register_builtin_specs(...)` (public API unchanged for now).
5. Keep comments in `all_primitives.h` and `builtin_env.h` aligned with new source-of-truth wording.

Tests to add in Phase A:
1. Extend `eta/qa/test/src/builtin_sync_tests.cpp` with:
   - `catalog_has_no_duplicate_names`
   - `catalog_entries_have_valid_arity_metadata`
   - `register_builtin_specs_matches_register_builtin_names_legacy` (exact name/arity/rest parity while legacy still exists)
2. Add optional dedicated file `eta/qa/test/src/builtin_catalog_tests.cpp` if `builtin_sync_tests.cpp` becomes too large.
3. Update `eta/qa/test/CMakeLists.txt` to include new test file if added.

Phase A test gate:
1. `builtin_sync_tests` (including new catalog parity checks)
2. `runtime_primitives_tests`
3. `lsp_tests`
4. `eta_core_test` full run

### Phase B: Move metadata + sidecar ownership to catalog

1. Refactor `builtin_metadata.cpp` to build from `builtin_catalog()` directly.
2. Replace hardcoded sidecar ownership logic with `owner` field from catalog.
3. Keep doc overrides in one place (prefer catalog fields over name-prefix heuristics).

Exit criteria:
1. `builtin_metadata()` output unchanged (or intentional + reviewed deltas only).
2. stdlib doc tests and builtin sync tests pass.

Implementation checklist:
1. Move sidecar ownership from heuristic code in `builtin_metadata.cpp` into catalog entry data.
2. Refactor metadata generation to iterate catalog entries directly (not legacy registration).
3. Keep `category/signature/summary` behavior stable:
   - preserve current overrides;
   - only move storage location/source.
4. Keep `lookup_builtin_metadata`, `builtin_native_sidecar_package`, and `missing_builtin_docs` API signatures unchanged.

Tests to add in Phase B:
1. In `builtin_sync_tests.cpp`:
   - `builtin_native_sidecar_package_matches_catalog_owner`
   - `builtin_metadata_order_matches_catalog_order`
2. In `lsp_tests.cpp`:
   - hover/signature test for at least one core builtin and one sidecar-owned builtin to ensure metadata remained intact.
3. In `native_sidecar_manager_tests.cpp`:
   - assert sidecar ownership mapping used by placeholders still resolves expected package IDs.

Phase B test gate:
1. `builtin_sync_tests`
2. `stdlib_doc_tests`
3. `lsp_tests`
4. `native_sidecar_loader_tests` and `native_sidecar_manager_tests`
5. `eta_core_test` full run

### Phase C: Replace all `builtin_names.h` consumers

1. Driver: replace `register_builtin_names(...)` with `register_builtin_specs(...)`.
2. LSP: same replacement for analysis env setup.
3. Tests: switch includes/calls to new catalog API.
4. Keep a short compatibility shim only if needed for one release.

Exit criteria:
1. No production call sites include `builtin_names.h`.
2. CI passes across core/session/tools/tests.

Implementation checklist:
1. Update Driver bootstrap call sites:
   - `eta/session/src/eta/session/driver.cpp`
2. Update LSP analyzer bootstrap call sites:
   - `eta/tools/lsp/src/eta/lsp/lsp_server.cpp`
3. Update remaining tests and helpers to include/use catalog APIs.
4. Keep temporary shim in `builtin_names.h` only for compatibility if any external/internal tool still depends on old symbol names.

Tests to add in Phase C:
1. In `builtin_sync_tests.cpp`:
   - `driver_and_lsp_bootstrap_use_catalog_registration` (can be behavioral via environment/spec count parity checks)
2. In `lsp_tests.cpp`:
   - semantic diagnostics test covering builtin arity errors still reported as before.
3. In `runtime_primitives_tests.cpp`:
   - ensure builtin install count equals `builtin_metadata().size()`.

Phase C test gate:
1. `builtin_sync_tests`
2. `runtime_primitives_tests`
3. `lsp_tests`
4. `compilation_session_tests`
5. `eta_core_test` full run

### Phase D: Delete `builtin_names.h`

1. Remove file and legacy API declarations (`register_builtin_names_legacy`).
2. Remove compatibility shim if introduced.
3. Update docs/comments that still reference "legacy names table."

Exit criteria:
1. No references to `register_builtin_names*`.
2. Build + test green.

Implementation checklist:
1. Delete:
   - `eta/core/src/eta/runtime/builtin_names.h`
2. Remove legacy declarations from:
   - `eta/core/src/eta/runtime/builtin_metadata.h`
3. Remove all includes/usages and update comments/docs.
4. Add a lightweight guard to prevent reintroduction:
   - repository check script in `scripts/tests/` that fails CI if `register_builtin_names` symbols are referenced.

Tests to add in Phase D:
1. Add CI/script-level check:
   - `no_legacy_builtin_names_symbol_references`
2. Keep all catalog parity tests (legacy parity test should be replaced with direct catalog/runtime parity checks).

Phase D test gate:
1. `eta_core_test` full run
2. CLI smoke (`eta`, `etai`, `etac`) in CI job
3. LSP smoke (in-process tests + startup check)

---

## 4. Core AAD wrapper de-duplication plan

This runs in parallel with Phases B-C once catalog work is stable.

1. Introduce shared helper(s) for unary tape-aware math ops (`sin`, `cos`, `exp`, `log`, `sqrt`, `asin`, `acos`, `atan`, `tan`):
   - Common active-tape lookup
   - TapeRef validation
   - Push + node-index guard
   - Common numeric fallback path
2. Keep per-op domain checks/strict-mode behavior explicit and local.
3. Apply incrementally op-by-op with tests after each batch.

Expected result:
1. Smaller `core_primitives.h`.
2. Fewer copy/paste error surfaces.
3. No change to AD semantics.

Implementation batches:
1. Batch 1:
   - Introduce helper(s) only; no primitive migration yet.
   - Add focused unit tests for helper edge paths (no active tape, stale ref, out-of-range index).
2. Batch 2:
   - Migrate low-risk ops: `sin`, `cos`, `exp`.
3. Batch 3:
   - Migrate domain-sensitive ops: `log`, `sqrt`, `asin`, `acos`, `atan`, `tan`.
4. Batch 4 (optional):
   - Evaluate binary helper for `pow` if it improves readability without obscuring domain/strict handling.

Tests to add for AAD refactor:
1. Add/extend tests in `eta/qa/test/src/runtime_primitives_tests.cpp` (or dedicated `aad_primitives_tests.cpp`) covering:
   - primal output parity
   - tape op type parity
   - strict mode non-diff error parity
   - domain error parity
2. Add end-to-end regression in stdlib tests for gradients across migrated ops (reuse `stdlib/tests/aad.test.eta`, extend if needed).

AAD gate per batch:
1. New AAD unit tests
2. Existing `runtime_primitives_tests`
3. Existing `builtin_sync_tests`
4. `eta_core_test` full run before merging each batch

---

## 5. Verification strategy

Required checks per phase:
1. `builtin_sync_tests`
2. `stdlib_doc_tests`
3. `runtime_primitives_tests`
4. `native_sidecar_*_tests`
5. LSP smoke/semantic diagnostics tests

Add one new invariant test:
1. Catalog order exactly matches runtime patch order in `register_all_primitives`.

Recommended CI structure:
1. Fast gate (every PR push):
   - catalog/builtin/AAD targeted tests
   - LSP targeted tests
2. Full gate (required before merge):
   - full `eta_core_test`
   - package/CLI integration jobs already present in CI

Manual reviewer checklist per phase:
1. Confirm no user-visible doc/signature regressions in LSP hover/signature help.
2. Confirm sidecar placeholder behavior unchanged for missing dependencies.
3. Confirm patch-mode abort guards remain intact and exercised by tests.

---

## 6. Risks and mitigations

1. Risk: order drift breaks patch-mode bootstrap.
   Mitigation: explicit order test + keep `begin_patching()/verify_all_patched()` hard-fail behavior.

2. Risk: metadata regressions in docs/hover/signature help.
   Mitigation: snapshot test for `builtin_metadata()` and targeted LSP hover/signature tests.

3. Risk: sidecar owner mapping drift.
   Mitigation: sidecar package mapping test from catalog entries.

---

## 7. Recommended implementation order

1. Phase A (catalog + adapters)
2. Phase B (metadata/owner source switch)
3. Phase C (consumer cutover)
4. Phase D (delete `builtin_names.h`)
5. AAD helper refactor batches

This keeps each commit bisectable and avoids a risky all-at-once migration.
