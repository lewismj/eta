# `core_primitives.h` Split Plan

Status: **draft / proposed**

Objective:
1. Break the 6 730-line monolithic `register_core_primitives` function into 8–9 focused
   domain units, each with its own `.h`/`.cpp` pair (or `.h`-only thin delegator).
2. Reduce per-translation-unit include cost for the ~20+ test files that currently
   include all of `core_primitives.h` just to call `register_core_primitives`.
3. Keep public API at `core_primitives.h` identical: a single
   `register_core_primitives(env, heap, intern_table, vm*)` declaration.
4. No behavior change; all tests must remain green.

---

## Background

`core_primitives.h` is a single 6 730-line header containing the _entire body_ of
`register_core_primitives` — 219 `env.register_builtin(...)` calls spanning:

| Domain | Approx. line range | Approx. builtins |
|---|---|---|
| Shared helpers (tape ID allocator, `has_tape_ref`, …) | 86–172 | — |
| Arithmetic + AAD (`+` `-` `*` `/`, comparison, `abs`, …) | 165–755 | ~25 |
| Transcendental / AAD unary (`sin` `cos` `exp` `log` `sqrt` `pow` …) | 756–1068 | ~15 |
| Lists, higher-order (`length`, `append`, `map`, `apply`, `equal?`, …) | 1069–1773 | ~30 |
| Strings, symbols | 1774–1893 | ~20 |
| Delegated builtins (CSV, Regex, JSON, delegate calls) | 1894–1925 | ~3 delegate |
| Vectors, HashMaps, HashSets, sorting, hash-value | 1926–2568 | ~50 |
| Error, platform, profiler, atoms, finalizers | 2568–2899 | ~15 |
| Logic variables, compound terms, unification | 2780–3214 | ~25 |
| CLP domains, linearize, FM, CLP(R) posting | 3215–4776 | ~30 |
| CLP(FD) propagators (`%clp-fd-*`) | 4777–5200 | ~12 |
| Boolean CLP (`%bool-*`) | 5201–5700 | ~15 |
| Tape / AAD control (`make-tape`, `tape-backward!`, `tape-ref-value`, …) | 5700–5900 | ~12 |
| Stats / tensor / fact-tables / groupby | 5900–6730 | ~12 |

### Why a naive `.cpp` move fails

`register_core_primitives` contains lambdas that capture each other
(e.g. `hash_value_impl` captures itself via `shared_ptr`; `narrow_bool` and
`propagate_ternary` in the FD block; `make_ad_runtime_error` captured by
`make_nondiff_error`). Because lambda types are anonymous and local, you cannot
forward-declare them across files.

### Chosen approach: `struct PrimReg` dispatcher

Introduce a local `struct PrimReg` in a new `core_primitives.cpp`.  It holds
`env`, `heap`, `intern_table`, and `vm*` as members, and its member functions
replace the lambda-clusters that are today inlined into one giant function body.

Each domain gets its own `core_primitives_<domain>.cpp`.  A single thin
`core_primitives.h` exposes only the public `register_core_primitives` declaration.

```
core_primitives.h                   ← public declaration (≤15 lines)
core_primitives.cpp                 ← PrimReg definition, dispatcher
core_primitives_arithmetic.cpp      ← +  -  *  /  comparison  abs  min  max  …
core_primitives_math.cpp            ← sin cos exp log sqrt pow asin acos atan …
core_primitives_sequences.cpp       ← cons car cdr list vector hashmap hashset sort …
core_primitives_strings.cpp         ← string-length substring number->string …
core_primitives_logic.cpp           ← logic-var  compound  unify  ground? …
core_primitives_clp.cpp             ← %clp-domain-z!  %clp-r-*  %clp-fd-*  %bool-* …
core_primitives_aad.cpp             ← make-tape  tape-backward!  tape-ref-value  …
core_primitives_misc.cpp            ← error  platform  profiler  atoms  finalizers
```

Each `.cpp` includes only the headers it actually needs, breaking the current
fan-out that forces every test TU to parse `clp/`, `types/tape.h`, `<random>`,
`<sstream>`, etc.

---

## Task 1: Establish Baseline

Code changes:
1. None.

Tests:
1. Run `eta_core_test` (full suite).
2. Run targeted:
   - `runtime_primitives_tests`
   - `builtin_sync_tests`
   - `atom_tests`
   - `csv_reader_tests`, `csv_writer_tests`
3. Record test counts and build duration as baseline.

Done when:
1. Baseline is green and timings are captured.

---

## Task 2: Introduce `PrimReg` Skeleton (no behavior change)

