# Core Primitive De-duplication Plan

Status: **draft / proposed**

Objective:
1. Remove duplicate builtin registration metadata.
2. Eliminate `builtin_names.h` safely.
3. Reduce repetitive AAD wrapper code in `core_primitives.h`.
4. Keep runtime, analyzer, docs, and LSP behavior unchanged.

Decision:
1. **Yes, we should remove `builtin_names.h`**, but only after its current responsibilities are migrated.

---

## Task 1: Establish Baseline and Migration Safety

Code changes:
1. No functional changes.
2. Record current behavior and test baseline in PR notes.

Tests:
1. Build and run `eta_core_test`.
2. Run targeted suites:
   - `builtin_sync_tests`
   - `runtime_primitives_tests`
   - `stdlib_doc_tests`
   - `lsp_tests`
   - `native_sidecar_loader_tests`
   - `native_sidecar_manager_tests`

Done when:
1. Baseline is green and captured.

---

## Task 2: Add Canonical Builtin Catalog

Code changes:
1. Add catalog source:
   - `eta/core/src/eta/runtime/builtin_catalog.h`
   - `eta/core/src/eta/runtime/builtin_catalog.cpp` (or header-only constexpr table)
2. Add `BuiltinCatalogEntry` with:
   - `name`
   - `arity`
   - `has_rest`
   - `owner` (`core`, `sidecar:eta-torch`, etc.)
   - optional metadata override fields (`category`, `signature`, `summary`)
3. Add API:
   - `builtin_catalog() -> span<const BuiltinCatalogEntry>`

Tests:
1. Add tests in `builtin_sync_tests.cpp` (or `builtin_catalog_tests.cpp`):
   - no duplicate names
   - valid arity metadata

Done when:
1. Catalog compiles and tests pass.

---

## Task 3: Add Catalog Registration Adapter (No Behavior Change)

Code changes:
1. Add `register_builtin_specs(BuiltinEnvironment&)` that registers null-func specs from catalog.
2. Keep `builtin_names.h` public API intact for now.
3. Internally make `builtin_names.h` delegate to the new adapter.

Tests:
1. Add parity test:
   - adapter output matches legacy registration exactly (name/order/arity/has_rest).
2. Re-run baseline suites.

Done when:
1. Adapter and legacy path are behaviorally identical.

---

## Task 4: Move Metadata Construction to Catalog

Code changes:
1. Refactor `builtin_metadata.cpp` to build metadata from `builtin_catalog()`, not legacy registration.
2. Keep existing metadata APIs unchanged:
   - `builtin_metadata()`
   - `lookup_builtin_metadata(...)`
   - `missing_builtin_docs(...)`

Tests:
1. Add metadata order parity test:
   - metadata order == catalog order.
2. Add/keep doc completeness and uniqueness tests.
3. Run `stdlib_doc_tests` and `lsp_tests`.

Done when:
1. Metadata output is unchanged (or intentional deltas are reviewed).

---

## Task 5: Move Sidecar Ownership Mapping to Catalog

Code changes:
1. Replace ownership heuristics in `builtin_metadata.cpp` with `owner` from catalog.
2. Keep `builtin_native_sidecar_package(...)` signature unchanged.

Tests:
1. Add ownership mapping test:
   - catalog owner == `builtin_native_sidecar_package(...)`.
2. Re-run:
   - `native_sidecar_loader_tests`
   - `native_sidecar_manager_tests`
   - `builtin_sync_tests`

Done when:
1. Sidecar placeholders and ownership behavior are unchanged.

---

## Task 6: Switch Driver Bootstrap to Catalog Adapter

Code changes:
1. Update `eta/session/src/eta/session/driver.cpp`:
   - replace `register_builtin_names(...)` usage with `register_builtin_specs(...)`.
2. Keep patching flow unchanged:
   - `begin_patching()`
   - runtime registration
   - `verify_all_patched()`

Tests:
1. Add/adjust test ensuring driver bootstrap installs expected builtin count/order.
2. Run:
   - `runtime_primitives_tests`
   - `compilation_session_tests`
   - `builtin_sync_tests`

Done when:
1. Driver no longer depends on legacy names API.

---

## Task 7: Switch LSP Bootstrap to Catalog Adapter

