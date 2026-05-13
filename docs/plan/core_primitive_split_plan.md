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
core_primitives_arithmetic.cpp
core_primitives_math.cpp
core_primitives_sequences.cpp
core_primitives_strings.cpp
core_primitives_misc.cpp
core_primitives_logic.cpp
core_primitives_clp.cpp
core_primitives_aad.cpp
core_primitives_stats.cpp
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