Code changes:
1. Create `eta/core/src/eta/runtime/core_primitives.cpp`:
   ```cpp
   #include "eta/runtime/core_primitives.h"
   // domain headers will be added in later tasks
   namespace eta::runtime {
   struct PrimReg {
       BuiltinEnvironment& env;
       Heap& heap;
       InternTable& intern_table;
       vm::VM* vm;
   };
   void register_core_primitives(BuiltinEnvironment& env, Heap& heap,
                                  InternTable& intern_table, vm::VM* vm) {
       // body still lives in the header for now — will migrate per task
       register_core_primitives_inline(env, heap, intern_table, vm);
   }
   } // namespace eta::runtime
   ```
2. Rename the current `inline void register_core_primitives(...)` in the header
   to `inline void register_core_primitives_inline(...)` temporarily.
3. Add `core_primitives.cpp` to the `eta_core` CMake target.

Tests:
1. Full `eta_core_test`.
2. Confirm `register_core_primitives_inline` is only called from `core_primitives.cpp`.

Done when:
1. Build is green; `register_core_primitives_inline` symbol not visible outside
   `eta_core`.

---

## Task 3: Extract Shared Helpers into `PrimReg` Member Functions

Code changes:
1. Move the following from `register_core_primitives_inline` into `PrimReg` member
   functions declared in `core_primitives.cpp`:
   - `has_tape_ref(Args)` → `PrimReg::has_tape_ref(Args)`
   - `allocate_tape_id()` → `static PrimReg::allocate_tape_id()` (keeps the
     `static std::atomic` behavior)
   - `make_ad_runtime_error(...)` → `PrimReg::make_ad_runtime_error(...)`
   - `validate_ref_for_tape(...)` → thin wrapper delegating to
     `detail::aad_unary::validate_tape_ref_for_op` (already extraced)
   - `policy_is_strict()`, `make_nondiff_error(...)`, `make_domain_error(...)`,
     `make_unary_domain_error(...)`, `get_active_tape_for_op(...)` →
     `PrimReg` member functions
2. Update call sites inside `register_core_primitives_inline` to use `this->X()`
   notation (or, since the lambdas are still local, pass `PrimReg*` into the inline
   body through a local variable).

Tests:
1. Full `eta_core_test`.

Done when:
1. Shared helpers are member functions; all call sites confirmed by grep.

---

## Task 4: Extract Arithmetic + Comparison + AAD Binary (`arithmetic`)

Code changes:
1. Add `core_primitives_arithmetic.cpp`.
2. Move builtins for: `+` `-` `*` `/` `=` `<` `>` `<=` `>=` `eq?` `eqv?`
   `not` `zero?` `positive?` `negative?` `abs` `min` `max` `modulo`
   `remainder` `quotient` `gcd` `lcm` `expt` `floor` `ceiling` `truncate`
   `round` `exact->inexact` `inexact->exact` `exact?` `inexact?`
   `integer->char` `char->integer` `number?` `integer?` `boolean?`
   (lines ~165–755 of current header).
3. Shared lambdas that exist only in this cluster (`make_comparison`,
   `make_numeric_predicate`) become `static` helpers at file scope in
   `core_primitives_arithmetic.cpp`.
4. Add `void PrimReg::register_arithmetic()` declaration to the struct.
5. Call `reg.register_arithmetic()` from the dispatcher in `core_primitives.cpp`.
6. Remove these builtins from `register_core_primitives_inline`.

Tests:
1. `runtime_primitives_tests`.
2. `builtin_sync_tests`.

Done when:
1. Arithmetic builtins are in `core_primitives_arithmetic.cpp`; green tests.

---

## Task 5: Extract Transcendental / AAD Unary Math (`math`)

Code changes:
1. Add `core_primitives_math.cpp`.
2. Move: `sin` `cos` `tan` `asin` `acos` `atan` `exp` `log` `log2` `log10`
   `sqrt` `cbrt` `pow` `hypot` `floor` `ceil` `round` `truncate`
   (lines ~756–1068).
3. The domain-error helpers (`make_unary_domain_error`, `make_domain_error`) are
   already `PrimReg` members after Task 3.
4. Note on `pow`: Keep all special-case branches (negative base, integer exponent
   fast path, strict-mode) explicit; do not hide behind a generic helper.
   The deduplication plan (Tasks 11–13) can revisit `sin`/`cos`/`exp` later; this
   task only moves existing code.
5. Add `void PrimReg::register_math()`.

Tests:
1. `runtime_primitives_tests`.
2. Stdlib `aad.test.eta` via cookbook example runner.

Done when:
1. Math builtins in `core_primitives_math.cpp`; AAD backward passes match baseline.

---

## Task 6: Extract Sequences — Lists, Vectors, HashMaps, HashSets (`sequences`)