Code changes:
1. Update `eta/tools/lsp/src/eta/lsp/lsp_server.cpp`:
   - replace `register_builtin_names(...)` with `register_builtin_specs(...)`.

Tests:
1. Run `lsp_tests`.
2. Add/adjust LSP semantic test for builtin arity diagnostics to confirm no regression.

Done when:
1. LSP no longer depends on legacy names API.

---

## Task 8: Remove Legacy API Surface

Code changes:
1. Remove legacy declarations such as `register_builtin_names_legacy` from public headers.
2. Update includes and comments across core/session/tools/tests.

Tests:
1. Full `eta_core_test` run.
2. Targeted compile check for `eta_lsp`, `etai`, and `etac`.

Done when:
1. No call sites reference legacy names functions.

---

## Task 9: Delete `builtin_names.h`

Code changes:
1. Delete `eta/core/src/eta/runtime/builtin_names.h`.
2. Remove any remaining include references.
3. Add regression guard script in `scripts/tests/` to fail on `register_builtin_names` symbol usage.

Tests:
1. Run full `eta_core_test`.
2. Run CI smoke for:
   - `eta`
   - `etai`
   - `etac`
   - `eta_lsp`
3. Run legacy-symbol guard script.

Done when:
1. Build/test green without `builtin_names.h`.

---

## Task 10: Add Catalog-to-Runtime Order Invariant Test

Code changes:
1. Add invariant test:
   - catalog order exactly matches runtime patch registration order in `register_all_primitives`.

Tests:
1. Run `builtin_sync_tests`.

Done when:
1. Any future order drift fails tests immediately.

---

## Task 11: Introduce Shared AAD Unary Wrapper Helpers

Code changes:
1. Add helper functions inside `core_primitives.h` (or local helper header) for unary tape-aware math op boilerplate:
   - active tape lookup
   - TapeRef validation
   - entry push + node index check
   - numeric fallback dispatch
2. Do not migrate ops yet.

Tests:
1. Add helper-focused unit coverage in `runtime_primitives_tests.cpp` (or new `aad_primitives_tests.cpp`):
   - no active tape
   - stale/mismatched ref
   - index overflow guard path

Done when:
1. Helpers are covered and behavior-neutral.

---

## Task 12: Migrate Low-risk Unary AAD Ops

Code changes:
1. Migrate `sin`, `cos`, `exp` to helper-based implementation.

Tests:
1. Add parity tests for:
   - primal value
   - tape op type
   - backward gradient
2. Run:
   - `runtime_primitives_tests`
   - `builtin_sync_tests`
   - stdlib `aad.test.eta`

Done when:
1. No behavior deltas vs baseline.

---

## Task 13: Migrate Domain-sensitive Unary AAD Ops

Code changes:
1. Migrate `log`, `sqrt`, `asin`, `acos`, `atan`, `tan`.
2. Keep strict-mode and domain checks explicit per op.

Tests:
1. Add parity coverage for:
   - domain error messages/tags
   - strict-mode non-diff behavior
2. Re-run all AAD and runtime primitive tests.

Done when:
1. Domain and strict-mode behavior matches baseline.

---

## Task 14: Evaluate `pow` Refactor (Optional)

Code changes:
1. Refactor `pow` only if readability improves without hiding its special-case logic.
2. Skip if helper abstraction makes behavior harder to reason about.

Tests:
1. Add explicit `pow` regression cases:
   - negative base with non-integer exponent
   - zero base with negative exponent
   - strict-mode edge cases
2. Re-run AAD tests.

Done when:
1. Either `pow` is improved with full parity, or explicitly left unchanged.

---

## Task 15: Final Cleanup and Completion Gate

Code changes:
1. Remove stale comments/docs mentioning legacy builtin names table.
2. Ensure architecture docs point to catalog as single source of truth.

Tests:
1. Full `eta_core_test`.
2. Tooling smoke (`eta`, `etai`, `etac`, `eta_lsp`).
3. Legacy symbol guard script.

Done when:
1. All tasks complete, all tests green, and docs reflect final architecture.

---

## Minimal Merge Policy

1. One task per PR (or tightly related pair only).
2. No task merges without its listed tests passing.
3. No deletion of legacy files before all consuming call sites are migrated.
