# `core_primitives.h` Split Plan

Status: **draft / proposed**
Last reviewed against this branch: **2026-05-13**

Current snapshot:
1. `eta/core/src/eta/runtime/core_primitives.h` is 6,729 lines.
2. It contains 219 `env.register_builtin(...)` calls.
3. `core_primitives.cpp` and `core_primitives_*.cpp` do not exist yet.
4. `eta/core/CMakeLists.txt` has no source file dedicated to core primitive registration.

Objective:
1. Split monolithic `register_core_primitives(...)` into 9 focused domain units plus a dispatcher.
2. Remove heavy transitive includes from the public `core_primitives.h`.
3. Keep the public API unchanged:
   `register_core_primitives(env, heap, intern_table, vm*)`.
4. Preserve builtin registration order exactly (slot order must stay catalog-identical).
5. No behavior change; all tests remain green.

---

## Background

`core_primitives.h` currently contains the entire runtime implementation of
`register_core_primitives(...)` plus all heavy includes (`clp/`, `types/tape.h`,
`stats_math.h`, profiler, VM internals, etc.).

Approximate registration regions in the current header:

| Domain | Approx. lines | Approx. builtins |
|---|---:|---:|
| Shared helper lambdas | 86-172 | n/a |
| Arithmetic, predicates | 173-747 | ~30 |
| Transcendental math + AAD policy | 760-1056 | ~11 |
| Lists, higher-order, equality | 1073-1770 | ~16 |
| Strings/symbols + delegate hooks | 1778-1925 | ~16 + 3 delegates |
| Vectors, hash-map, hash-set, atoms | 1930-2509 | ~36 |
| Error/platform/profiler | 2572-2775 | ~9 |
| Logic vars, attrs, occurs-check, dual | 2784-3203 | ~25 |
| CLP (`%clp-*`) | 3231-5690 | ~25 |
| Tape/AAD control | 5690-5825 | ~13 |
| Fact-table/stats/tail hooks/eval | 5943-6720 | ~37 |

### Why a naive `.cpp` move fails

`register_core_primitives(...)` has local lambdas that capture each other
and/or recursive closures (`hash_value_impl`, CLP helper closures, AAD helpers).
Those anonymous types cannot be forward declared cleanly across translation
units.

### Chosen approach: `PrimReg` + private internal header

`PrimReg` must be declared in a shared private header so member functions can be
defined across multiple `.cpp` files.

Planned file layout:

```
core_primitives.h                  <- public declaration only
core_primitives_internal.h         <- private PrimReg declaration
core_primitives.cpp                <- dispatcher + call order
primitives/core_primitives_arithmetic.cpp
primitives/core_primitives_math.cpp
primitives/core_primitives_sequences.cpp
primitives/core_primitives_strings.cpp
primitives/core_primitives_misc.cpp
primitives/core_primitives_logic.cpp
primitives/core_primitives_clp.cpp
primitives/core_primitives_aad.cpp
primitives/core_primitives_stats.cpp
```

---

## Task 1: Establish Baseline

Code changes:
1. None.

Tests:
1. `eta_core_test`
2. Targeted:
   - `runtime_primitives_tests`
   - `builtin_sync_tests`
   - `atom_tests`
   - `csv_reader_tests`
   - `csv_writer_tests`
3. Record build/test timing baseline.

Done when:
1. Baseline is green and timing notes are captured.

Task 1 implementation notes (2026-05-14):
1. Baseline build command (with `ETA_MODULE_PATH` unset):
   - `cmd /d /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && "C:\Program Files\JetBrains\CLion 2026.1\bin\cmake\win\x64\bin\cmake.exe" --build C:\Users\lewis\develop\eta\out\msvc-release --target eta_all -j 6'`
2. Baseline timing snapshot:
   - `eta_all` build: `35.16s`
   - targeted suites (`runtime_primitives_tests`, `builtin_sync_tests`,
     `atom_tests`, `csv_reader_tests`, `csv_writer_tests`): `0.49s`
   - stage gate (`ctest -R "eta_core_test|eta_stdlib_tests"`): `105.72s`
     (`eta_core_test`: `42.28s`, `eta_stdlib_tests`: `63.41s`)
3. Stage gate result:
   - `eta_core_test`: pass
   - `eta_stdlib_tests`: pass
4. Baseline and stage-gate runs in this repo should unset `ETA_MODULE_PATH`
   for the test process so external install/module overrides do not affect
   results.

---

## Task 2: Introduce Public/Private Split Skeleton (no behavior change)

Code changes:
1. Create `eta/core/src/eta/runtime/core_primitives_internal.h`:
   - Declare `struct PrimReg` with references to `env`, `heap`, `intern_table`, `vm`.
   - Declare domain methods (`register_arithmetic()`, ..., `register_stats()`).