Code changes:
1. Add `core_primitives_sequences.cpp`.
2. Move:
   - List: `cons` `car` `cdr` `pair?` `null?` `list` `length` `append`
     `reverse` `list-ref` `list-tail` `set-car!` `set-cdr!` `assq` `assoc`
     `member` `list-copy` `list->vector` `vector->list`
   - Higher-order: `apply` `map` `for-each` `filter` `foldl` `foldr`
   - Deep equality: `equal?`
   - Vectors: `vector` `vector?` `make-vector` `vector-length` `vector-ref`
     `vector-set!` `vector-fill!` `vector-copy` `vector-map` `vector-for-each`
   - HashMaps/HashSets: `make-hashmap` `hashmap-set!` `hashmap-ref` etc.
   - Sorting: `sort` `sort!` `list-sort`
   - Hash operations: `hash-value` `hash-value/logic` (lines ~1926–2568)
   (together: lines ~1069–2568)
3. **Key constraint:** `hash_value_impl` is a recursive `shared_ptr<std::function<...>>`
   lambda that captures itself. It must remain in one TU. Move it as a
   `static` file-scope variable initialized on first use inside
   `core_primitives_sequences.cpp`.  Alternatively, convert it to a regular
   recursive static function `static uint64_t hash_value_impl(...)`.
4. Add `void PrimReg::register_sequences()`.

Tests:
1. `runtime_primitives_tests`.
2. `atom_tests`.
3. `csv_reader_tests`, `csv_writer_tests` (only need sequences, should now compile
   without CLP/tape headers).

Done when:
1. Sequences in own `.cpp`; no include fan-out of `clp/` into sequence test TUs.

---

## Task 7: Extract Strings and Symbols (`strings`)

Code changes:
1. Add `core_primitives_strings.cpp`.
2. Move: `string?` `string-length` `string-append` `string-ref` `substring`
   `string->list` `list->string` `string->symbol` `symbol->string`
   `string->number` `number->string` `string-upcase` `string-downcase`
   `string-contains` `string-split` `string-join` `char?` `char->integer`
   `integer->char` `symbol?` `string-copy` `string=?` `string<?`
   (lines ~1774–1893).
3. Add `void PrimReg::register_strings()`.

Tests:
1. `runtime_primitives_tests`.

Done when:
1. String/symbol builtins in own `.cpp`.

---

## Task 8: Extract Misc (error, platform, profiler, atoms, finalizers) (`misc`)

Code changes:
1. Add `core_primitives_misc.cpp`.
2. Move: `error` `platform` `%prof-start` `%prof-stop` `%prof-report`
   `make-atom` `atom?` `atom-ref` `atom-set!` `atom-swap!`
   `register-finalizer!` `unregister-finalizer!` `register-prop-attr!`
   `%clp-prop-queue-size` `eval` (stub)
   (lines ~2568–2779, plus 6707–6726).
3. Add `void PrimReg::register_misc()`.

Tests:
1. `atom_tests`.
2. `runtime_primitives_tests`.

Done when:
1. Misc builtins in own `.cpp`.

---

## Task 9: Extract Logic Variables and Unification (`logic`)

Code changes:
1. Add `core_primitives_logic.cpp`.
2. Move: `logic-var?` `logic-var` `logic-var/named` `var-name` `occurs-check-policy`
   `ground?` `term` `functor` `arity-of` `arg` `compound?` `compound-term?`
   and all `%unify*` / `%walk*` primitives
   (lines ~2780–3214).
3. Add `void PrimReg::register_logic()`.
4. This file needs: `types/logic_var.h`, `types/compound_term.h`, but NOT `clp/`.

Tests:
1. `runtime_primitives_tests`.
2. Any logic/unification cookbook example via example runner.

Done when:
1. Logic builtins in own `.cpp`; CLP headers not included in logic TU.

---

## Task 10: Extract CLP (`clp`)

Code changes:
1. Add `core_primitives_clp.cpp`.
2. Move: all `%clp-domain-z!` `%clp-domain-fd!` `%clp-domain-r!`
   `%clp-get-domain` `%clp-linearize` FM oracle primitives all CLP(R) posting
   primitives (`%clp-r-*`) all CLP(FD) propagators (`%clp-fd-*`) all Boolean
   CLP primitives (`%bool-*`)
   (lines ~3215–5700).
3. Shared CLP locals (`narrow_var`, `extract_bounds`, `walk_list`,
   `r_user_error`, `narrow_bool`, `propagate_ternary`, etc.) become `static`
   file-scope helpers or members of a `struct ClpPrimHelper` local to
   `core_primitives_clp.cpp`.
4. Add `void PrimReg::register_clp()`.
5. `core_primitives_clp.cpp` is the only TU that needs to include the entire
   `clp/` subdirectory.

Tests:
1. `runtime_primitives_tests` (CLP subset).
2. Logic cookbook examples (`nqueens.eta`, `send-more-money.eta`).
3. `%clp-*` invariant tests if present.

Done when:
1. All CLP builtins in own `.cpp`; none of the arithmetic/string/sequence TUs
   pull in `clp/` headers.

---

