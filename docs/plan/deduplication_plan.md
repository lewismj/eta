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

Task 1 baseline notes (2026-05-13):
1. Added `legacy_and_public_builtin_registration_are_slot_identical` in
   `eta/qa/test/src/builtin_sync_tests.cpp` to lock in slot parity between
   `register_builtin_names_legacy(...)` and `register_builtin_names(...)`.
2. Baseline verification suites for this task:
   - `builtin_sync_tests`
   - `runtime_primitives_tests`
   - `stdlib_doc_tests`
   - `lsp_*` (`lsp_protocol` + `lsp_framing_robustness`)
   - `native_sidecar_loader_tests`
   - `native_sidecar_manager_tests`
3. Stage gate: run `eta_core_test` and `eta_stdlib_tests` before starting the
   next task.
4. Baseline runs in this repo should unset `ETA_MODULE_PATH` for the test
   process so external local module-path overrides do not affect results.

---

## Task 2: Add Canonical Builtin Catalog

Code changes:
1. Add catalog source:
   - `eta/core/src/eta/runtime/builtin_catalog.h`
   - `eta/core/src/eta/runtime/builtin_catalog.cpp` (preferred; avoid header-only constexpr catalog)
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

Task 2 implementation notes (2026-05-13):
1. Added `eta/core/src/eta/runtime/builtin_catalog.h` and
   `eta/core/src/eta/runtime/builtin_catalog.cpp`.
2. Added `BuiltinCatalogEntry` with `name`, `arity`, `has_rest`, `owner`, and
   optional metadata override fields (`category`, `signature`, `summary`).
3. Added `builtin_catalog()` API returning
   `std::span<const BuiltinCatalogEntry>` in registration order.
4. Added tests in `eta/qa/test/src/builtin_sync_tests.cpp`:
   - `catalog_has_no_duplicate_names`
   - `catalog_matches_legacy_registration_metadata`
5. Stage gate for this task: run `eta_core_test` and `eta_stdlib_tests` with
   `ETA_MODULE_PATH` unset in the test process.

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

Task 3 implementation notes (2026-05-13):
1. Added `register_builtin_specs(BuiltinEnvironment&)` to
   `eta/core/src/eta/runtime/builtin_catalog.h` and implemented it in
   `eta/core/src/eta/runtime/builtin_catalog.cpp`.
2. Updated `register_builtin_names(...)` in
   `eta/core/src/eta/runtime/builtin_names.h` to delegate to the catalog
   adapter while keeping the existing public API surface unchanged.
3. Added `catalog_registration_adapter_matches_legacy_exactly` in
   `eta/qa/test/src/builtin_sync_tests.cpp` to verify adapter output is
   exactly identical to legacy registration (name/order/arity/has_rest).
4. Baseline suites and stage gate were run with `ETA_MODULE_PATH` unset:
   - `builtin_sync_tests`
   - `runtime_primitives_tests`
   - `stdlib_doc_tests`
   - `lsp_protocol`
   - `lsp_framing_robustness`
   - `native_sidecar_loader_tests`
   - `native_sidecar_manager_tests`
   - `eta_core_test`
   - `eta_stdlib_tests`

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

Task 4 implementation notes (2026-05-13):
1. Refactored `eta/core/src/eta/runtime/builtin_metadata.cpp` to build
   metadata entries from `builtin_catalog()` instead of
   `register_builtin_names_legacy(...)`.
2. Kept metadata APIs unchanged:
   - `builtin_metadata()`
   - `lookup_builtin_metadata(...)`
   - `missing_builtin_docs(...)`
3. Added `builtin_metadata_order_matches_catalog_order` in
   `eta/qa/test/src/builtin_sync_tests.cpp` to enforce metadata order parity
   with catalog order (name/arity/has_rest per slot).
4. Updated `eta/core/src/eta/runtime/builtin_metadata.h` legacy declaration
   comment to reflect migration status without claiming metadata seeding.
5. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - `stdlib_doc_tests`
   - `lsp_protocol`
   - `lsp_framing_robustness`
   - `builtin_sync_tests`
   - `eta_core_test`
   - `eta_stdlib_tests`

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