2. Create `eta/core/src/eta/runtime/core_primitives.cpp`:
   - Define `register_core_primitives(...)` dispatcher.
   - Temporarily keep legacy behavior via one internal method
     (for example `PrimReg::register_legacy_block()`).
3. Convert `core_primitives.h` to declaration-only immediately.
4. Add `core_primitives.cpp` to `eta/core/CMakeLists.txt`.

Tests:
1. `eta_core_test`
2. `builtin_sync_tests`

Done when:
1. Public header no longer contains implementation body.
2. Build and slot-order parity are unchanged.

Task 2 implementation notes (2026-05-14):
1. Public/private split skeleton landed:
   - `eta/core/src/eta/runtime/core_primitives.h` is now declaration-only.
   - Added `eta/core/src/eta/runtime/core_primitives_internal.h` with
     `PrimReg` state and domain method declarations.
   - Added `eta/core/src/eta/runtime/core_primitives.cpp` with:
     - `register_core_primitives(...)` dispatcher
     - `PrimReg` constructor
     - placeholder domain methods (`register_arithmetic()` ... `register_stats()`)
     - temporary legacy implementation path in `PrimReg::register_legacy_block()`.
   - Added `src/eta/runtime/core_primitives.cpp` to `eta/core/CMakeLists.txt`.
2. Added slot-order regression coverage for core registration in:
   - `eta/qa/test/src/builtin_sync_tests.cpp`
   - new test case: `core_registration_matches_metadata_prefix_exactly`.
3. While removing transitive includes from `core_primitives.h`, tests that used
   `classify_numeric(...)` now include `eta/runtime/numeric_value.h` directly:
   - `eta/qa/test/src/csv_reader_tests.cpp`
   - `eta/qa/test/src/csv_fact_table_tests.cpp`
4. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass (`132.6s`)
   - `ctest -R "eta_core_test|eta_stdlib_tests"`: pass (`192.11s`)
     (`eta_core_test`: `64.24s`, `eta_stdlib_tests`: `127.83s`)

---

## Task 3: Extract Shared Helpers into `PrimReg`

Code changes:
1. Move shared helper lambdas into `PrimReg` member/static functions:
   - `has_tape_ref(...)`
   - `allocate_tape_id()`
   - `make_ad_runtime_error(...)`
   - `validate_ref_for_tape(...)`
   - `policy_is_strict()`
   - `make_nondiff_error(...)`
   - `make_domain_error(...)`
   - `make_unary_domain_error(...)`
   - `get_active_tape_for_op(...)`
2. Leave behavior unchanged; move code only.

Tests:
1. `eta_core_test`
2. `builtin_sync_tests`

Done when:
1. Shared helpers are no longer local lambdas in a monolithic block.

Task 3 implementation notes (2026-05-14):
1. Extracted shared AD helper lambdas into `PrimReg` helper methods:
   - Declared in `eta/core/src/eta/runtime/core_primitives_internal.h`.
   - Defined in `eta/core/src/eta/runtime/core_primitives.cpp`.
   - Helpers moved:
     - `has_tape_ref(...)`
     - `allocate_tape_id()`
     - `make_ad_runtime_error(...)`
     - `validate_ref_for_tape(...)`
     - `policy_is_strict(...)`
     - `make_nondiff_error(...)`
     - `make_domain_error(...)`
     - `make_unary_domain_error(...)`
     - `get_active_tape_for_op(...)`
2. Updated legacy registration call sites to use `PrimReg` helper methods
   directly (captures removed for those helpers), with no registration-order or
   behavior change.
3. Added focused regression coverage in
   `eta/qa/test/src/runtime_primitives_tests.cpp`:
   - `aad_tape_comparison_strict_mode_reports_nondiff_error_shape`
   - `aad_taped_pow_strict_mode_reports_domain_error_shape`
4. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass (`91.2s`)
   - targeted suites via `eta_core_test`:
     - `runtime_primitives_tests/*`: pass
     - `builtin_sync_tests/*`: pass
   - stage gate `ctest -R "eta_core_test|eta_stdlib_tests"`: pass (`195.36s`)
   - full `ctest`: pass (`9/9`, `203.94s`)

---

## Task 4: Extract Arithmetic + Predicates (`arithmetic`)

Code changes:
1. Add `core_primitives_arithmetic.cpp`.
2. Move:
   - `+` `-` `*` `/`
   - `=` `<` `>` `<=` `>=`
   - `eq?` `eqv?` `not`
   - `number?` `boolean?` `string?` `char?` `symbol?` `procedure?` `integer?`
   - `zero?` `positive?` `negative?`
   - `abs` `min` `max` `modulo` `remainder`
3. Add `PrimReg::register_arithmetic()` and call it from dispatcher in exact current order.

Tests:
1. `runtime_primitives_tests`
2. `builtin_sync_tests`

Done when:
1. Arithmetic/predicate registrations are out of the legacy block with no slot drift.

