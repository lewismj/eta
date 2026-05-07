# Relocatable Linking Test Plan

[Back to workspace plan](./workspace_plan.md)

---

## Scope

Close the remaining negative-path coverage gaps for relocatable `.etac` linking
in `eta::session::Driver::run_etac_file`.

---

## Planned tests

Implemented on 2026-05-07 in
`eta/qa/test/src/packaging_contract_tests.cpp`.

1. `run_etac_file_relocate_missing_export_emits_clear_error`
   - Build fixture modules, then load a compiled module whose import binding
     references an export that does not exist at relocation time.
   - Assert failure and diagnostic text:
     `cannot relocate import '<name>' from module '<mod>' while loading '<mod>'`.

2. `run_etac_file_relocate_conflicting_local_slot_emits_inconsistent_error`
   - Build fixture metadata where two import bindings target the same
     `local_slot` but resolve to different runtime slots.
   - Assert failure and diagnostic text:
     `inconsistent relocation for slot <n> while loading '<mod>'`.

3. `run_etac_file_relocate_duplicate_local_slot_same_target_is_allowed`
   - Build fixture metadata where duplicate import bindings map one `local_slot`
     to the same runtime slot.
   - Assert relocation succeeds and runtime result is correct.

4. `relocation_diagnostic_spanless_output_uses_file_id_fallback`
   - Exercise a relocation failure emitted with empty span metadata.
   - Assert formatted diagnostics use numeric fallback (`file <id>`) rather than
     requiring a resolved filename.

---

## Location and sequencing

1. Add tests in `eta/qa/test/src/packaging_contract_tests.cpp` (integration).
2. Reuse existing `compile_to_etac` helper; add a small local helper to mutate
   module metadata for relocation-failure fixtures.
3. Keep assertions deterministic (explicit message substrings, fixed module
   names, fixed slot ids).

---

## Verification gate

1. Build with the standard MSVC command used for Eta.
2. Run `ctest --test-dir out/msvc-release -C Release --output-on-failure`.
3. Run `eta_test --path <stdlib-root> <stdlib/tests>` with the same invocation
   pattern used by CTest.