Task 5 implementation notes (2026-05-13):
1. Replaced ownership inference inside
   `eta/core/src/eta/runtime/builtin_metadata.cpp` by resolving native sidecar
   package names from `builtin_catalog().owner`.
2. Kept `builtin_native_sidecar_package(...)` signature unchanged.
3. Added `catalog_owner_matches_native_sidecar_lookup` in
   `eta/qa/test/src/builtin_sync_tests.cpp` to assert catalog ownership parity
   with `builtin_native_sidecar_package(...)`.
4. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - `native_sidecar_loader_tests`
   - `native_sidecar_manager_tests`
   - `builtin_sync_tests`
   - `eta_core_test`
   - `eta_stdlib_tests`

---

## Task 6+7: Switch Driver and LSP Bootstrap to Catalog Adapter (Single PR)

Code changes:
1. Update `eta/session/src/eta/session/driver.cpp`:
   - replace `register_builtin_names(...)` usage with `register_builtin_specs(...)`.
2. Keep driver patching flow unchanged:
   - `begin_patching()`
   - runtime registration
   - `verify_all_patched()`
3. Update `eta/tools/lsp/src/eta/lsp/lsp_server.cpp`:
   - replace `register_builtin_names(...)` with `register_builtin_specs(...)`.

Tests:
1. Add/adjust test ensuring driver bootstrap installs expected builtin count/order.
2. Add/adjust LSP semantic test for builtin arity diagnostics to confirm no regression.
3. Run:
   - `runtime_primitives_tests`
   - `compilation_session_tests`
   - `builtin_sync_tests`
   - `lsp_tests`

Done when:
1. Driver and LSP no longer depend on legacy names API.

Task 6+7 implementation notes (2026-05-13):
1. Updated `eta/session/src/eta/session/driver.cpp` to include
   `eta/runtime/builtin_catalog.h` and switched driver bootstrap registration
   from `register_builtin_names(...)` to `register_builtin_specs(...)`.
2. Kept driver patching flow unchanged (`begin_patching()`,
   `register_all_primitives(...)`, `verify_all_patched()`).
3. Updated `eta/tools/lsp/src/eta/lsp/lsp_server.cpp` to include
   `eta/runtime/builtin_catalog.h` and switched semantic bootstrap registration
   from `register_builtin_names(...)` to `register_builtin_specs(...)`.
4. Added `driver_bootstrap_builtin_slots_match_catalog_order` in
   `eta/qa/test/src/driver_facade_tests.cpp` to verify driver bootstrap count
   and slot/name order against `builtin_catalog()`.
5. Added `diagnostics_report_semantic_arity_error_for_if` in
   `eta/qa/test/src/lsp_tests.cpp` as a non-regression check for LSP
   arity-related diagnostics during validation.
6. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - `runtime_primitives_tests`
   - `compilation_session_tests`
   - `builtin_sync_tests`
   - `lsp_protocol`
   - `lsp_framing_robustness`
   - `eta_core_test`
   - `eta_stdlib_tests`

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

Task 8 implementation notes (2026-05-13):
1. Removed the legacy names-table declaration from
   `eta/core/src/eta/runtime/builtin_metadata.h`.
2. Renamed `register_builtin_names_legacy(...)` in
   `eta/core/src/eta/runtime/builtin_names.h` to
   `detail::register_builtin_specs_seed(...)` and updated
   `eta/core/src/eta/runtime/builtin_catalog.cpp` to use that internal helper.
3. Updated test and sidecar metadata registration call sites to use
   `register_builtin_specs(...)` instead of `register_builtin_names*`:
   - `eta/qa/test/src/builtin_sync_tests.cpp`
   - `eta/qa/test/src/atom_tests.cpp`
   - `eta/qa/test/src/torch_tests.cpp`
   - `eta/qa/test/src/native_sidecar_test_extension.cpp`
4. Updated registration-order comments to reference builtin catalog/spec
   registration in:
   - `eta/core/src/eta/runtime/builtin_env.h`
   - `eta/tools/interpreter/src/eta/interpreter/all_primitives.h`
   - `packages/stdlib/native/nng/src/eta/nng/nng_primitives.h`
5. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - compile check `eta_lsp`, `etai`, `etac`
   - `eta_core_test`
   - `eta_stdlib_tests`

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

Task 9 implementation notes (2026-05-13):
1. Deleted `eta/core/src/eta/runtime/builtin_names.h`.
2. Migrated the seed registration helper to
   `eta/core/src/eta/runtime/builtin_specs_seed.h` and updated
   `eta/core/src/eta/runtime/builtin_catalog.cpp` to include that internal
   header.
3. Added legacy-symbol regression guard script:
   `scripts/tests/check_legacy_builtin_names_symbol.py`.
4. Added focused unit tests for the guard script in:
   `scripts/tests/test_check_legacy_builtin_names_symbol.py`.
5. Wired both into CTest from `eta/CMakeLists.txt`:
   - `legacy_builtin_names_symbol_guard`
   - `legacy_builtin_names_symbol_guard_tests`
6. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - smoke build targets `eta`, `etai`, `etac`, `eta_lsp`
   - `legacy_builtin_names_symbol_guard`
   - `legacy_builtin_names_symbol_guard_tests`
   - `eta_core_test`
   - `eta_stdlib_tests`

---

## Task 10: Add Catalog-to-Runtime Order Invariant Test

Code changes:
1. Add invariant test:
   - catalog order exactly matches runtime patch registration order in `register_all_primitives`.

Tests:
1. Run `builtin_sync_tests`.

Done when:
1. Any future order drift fails tests immediately.

Task 10 implementation notes (2026-05-13):
1. Added `catalog_order_matches_runtime_registration_order` in
   `eta/qa/test/src/builtin_sync_tests.cpp` to assert that
   `register_all_primitives(...)` append-mode registration order exactly
   matches `builtin_catalog()` (name/arity/has_rest per slot).
2. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - `builtin_sync_tests`
   - `eta_core_test`
   - `eta_stdlib_tests`

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

Task 11 implementation notes (2026-05-13):
1. Added local helper header `eta/core/src/eta/runtime/aad_unary_helpers.h` with
   shared unary AAD wrapper helpers for:
   - active tape lookup (`active_tape_for_op`)
   - TapeRef validation (`validate_tape_ref_for_op`)
   - node-index guard + TapeRef construction (`checked_node_index_for_op`,
     `make_tape_ref_result`, `push_unary_tape_entry`)
   - numeric fallback dispatch (`dispatch_numeric_fallback`)
2. Updated `eta/core/src/eta/runtime/core_primitives.h` to include the new
   local header and route existing shared lambdas (`validate_ref_for_tape`,
   `get_active_tape_for_op`, `make_tape_ref_result`) through these helpers
   without migrating unary op implementations yet.
3. Added helper-focused unit coverage in
   `eta/qa/test/src/runtime_primitives_tests.cpp`:
   - `aad_unary_helper_reports_no_active_tape`
   - `aad_unary_helper_rejects_mismatched_and_stale_tape_refs`
   - `aad_unary_helper_checks_tape_ref_index_capacity`
4. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - `runtime_primitives_tests` (targeted Boost suite)
   - `eta_core_test`
   - `eta_stdlib_tests`

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

Task 12 implementation notes (2026-05-13):
1. Migrated `sin`, `cos`, and `exp` in
   `eta/core/src/eta/runtime/core_primitives.h` to the shared unary AAD
   helpers:
   - active tape resolution via `active_tape_for_op` (through
     `get_active_tape_for_op`)
   - TapeRef validation via `validate_tape_ref_for_op` (through
     `validate_ref_for_tape`)
   - node append + TapeRef construction via `push_unary_tape_entry`
   - numeric fallback/type-check path via `dispatch_numeric_fallback`
2. Added parity coverage in
   `eta/qa/test/src/runtime_primitives_tests.cpp`:
   - `aad_unary_sin_cos_exp_match_primal_op_type_and_backward_gradient`
   - verifies per-op primal value parity, recorded tape op type
     (`TapeOp::Sin`, `TapeOp::Cos`, `TapeOp::Exp`), and backward gradient.
3. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - `runtime_primitives_tests` (targeted Boost suite)
   - `builtin_sync_tests` (targeted Boost suite)
   - stdlib `aad.test.eta`
   - `eta_core_test`
   - `eta_stdlib_tests`

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

Task 13 implementation notes (2026-05-13):
1. Migrated `tan`, `asin`, `acos`, `atan`, `log`, and `sqrt` in
   `eta/core/src/eta/runtime/core_primitives.h` to the shared unary AAD
   helpers:
   - active tape resolution via `active_tape_for_op` (through
     `get_active_tape_for_op`)
   - TapeRef validation via `validate_tape_ref_for_op` (through
     `validate_ref_for_tape`)
   - node append + TapeRef construction via `push_unary_tape_entry`
   - numeric fallback/type-check path via `dispatch_numeric_fallback`
2. Kept domain checks explicit per operation in the taped path:
   - `log`: `x > 0`
   - `sqrt`: `x >= 0`
   - `asin`/`acos`: `-1 <= x <= 1`
3. Added Task 13 parity coverage in
   `eta/qa/test/src/runtime_primitives_tests.cpp`:
   - `aad_unary_domain_sensitive_ops_match_primal_op_type_and_backward_gradient`
   - `aad_unary_domain_sensitive_ops_report_domain_error_tag_and_message`
   - `aad_unary_domain_sensitive_ops_preserve_singular_gradient_behavior_in_strict_mode`
4. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - `runtime_primitives_tests` (targeted Boost suite)
   - `builtin_sync_tests` (targeted Boost suite)
   - full `ctest --output-on-failure` in `out/msvc-release`
     (includes `eta_core_test` and `eta_stdlib_tests`)

---

## Task 14: Evaluate `pow` Refactor (Optional, Default: Skipped)

Code changes:
1. Default outcome: explicitly mark Task 14 as skipped.
2. Open this task only if Task 13 migration reveals a natural helper-compatible shape for `pow`.
3. If opened, refactor `pow` only if readability improves without hiding its special-case logic.
4. Skip if helper abstraction makes behavior harder to reason about.

Tests:
1. Add explicit `pow` regression cases:
   - negative base with non-integer exponent
   - zero base with negative exponent
   - strict-mode edge cases
2. Re-run AAD tests.

Done when:
1. Either Task 14 is explicitly skipped by default, or `pow` is improved with full parity when opened.

Task 14 implementation notes (2026-05-13):
1. Task 14 is explicitly skipped by default.
2. `pow` remains unchanged because its edge-case behavior is clearer in its
   current dedicated implementation than in the unary helper abstraction.

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

Task 15 implementation notes (2026-05-13):
1. Removed stale doc references to the legacy builtin names table in:
   - `docs/plan/std_bitset_plan.md`
   - `docs/plan/actor_improvement_plan.md`
2. Updated `docs/architecture.md` with a dedicated "Builtin Catalog
   Contract" section and explicit `builtin_catalog.h` /
   `register_builtin_specs(...)` source-of-truth guidance.
3. Added a docs guard script and tests:
   - `scripts/tests/check_builtin_catalog_docs.py`
   - `scripts/tests/test_check_builtin_catalog_docs.py`
4. Wired new docs guard tests into CTest from `eta/CMakeLists.txt`:
   - `builtin_catalog_docs_guard`
   - `builtin_catalog_docs_guard_tests`
5. Verification for this task (with `ETA_MODULE_PATH` unset in each process):
   - build `eta_all`
   - full `ctest --output-on-failure` in `out/msvc-release`
     (includes `eta_core_test`, `eta_stdlib_tests`, and all Python guard tests)
   - tooling smoke:
     - `eta --help`
     - `etai --help`
     - `etac --help`
     - `eta_lsp --help`
   - direct legacy-symbol guard: `check_legacy_builtin_names_symbol.py`

---

## Minimal Merge Policy

1. Tasks 6+7 are intentionally merged into one PR; all other tasks are one task per PR unless explicitly paired.
2. No task merges without its listed tests passing.
3. No deletion of legacy files before all consuming call sites are migrated.