Task 4 implementation notes (2026-05-14):
1. Added arithmetic split unit and local helper header:
   - `eta/core/src/eta/runtime/core_primitives_arithmetic.cpp`
   - `eta/core/src/eta/runtime/core_primitives_arithmetic_helpers.h`
2. Extracted arithmetic/predicate registrations from the legacy block into
   `PrimReg::register_arithmetic()`:
   - `+` `-` `*` `/`
   - `=` `<` `>` `<=` `>=`
   - `eq?` `eqv?` `not`
   - `number?` `boolean?` `string?` `char?` `symbol?` `procedure?` `integer?`
   - `zero?` `positive?` `negative?`
   - `abs` `min` `max` `modulo` `remainder`
3. Dispatcher wiring:
   - `register_core_primitives(...)` now calls `reg.register_arithmetic();`
     before `reg.register_legacy_block();`.
4. Slot-order bridge:
   - Added `PrimReg::register_pair_list_bridge()` for
     `cons` `car` `cdr` `pair?` `null?` `list` so slot order remains unchanged
     around the arithmetic/type-predicate boundary while Task 4 is isolated.
5. Build wiring:
   - Added `src/eta/runtime/core_primitives_arithmetic.cpp` to
     `eta/core/CMakeLists.txt`.
6. Regression tests added:
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `arithmetic_primitives_preserve_numeric_and_predicate_behavior`
     - `aad_taped_min_strict_mode_reports_nondiff_error_shape`
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_arithmetic_and_predicate_window_matches_seeded_slots`
7. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass (`163.7s`)
   - targeted runtime suites:
     - `eta_core_test --run_test=runtime_primitives_tests/*:builtin_sync_tests/*`: pass (`1.9s`)
   - stage gate `ctest -R "eta_core_test|eta_stdlib_tests"`: pass (`184.17s`)
     (`eta_core_test`: `64.24s`, `eta_stdlib_tests`: `119.89s`)
   - full `ctest`: pass (`9/9`, `183.47s`)

---

## Task 5: Extract Transcendental + AAD Policy (`math`)

Code changes:
1. Add `core_primitives_math.cpp`.
2. Move:
   - `sin` `cos` `tan` `asin` `acos` `atan` `exp` `log` `sqrt` `pow`
   - `set-aad-nondiff-policy!`
   - `aad-nondiff-policy`
3. Keep `pow` branch behavior identical (strict policy and domain handling).

Tests:
1. `runtime_primitives_tests`
2. `builtin_sync_tests`
3. `aad.test.eta` via example runner

Done when:
1. Math and policy registrations live in `core_primitives_math.cpp`.

Task 5 implementation notes (2026-05-14):
1. Added math split unit and local helper header:
   - `eta/core/src/eta/runtime/core_primitives_math.cpp`
   - `eta/core/src/eta/runtime/core_primitives_math_helpers.h`
2. Extracted transcendental and AAD policy registrations from the legacy block
   into `PrimReg::register_math()`:
   - `sin` `cos` `tan` `asin` `acos` `atan` `exp` `log` `sqrt` `pow`
   - `set-aad-nondiff-policy!`
   - `aad-nondiff-policy`
3. Dispatcher and build wiring:
   - `register_core_primitives(...)` now calls `reg.register_math();`
     immediately after `reg.register_arithmetic();`.
   - Added `src/eta/runtime/core_primitives_math.cpp` to
     `eta/core/CMakeLists.txt`.
4. `pow` tape argument resolution helpers were moved into a dedicated local
   helper header (`core_primitives_math_helpers.h`) to keep shared support
   logic out of public headers.
5. Regression tests added:
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `aad_nondiff_policy_primitives_roundtrip_and_validate_input`
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_math_and_aad_policy_window_matches_seeded_slots`
6. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass (`139.0s`)
   - targeted suites:
     - `eta_core_test --run_test=runtime_primitives_tests/*:builtin_sync_tests/*`: pass
   - stage gate `ctest -R "eta_core_test|eta_stdlib_tests"`: pass (`186.60s`)
     (`eta_core_test`: `67.63s`, `eta_stdlib_tests`: `118.94s`)
   - full `ctest`: pass (`9/9`, `147.42s`)

---

## Task 6: Extract Sequences, Collections, and Atoms (`sequences`)

Code changes:
1. Add `core_primitives_sequences.cpp`.
2. Move:
   - Lists/higher-order/equality:
     `cons` `car` `cdr` `pair?` `null?` `list` `length` `append` `reverse`
     `list-ref` `list-tail` `set-car!` `set-cdr!` `assq` `assoc` `member`
     `apply` `map` `for-each` `equal?`
   - Vectors:
     `vector` `vector-length` `vector-ref` `vector-set!` `vector?` `make-vector`
   - Hash map/set:
     `hash-map` `make-hash-map` `hash-map?` `hash-map-ref` `hash-map-assoc`
     `hash-map-dissoc` `hash-map-keys` `hash-map-values` `hash-map-size`
     `hash-map->list` `list->hash-map` `hash`
     `make-hash-set` `hash-set` `hash-set?` `hash-set-add` `hash-set-remove`
     `hash-set-contains?` `hash-set-union` `hash-set-intersect` `hash-set-diff`
     `hash-set->list` `list->hash-set`
   - Atom ops:
     `%atom-new` `%atom?` `%atom-deref` `%atom-reset!`
     `%atom-compare-and-set!` `%atom-swap!`
3. Keep recursive hash helpers in one TU (`hash_value_impl` and related helpers).

Tests:
1. `runtime_primitives_tests`
2. `atom_tests`
3. `csv_reader_tests`
4. `csv_writer_tests`
5. `builtin_sync_tests`

Done when:
1. Sequence/collection/atom registrations are in `core_primitives_sequences.cpp`.

Task 6 implementation notes (2026-05-14):
1. Added sequence split unit:
   - `eta/core/src/eta/runtime/core_primitives_sequences.cpp`
2. Extracted Task 6 registrations from the legacy block into sequence methods:
   - `PrimReg::register_pair_list_bridge()` now lives in
     `core_primitives_sequences.cpp` and is still invoked from
     `register_arithmetic()` to preserve existing slot order around predicates.
   - `PrimReg::register_sequences()` now owns list/alist operations:
     `length` `append` `reverse` `list-ref` `list-tail` `set-car!` `set-cdr!`
     `assq` `assoc` `member`.
   - Added bridge methods in `core_primitives_sequences.cpp` for in-place
     order-preserving extraction from the legacy flow:
     - `PrimReg::register_sequences_higher_order_bridge()`:
       `apply` `map` `for-each` `equal?`
     - `PrimReg::register_sequences_collections_and_atoms_bridge()`:
       vectors, hash-map/hash-set primitives (including `hash-map-fold` and
       `hash`), and atom primitives.
3. Dispatcher/build wiring:
   - `register_core_primitives(...)` now calls `reg.register_sequences();`
     after `reg.register_math();`.
   - `register_legacy_block()` now invokes:
     - `register_sequences_higher_order_bridge();`
     - `register_sequences_collections_and_atoms_bridge();`
     at the original registration points so slot order remains unchanged.
   - Added method declarations to
     `eta/core/src/eta/runtime/core_primitives_internal.h`.
   - Added `src/eta/runtime/core_primitives_sequences.cpp` to
     `eta/core/CMakeLists.txt`.
4. Regression tests added:
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_sequences_and_collections_windows_match_seeded_slots`
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `sequence_collection_primitives_preserve_list_map_vector_hash_and_equal_behavior`
5. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass (`141.7s`)
   - full `ctest --output-on-failure`: pass (`9/9`, `154.39s`)

---

## Task 7: Extract Strings, Symbols, and Delegates (`strings`)

Code changes:
1. Add `core_primitives_strings.cpp`.
2. Move:
   - `symbol->string` `string->symbol`
   - `string-length` `string-append` `number->string` `string->number`
   - `string-ref` `substring`
   - `string=?` `string<?` `string>?` `string<=?` `string>=?`
   - `char->integer` `integer->char`
3. Move delegated registration calls in-place order:
   - `register_csv_builtins(...)`
   - `register_regex_builtins(...)`
   - `register_json_builtins(...)`

Tests:
1. `runtime_primitives_tests`
2. `regex_tests`
3. `json_tests`
4. `csv_reader_tests`
5. `csv_writer_tests`
6. `builtin_sync_tests`

Done when:
1. String/symbol/delegate registrations live in `core_primitives_strings.cpp`.

Task 7 implementation notes (2026-05-14):
1. Added string split unit and local helper header:
   - `eta/core/src/eta/runtime/core_primitives_strings.cpp`
   - `eta/core/src/eta/runtime/core_primitives_strings_helpers.h`
2. Extracted string/symbol/char and delegated registration calls from the
   legacy block into `PrimReg::register_strings()`:
   - `symbol->string` `string->symbol`
   - `string-length` `string-append` `number->string` `string->number`
   - `string-ref` `substring`
   - `string=?` `string<?` `string>?` `string<=?` `string>=?`
   - `char->integer` `integer->char`
   - `register_csv_builtins(...)`
   - `register_regex_builtins(...)`
   - `register_json_builtins(...)`
3. Dispatcher/build wiring:
   - `register_core_primitives(...)` now calls `reg.register_strings();`
     immediately after `reg.register_sequences();`.
   - Added `src/eta/runtime/core_primitives_strings.cpp` to
     `eta/core/CMakeLists.txt`.
   - Removed migrated string/delegate registrations from
     `register_legacy_block()` while keeping the remaining bridge call order
     unchanged.
4. Regression tests added:
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `string_symbol_and_char_primitives_preserve_conversion_and_comparison_behavior`
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_strings_and_delegate_windows_match_seeded_slots`
5. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass
   - targeted suites:
     - `eta_core_test --run_test=runtime_primitives_tests/*:regex_tests/*:json_tests/*:csv_reader_tests/*:csv_writer_tests/*:builtin_sync_tests/*`: pass
   - full `ctest --output-on-failure`: pass (`9/9`, `140.49s`)

---

## Task 8: Extract Misc Runtime Builtins (`misc`)

Code changes:
1. Add `core_primitives_misc.cpp`.
2. Move:
   - `error`
   - `platform`
   - `%prof-start` `%prof-stop` `%prof-report`
   - `%prof-counter` `%prof-region-enter` `%prof-region-exit` `%prof-enabled?`
   - `register-finalizer!` `unregister-finalizer!`
   - `make-guardian` `guardian-track!` `guardian-collect`
   - `eval` runtime stub
3. Keep VM/profiler wiring unchanged.

Tests:
1. `runtime_primitives_tests`
2. `atom_tests`
3. `builtin_sync_tests`

Done when:
1. Misc registrations are isolated in `core_primitives_misc.cpp`.

Task 8 implementation notes (2026-05-14):
1. Added misc split unit:
   - `eta/core/src/eta/runtime/core_primitives_misc.cpp`
2. Extracted misc builtins from the legacy block into misc methods:
   - `PrimReg::register_misc()`:
     `error`, `platform`,
     `%prof-start`, `%prof-stop`, `%prof-report`,
     `%prof-counter`, `%prof-region-enter`, `%prof-region-exit`,
     `%prof-enabled?`
   - `PrimReg::register_misc_lifecycle_bridge()`:
     `register-finalizer!`, `unregister-finalizer!`,
     `make-guardian`, `guardian-track!`, `guardian-collect`
   - `PrimReg::register_misc_eval_bridge()`:
     `eval` runtime stub
3. Order-preserving wiring:
   - `register_legacy_block()` now calls `register_misc()` after
     `register_sequences_collections_and_atoms_bridge()` so
     `error/platform/profiler` stay in the same slot region.
   - Kept `logic-var?` in legacy for Task 9 and inserted
     `register_misc_lifecycle_bridge()` immediately after it to preserve
     catalog order around lifecycle builtins.
   - Replaced the legacy `eval` stub registration with
     `register_misc_eval_bridge()` at the original end-of-block location.
4. Build wiring:
   - Added `src/eta/runtime/core_primitives_misc.cpp` to
     `eta/core/CMakeLists.txt`.
   - Added internal method declarations in
     `eta/core/src/eta/runtime/core_primitives_internal.h` for
     `register_misc_lifecycle_bridge()` and `register_misc_eval_bridge()`.
5. Regression tests added:
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `misc_primitives_preserve_runtime_error_profiler_guardian_and_eval_behavior`
   - `eta/qa/test/src/atom_tests.cpp`
     - `finalizer_and_guardian_primitives_roundtrip_basic_lifecycle`
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_misc_windows_match_seeded_slots`
6. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass
   - targeted suites:
     - `eta_core_test --run_test=runtime_primitives_tests/*:atom_tests/*:builtin_sync_tests/*`: pass
   - full `ctest --output-on-failure`: pass (`9/9`, `153.48s`)

---

## Task 9: Extract Logic and Attributes (`logic`)

Code changes:
1. Add `core_primitives_logic.cpp`.
2. Move:
   - `logic-var?`
   - `put-attr` `get-attr` `del-attr` `attr-var?` `register-attr-hook!`
   - `logic-var/named` `var-name`
   - `set-occurs-check!` `occurs-check-mode`
   - `ground?` `compound?` `term` `functor` `arity` `arg`
   - `dual?` `dual-primal` `dual-backprop` `make-dual`
   - `register-prop-attr!`
3. Keep trail and backtracking semantics identical.

Tests:
1. `runtime_primitives_tests`
2. logic/unification example runner tests
3. `builtin_sync_tests`

Done when:
1. Logic/attr registrations are in `core_primitives_logic.cpp`.

Task 9 implementation notes (2026-05-14):
1. Added logic split unit and local helper header:
   - `eta/core/src/eta/runtime/core_primitives_logic.cpp`
   - `eta/core/src/eta/runtime/core_primitives_logic_helpers.h`
2. Extracted Task 9 logic/attribute registrations from the legacy block into:
   - `PrimReg::register_logic()`:
     - `logic-var?`
     - `put-attr` `get-attr` `del-attr` `attr-var?` `register-attr-hook!`
     - `logic-var/named` `var-name`
     - `set-occurs-check!` `occurs-check-mode`
     - `ground?` `compound?` `term` `functor` `arity` `arg`
     - `dual?` `dual-primal` `dual-backprop` `make-dual`
   - `PrimReg::register_logic_prop_attr_bridge()`:
     - `register-prop-attr!` (kept at its original late-slot position).
3. Order-preserving wiring:
   - `register_legacy_block()` now calls `register_logic();` at the original
     logic region.
   - `register_logic()` invokes `register_misc_lifecycle_bridge()` in-place so
     finalizer/guardian slot order remains unchanged.
   - Replaced legacy `register-prop-attr!` registration with
     `register_logic_prop_attr_bridge()` immediately before
     `%clp-prop-queue-size`.
4. Build wiring:
   - Added `src/eta/runtime/core_primitives_logic.cpp` to
     `eta/core/CMakeLists.txt`.
   - Added `register_logic_prop_attr_bridge()` declaration in
     `eta/core/src/eta/runtime/core_primitives_internal.h`.
5. Regression tests added:
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `logic_primitives_preserve_attr_occurs_term_dual_and_prop_attr_behavior`
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_logic_windows_match_seeded_slots`
6. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass
   - targeted suites:
     - `eta_core_test --run_test=runtime_primitives_tests/*:builtin_sync_tests/*`: pass
     - `eta_core_test --run_test=vm_tests/unification_tests/*`: pass
     - `eta_core_test --run_test=example_runner_tests/*:compiled_example_tests/*`: pass

---

## Task 10: Extract CLP (`clp`)

Code changes:
1. Add `core_primitives_clp.cpp`.
2. Move all CLP builtins:
   - `%clp-domain-z!` `%clp-domain-fd!` `%clp-domain-r!`
   - `%clp-get-domain` `%clp-linearize`
   - `%clp-fm-feasible?` `%clp-fm-bounds`
   - `%clp-r-*`
   - `%clp-fd-*`
   - `%clp-bool-*`
   - `%clp-prop-queue-size`
3. Keep CLP helper closures (`narrow_var`, bounds helpers, propagators, FM/LP helpers)
   in this TU only.

Tests:
1. `runtime_primitives_tests` (CLP subset)
2. CLP example runner tests (`nqueens.eta`, `send-more-money.eta`, etc.)
3. `builtin_sync_tests`

Done when:
1. CLP registrations exist only in `core_primitives_clp.cpp`.

Task 10 implementation notes (2026-05-14):
1. Added CLP split unit:
   - `eta/core/src/eta/runtime/core_primitives_clp.cpp`
2. Extracted all CLP registrations from `register_legacy_block()` into:
   - `PrimReg::register_clp()`:
     `%clp-domain-z!`, `%clp-domain-fd!`, `%clp-domain-r!`,
     `%clp-get-domain`, `%clp-linearize`,
     `%clp-fm-feasible?`, `%clp-fm-bounds`,
     `%clp-r-post-leq!`, `%clp-r-post-eq!`, `%clp-r-propagate!`,
     `%clp-r-minimize`, `%clp-r-maximize`,
     `%clp-r-qp-minimize`, `%clp-r-qp-maximize`,
     `%clp-fd-plus!`, `%clp-fd-plus-offset!`, `%clp-fd-abs!`,
     `%clp-fd-times!`, `%clp-fd-sum!`, `%clp-fd-scalar-product!`,
     `%clp-fd-element!`, `%clp-fd-all-different!`,
     `%clp-bool-and!`, `%clp-bool-or!`, `%clp-bool-xor!`,
     `%clp-bool-imp!`, `%clp-bool-eq!`, `%clp-bool-not!`,
     `%clp-bool-card!`.
   - `PrimReg::register_clp_prop_queue_size_bridge()`:
     `%clp-prop-queue-size`.
3. Order-preserving wiring:
   - `register_legacy_block()` now calls `register_clp();` at the original
     CLP slot region (between logic and tape/AAD sections).
   - Replaced inline `%clp-prop-queue-size` registration with
     `register_clp_prop_queue_size_bridge();` at the existing late-slot
     position, immediately after `register_logic_prop_attr_bridge();`.
4. Build wiring:
   - Added `src/eta/runtime/core_primitives_clp.cpp` to
     `eta/core/CMakeLists.txt`.
   - Added `register_clp_prop_queue_size_bridge()` declaration in
     `eta/core/src/eta/runtime/core_primitives_internal.h`.
5. Regression tests added:
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `clp_primitives_preserve_domain_lookup_and_queue_size_behavior`
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_clp_windows_match_seeded_slots`
6. Stage-gate stabilization during full test run:
   - Fixed Windows SHA-256 parser line filtering in
     `eta/cli/src/eta/cli/main_eta.cpp` so `eta vendor` only accepts digest
     lines (hex + whitespace), avoiding false matches from non-digest text.
   - Updated `eta/qa/cli_test/src/eta_cli_test.cpp` sidecar fixture hashing to
     use the same `certutil` digest source as CLI vendor verification.
7. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass
   - `ctest --output-on-failure`: pass (`9/9`, `150.27s`)

---

## Task 11: Extract Tape/AAD Control (`aad`)

Code changes:
1. Add `core_primitives_aad.cpp`.
2. Move:
   - `tape-new` `tape-start!` `tape-stop!` `tape-clear!`
   - `tape-var` `tape-backward!` `tape-adjoint` `tape-primal`
   - `tape-ref?` `tape-ref-index`
   - `tape-size` `tape-ref-value-of` `tape-ref-value`

Tests:
1. `runtime_primitives_tests`
2. `aad.test.eta` via example runner
3. `builtin_sync_tests`

Done when:
1. Tape control registrations are in `core_primitives_aad.cpp`.

Task 11 implementation notes (2026-05-14):
1. Added AAD split unit and local helper header:
   - `eta/core/src/eta/runtime/core_primitives_aad.cpp`
   - `eta/core/src/eta/runtime/core_primitives_aad_helpers.h`
2. Extracted all tape control registrations from `register_legacy_block()`
   into `PrimReg::register_aad()`:
   - `tape-new` `tape-start!` `tape-stop!` `tape-clear!`
   - `tape-var` `tape-backward!` `tape-adjoint` `tape-primal`
   - `tape-ref?` `tape-ref-index`
   - `tape-size` `tape-ref-value-of` `tape-ref-value`
3. Order-preserving wiring:
   - `register_legacy_block()` now calls `register_aad();` immediately after
     `register_clp();` at the existing tape/AAD slot region.
   - Removed the inline tape registration block from
     `eta/core/src/eta/runtime/core_primitives.cpp`.
4. Build wiring:
   - Added `src/eta/runtime/core_primitives_aad.cpp` to
     `eta/core/CMakeLists.txt`.
5. Regression tests added:
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `aad_tape_control_primitives_preserve_lifecycle_and_reference_access_behavior`
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_aad_window_matches_seeded_slots`
6. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass (`119.7s`)
   - targeted suites:
     - `eta_core_test --run_test=runtime_primitives_tests/*:builtin_sync_tests/*:example_runner_tests/*:compiled_example_tests/*`: pass
   - full `ctest --output-on-failure`: pass (`9/9`, `118.70s`)

---

## Task 12: Extract Fact-table and Stats (`stats`)

Code changes:
1. Add `core_primitives_stats.cpp`.
2. Move:
   - Fact-table builtins (`%fact-table-*`, `fact-table?`)
   - `term-hash` and `term-variant-hash`
   - `%stats-*`
3. Keep `stats_math.h` / `stats_extract.h` usage local to this TU.

Tests:
1. `runtime_primitives_tests`
2. fact-table/stats example runner tests
3. `stats_tests`
4. `builtin_sync_tests`

Done when:
1. Fact-table/stats registrations are in `core_primitives_stats.cpp`.

Task 12 implementation notes (2026-05-14):
1. Added stats split unit and local helper header:
   - `eta/core/src/eta/runtime/core_primitives_stats.cpp`
   - `eta/core/src/eta/runtime/core_primitives_stats_helpers.h`
2. Extracted Task 12 registrations from the legacy block into
   `PrimReg::register_stats()`:
   - Fact-table builtins (`%fact-table-*`, `fact-table?`)
   - `term-hash` and `term-variant-hash`
   - `%stats-*` builtins
3. Order-preserving wiring:
   - `register_legacy_block()` now calls `register_stats();` immediately after
     `register_aad();`.
   - Kept `register_logic_prop_attr_bridge()`,
     `register_clp_prop_queue_size_bridge()`, and
     `register_misc_eval_bridge()` at their existing late-slot positions.
4. Include-localization and build wiring:
   - `stats_math.h` and `stats_extract.h` are now included only from
     `core_primitives_stats.cpp`.
   - Added `src/eta/runtime/core_primitives_stats.cpp` to
     `eta/core/CMakeLists.txt`.
5. Regression tests added:
   - `eta/qa/test/src/runtime_primitives_tests.cpp`
     - `stats_primitives_preserve_fact_table_hash_and_stats_behavior`
   - `eta/qa/test/src/builtin_sync_tests.cpp`
     - `core_stats_windows_match_seeded_slots`
6. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass (`92.4s`)
   - targeted suites:
     - `eta_core_test --run_test=runtime_primitives_tests/*:builtin_sync_tests/*:stats_tests/*:example_runner_tests/*:compiled_example_tests/*`: pass (`20.6s`)
   - full `ctest --output-on-failure`: pass (`9/9`, `122.21s`)

---

## Task 13: Finalize Dispatcher-only Runtime Path

Code changes:
1. Remove the temporary legacy registration block from `core_primitives.cpp`.
2. Ensure dispatcher call order exactly matches current registration order.
3. Add all new files to `eta/core/CMakeLists.txt`:
   - `core_primitives.cpp`
   - 9 domain `.cpp` files
4. Keep `core_primitives_internal.h` private to `eta_core` target.

Tests:
1. `eta_core_test`
2. `builtin_sync_tests`
3. smoke runs: `etai`, `etac`, `eta_repl`, `eta_lsp`
4. build-time comparison vs baseline

Done when:
1. Public header is declaration-only and legacy block is gone.
2. Slot order parity is preserved.

Task 13 implementation notes (2026-05-14):
1. Removed the temporary legacy path from core registration:
   - Deleted `PrimReg::register_legacy_block()` from:
     - `eta/core/src/eta/runtime/core_primitives_internal.h`
     - `eta/core/src/eta/runtime/core_primitives.cpp`
2. Dispatcher is now fully explicit and order-preserving:
   - `register_core_primitives(...)` directly invokes:
     - `register_arithmetic()`
     - `register_math()`
     - `register_sequences()`
     - `register_strings()`
     - `register_sequences_collections_and_atoms_bridge()`
     - `register_misc()`
     - `register_logic()`
     - `register_clp()`
     - `register_aad()`
     - `register_stats()`
     - `register_logic_prop_attr_bridge()`
     - `register_clp_prop_queue_size_bridge()`
     - `register_misc_eval_bridge()`
3. Added Task 13 regression coverage in:
   - `eta/qa/test/src/builtin_sync_tests.cpp`
   - strengthened `core_registration_matches_metadata_prefix_exactly` to assert
     core registration spans the full metadata prefix through the `eval` slot.
4. Build/source wiring check:
   - `eta/core/CMakeLists.txt` includes `core_primitives.cpp` and all split
     domain units:
     `primitives/core_primitives_arithmetic.cpp`,
     `primitives/core_primitives_math.cpp`,
     `primitives/core_primitives_sequences.cpp`,
     `primitives/core_primitives_strings.cpp`,
     `primitives/core_primitives_misc.cpp`,
     `primitives/core_primitives_logic.cpp`,
     `primitives/core_primitives_clp.cpp`,
     `primitives/core_primitives_aad.cpp`,
     `primitives/core_primitives_stats.cpp`.
5. Validation run (with `ETA_MODULE_PATH` unset):
   - `eta_all` build: pass (`99.8s`)
   - stage gate: `ctest --output-on-failure -R "eta_core_test|eta_stdlib_tests"`:
     pass (`99.79s`)
   - smoke runs: `etai --help`, `etac --help`, `eta_repl --help`, `eta_lsp --help`:
     pass
   - full `ctest --output-on-failure`: pass (`9/9`, `97.97s`)

Task 13 layout cleanup notes (2026-05-14):
1. Moved split domain registration units into:
   - `eta/core/src/eta/runtime/primitives/`
2. Kept dispatcher/public-private surface in:
   - `eta/core/src/eta/runtime/core_primitives.h`
   - `eta/core/src/eta/runtime/core_primitives_internal.h`
   - `eta/core/src/eta/runtime/core_primitives.cpp`
3. Updated helper includes to use:
   - `eta/runtime/primitives/core_primitives_*_helpers.h`
4. Updated `eta/core/CMakeLists.txt` source entries to:
   - `src/eta/runtime/primitives/core_primitives_*.cpp`

---

## Task 14: Include Fan-out Guard

Code changes:
1. Add CI/script guard (or CMake custom target) to assert:
   - `core_primitives.h` does not include (directly or transitively):
     `eta/runtime/clp/*`, `eta/runtime/types/tape.h`,
     `eta/runtime/prof/profiler.h`, `eta/runtime/stats_math.h`.
2. Implement guard via `clang -H`/`/showIncludes` output parsing or equivalent include graph tooling.

Tests:
1. Run guard in CI on each PR touching core primitive registration.

Done when:
1. Guard is committed and passing.

---

## Minimal Merge Policy

1. One task per PR (adjacent extraction tasks may be paired only if they do not
   share helper migration risk).
2. Every task must pass `runtime_primitives_tests` and `builtin_sync_tests`.
3. Task order is strict for extraction tasks; Task 13 only after Tasks 4-12.
4. Behavior changes are forbidden in this plan.
5. Registration order is a hard invariant; any slot drift blocks merge.

---

## Expected Outcomes

| Metric | Before | After (expected) |
|---|---:|---:|
| `core_primitives.h` size | 6,729 lines | declaration-only (`<= 40` lines) |
| TUs parsing full monolithic implementation | ~20+ | 0 |
| Runtime source files for core primitive registration | 0 | 10 |
| Test TUs transitively seeing `clp/` via `core_primitives.h` | ~20+ | 0 |
| `register_core_primitives` implementation shape | 1 giant inline function | 1 dispatcher + 9 domain methods |

---

## Relationship to Other Plans

- **`deduplication_plan.md` (Tasks 11-13):**
  Already implemented in this branch. Preserve extracted AAD helper behavior
  while moving code into split TUs.
- **`std_bitset_plan.md`:**
  If bitset primitives land before this split, keep their placement consistent
  with current registration order; after split, place them in a dedicated domain
  file and update catalog order checks accordingly.