## Task 11: Extract AAD Tape Control (`aad`)

Code changes:
1. Add `core_primitives_aad.cpp`.
2. Move: `make-tape` `tape?` `tape-size` `tape-ref-value-of` `tape-ref-value`
   `tape-backward!` `tape-grad` `with-tape` `tape-ref?`
   (lines ~5700–5900).
3. These builtins share `get_tape_arg`, `validate_ref_for_tape`,
   `make_ad_runtime_error`; those are already `PrimReg` members after Task 3.
4. Add `void PrimReg::register_aad()`.

Tests:
1. `runtime_primitives_tests`.
2. Stdlib `aad.test.eta`.

Done when:
1. Tape control builtins in own `.cpp`.

---

## Task 12: Extract Stats / Tensor / Fact-Tables (`stats`)

Code changes:
1. Add `core_primitives_stats.cpp`.
2. Move: `%stats-*` `%tensor-*` `%fact-table-*` `%groupby-sum` and related
   (lines ~5900–6730).
3. This TU already delegates to `stats_math.h` / `stats_extract.h`; it needs
   `types/fact_table.h` and Eigen (if used), but not `clp/` or tape headers.
4. Add `void PrimReg::register_stats()`.

Tests:
1. `runtime_primitives_tests`.
2. `fact-table.test.eta`, `portfolio-lp.eta` via example runner.

Done when:
1. Stats/tensor builtins in own `.cpp`.

---

## Task 13: Finalize — Thin Header + Remove Inline Body

Code changes:
1. `register_core_primitives_inline` body in header is now empty (all groups
   migrated).
2. Remove `register_core_primitives_inline` entirely.
3. Replace `core_primitives.h` with a minimal public header:
   ```cpp
   #pragma once
   #include "eta/runtime/builtin_env.h"
   #include "eta/runtime/memory/heap.h"
   #include "eta/runtime/memory/intern_table.h"
   namespace eta::runtime::vm { class VM; }
   namespace eta::runtime {
   void register_core_primitives(BuiltinEnvironment& env,
                                  memory::heap::Heap& heap,
                                  memory::intern::InternTable& intern_table,
                                  vm::VM* vm = nullptr);
   } // namespace eta::runtime
   ```
4. All heavy headers (`clp/`, `types/tape.h`, `<random>`, `<sstream>`,
   `vm/vm.h`, `stats_math.h`, etc.) are now includes in specific `_domain.cpp`
   files only.
5. Add the 8 new `.cpp` files to the `eta_core` CMake target in
   `eta/core/CMakeLists.txt`.

Tests:
1. Full `eta_core_test`.
2. Tooling smoke: `etai`, `etac`, `eta_repl`, `eta_lsp`.
3. Measure build time vs. baseline (expect reduction in incremental rebuild cost
   for any test TU not needing CLP/tape).

Done when:
1. All tests green; `core_primitives.h` includes only the 3 lightweight headers
   above; no test TU transitively includes `clp/` unless it actually uses CLP.

---

## Task 14: Include Fan-out Validation

Code changes:
1. Add a CI/script step (or CMake custom target) that asserts:
   - `core_primitives.h` does not directly or transitively include any
     `clp/`, `types/tape.h`, `prof/profiler.h`, or `stats_math.h` header.
   - Use `clang -H` output or a grep over the include graph.

Tests:
1. Run the guard script in CI.

Done when:
1. Guard script passes and is committed.

---

## Minimal Merge Policy

1. One task per PR (Tasks 4–12 may be paired if they are adjacent domain moves
   with no shared-lambda conflict).
2. No task merges without its listed tests passing.
3. Tasks 4–12 must be merged in order; Task 13 only after all domain tasks are
   complete.
4. Behavior changes are **forbidden** in this plan; any divergence from baseline
   must be flagged and resolved before merge.

---

## Expected Outcomes

| Metric | Before | After (expected) |
|---|---|---|
| `core_primitives.h` size | 6 730 lines | ≤ 15 lines |
| TUs including all of `core_primitives.h` | ~20+ | 8 (the new `.cpp` files) |
| Test TUs including CLP headers | ~20+ | 1–2 (only those testing CLP) |
| `register_core_primitives` in source | 1 giant inline fn | 1 dispatcher + 9 domain fns |
| Lines removed from the header include graph per test TU | 0 | ~40–70 transitively heavy includes |

---

## Relationship to Other Plans

- **`deduplication_plan.md` Tasks 11–13** (AAD unary wrapper consolidation):
  Should be executed **after** Task 5 of this plan (`core_primitives_math.cpp`)
  is merged, so the helper extraction targets a focused file rather than the
  monolith.
- **`std_bitset_plan.md`**: New builtins add to a new `core_primitives_bitset.cpp`
  file; no changes to existing domain files.
- **`import_refactor_plan.md`**: No interaction; operates on different files.

