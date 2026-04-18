# Phase 6.2 Plan: Linear Expression Normaliser

This document is a detailed implementation plan for Stage 6.2 that stays aligned with:

- `docs/phase6_implementation.md` (Stage 6.2)
- `docs/logic-next-steps.md` (Phase 6 direction, and Phase 7 FactTable direction)

It is intentionally scoped to Stage 6.2 only.

## 1. Stage Goal and Boundaries

### Goal

Implement a pure C++ linear-expression normaliser that converts arithmetic term trees into canonical:

`sum_i (coef_i * var_i) + constant`

using:

- logic variable identity (`ObjectId`) for variable terms
- `double` coefficients/constants for Phase 6 real arithmetic

This normaliser is a shared substrate for Stage 6.3 (FM oracle), Stage 6.4 posting, and Stage 6.5 simplex integration.

### In Scope (Stage 6.2)

- New `clp::LinearExpr` representation and canonicalisation.
- Recursive parser for `+`, `-`, `*` over `CompoundTerm` and quoted cons trees.
- Dereference-aware handling of logic variables.
- Detection/reporting for non-linear expressions and unsupported atoms.
- Test-only primitive `%clp-linearize`.
- Unit tests and smoke tests for correctness and regressions.

### Out of Scope (Stage 6.2)

- No solver state, no FM/simplex solving.
- No symbolic division/reciprocal.
- No objective functions.
- No CLP(FD)+CLP(R) mixed propagation.
- No FactTable integration in runtime logic for this stage.

## 2. Contract (Authoritative for 6.2)

## 2.1 C++ API Contract

Add `eta/core/src/eta/runtime/clp/linear.h`:

```cpp
namespace eta::runtime::clp {
    struct LinearTerm {
        memory::heap::ObjectId var_id;
        double coef;
        bool operator<(const LinearTerm& other) const { return var_id < other.var_id; }
    };

    struct LinearExpr {
        std::vector<LinearTerm> terms;
        double constant = 0.0;
        void canonicalize();
    };

    struct LinearizeErrorInfo {
        std::string tag;      // clp.linearize.*
        std::string message;  // human-readable detail
        std::vector<memory::heap::ObjectId> offending_vars;
    };

    std::expected<LinearExpr, LinearizeErrorInfo>
    linearize(nanbox::LispVal term, memory::heap::Heap& heap, memory::intern::InternTable& intern_table);
}
```

Notes:

- `InternTable&` is included so operator symbols (`+`, `-`, `*`) can be identified reliably in both cons and compound forms without VM coupling.
- Function remains VM-decoupled and unit-testable.

## 2.2 Term Input Contract

The normaliser must accept both:

- `CompoundTerm` form, for example `(term '+ x 1)`
- quoted cons form, for example `` `(+ ,x 1) `` or `'(+ 1 2)`

Both are required by `phase6_implementation.md` Stage 6.2.

## 2.3 `%clp-linearize` Return Contract (Test Primitive)

To match Stage 6.2 "pairs plus constant tail" and keep Eta-side tests simple:

- return shape: `(pairs . constant)`
- `pairs` is a proper list of `(coef . var-id)` pairs

Example:

`(((2.0 . 101) (-1.0 . 202)) . 3.0)`

where `101` and `202` are logic-var object ids encoded as fixnums.

This preserves Stage 6.2 semantics while avoiding brittle Eta-side list-tail probing on improper alists.

## 2.4 Error Contract

Errors surface via `RuntimeErrorCode::UserError` with stable tagged messages:

- `clp.linearize.non-linear: ...`
- `clp.linearize.unknown-atom: ...`
- `clp.linearize.depth-exceeded: ...`
- `clp.linearize.type-error: ...` (optional catch-all)

## 3. Data Model and Canonicalisation Rules

`LinearExpr::canonicalize()` must:

1. sort terms by `var_id`
2. merge duplicate `var_id` by coefficient addition
3. drop terms with exactly `coef == 0.0`

Rationale:

- deterministic order for test stability
- canonical structure required by FM/simplex stages

No additional algebraic rewrites beyond one-pass linear combination assembly.

## 4. Parsing and Normalisation Algorithm

## 4.1 Runtime-Aligned Arity/Semantics

Arithmetic semantics must mirror runtime arithmetic builtins:

- `+`:
  - `(+ ) => 0.0`
  - variadic sum
- `-`:
  - unary negate for one arg
  - variadic subtraction for multiple args
- `*`:
  - `(* ) => 1.0`
  - variadic fold
  - reject any multiplication that combines two non-constant linear expressions

## 4.2 Leaf Handling

- numeric fixnum/flonum: promote via `classify_numeric(...)` to `double`
- logic var (after deref): `1.0 * var_id`
- symbol and other non-numeric atoms: `unknown-atom`

## 4.3 Dereference

Every visited node is dereferenced through logic-var bindings before classification.
Equivalent behavior to VM deref chain, but implemented in a local helper to keep VM decoupling.

## 4.4 Cycle and Depth Protection

- hard depth guard: `max_depth = 512`
- recursion-stack set on heap object ids:
  - add node id on entry
  - remove on exit
  - re-entry of active node => `depth-exceeded` (cycle)
- shared DAG subterms are allowed (only active-stack membership is rejected)

## 4.5 Internal Helpers

`linear.cpp` should define internal helpers:

- `linearize_rec(...)`
- `expr_add(lhs, rhs)`, `expr_sub(lhs, rhs)`, `expr_scale(expr, k)`
- `decode_operator(term, ...)` for both cons and compound inputs
- `collect_call_args(term, ...)` to extract operator arguments generically

## 5. Repo Changes

## 5.1 New Files

- `eta/core/src/eta/runtime/clp/linear.h`
- `eta/core/src/eta/runtime/clp/linear.cpp`
- `eta/test/src/clp_linear_tests.cpp`
- `stdlib/tests/clp_linearize_smoke.eta`

## 5.2 Modified Files

- `eta/core/src/eta/runtime/core_primitives.h`
  - register `%clp-linearize`
- `eta/core/src/eta/runtime/builtin_names.h`
  - add `%clp-linearize` in lockstep with `core_primitives.h`
- `eta/core/CMakeLists.txt`
  - add `src/eta/runtime/clp/linear.cpp`
- `eta/test/CMakeLists.txt`
  - add `src/clp_linear_tests.cpp`

## 5.3 Builtin Slot Discipline

Additions to `builtin_names.h` and `core_primitives.h` must be in identical order and grouping.
After adding `%clp-linearize`, run `builtin_sync_tests` to enforce lockstep.

Note: adding a new builtin changes subsequent slot indices. This is acceptable for this stage as long as all components are rebuilt together and lockstep is preserved.

## 6. Testing Plan

## 6.1 C++ Unit Tests (`eta/test/src/clp_linear_tests.cpp`)

Direct tests against `clp::linearize(...)`:

1. constants only:
   - `(+ 1 2 (* 3 4)) -> constant 15`
2. simple var:
   - `x -> 1*x + 0`
3. distributivity by scaling:
   - `(* 2 (+ x 3)) -> 2*x + 6`
4. cancellation:
   - `(+ x (- x)) -> 0`
5. nested mixed terms:
   - `(+ (* 2 (+ x y)) (* -1 x)) -> x + 2*y`
6. non-linear rejection:
   - `(* x y) -> non-linear`
7. deref chain:
   - `x -> y -> 5` in `(+ x z)` gives `z + 5`
8. DAG allowed:
   - shared subterm reused twice gives doubled coefficients
9. cycle rejection:
   - self-referential cons/compound triggers `depth-exceeded`
10. depth guard:
   - very deep nesting trips guard deterministically

## 6.2 Eta Smoke Test (`stdlib/tests/clp_linearize_smoke.eta`)

Use existing smoke style (local `chk`) or `std.test`, but use valid APIs only:

- import with `(import std.test std.logic std.core)`
- use `unify` (not `unify!`)
- use `assert-equal` (not `assert-equal?`)

Required smoke cases:

1. associativity equivalence
2. distributivity-style scaling over sums
3. constant folding
4. non-linear rejection with tagged error
5. deep nesting guard

For `%clp-linearize` result `(pairs . constant)`:

- read pairs via `(car res)`
- read constant via `(cdr res)`
- use helper to find coefficient by var-id in `(coef . var-id)` entries

## 6.3 Performance Check (Non-CI Gate)

Sanity benchmark:

- normalise flat 1000-term sum under 1 ms on reference machine
- measure in C++ unit harness or local benchmark utility

## 7. Forward Compatibility with Phase 6 and Phase 7

## 7.1 Phase 6 Continuity

This 6.2 representation is intentionally the direct input shape for:

- Stage 6.3 FM constraints (`LinearExpr` vectors)
- Stage 6.4 posting pipeline (`linearize -> post`)
- Stage 6.5 simplex row construction

No adapter layer should be needed beyond inequality/equality wrappers.

## 7.2 FactTable Direction (Phase 7)

`logic-next-steps.md` Phase 7 uses `FactTable` for clause database/indexing.
Stage 6.2 should not depend on `FactTable`, but should stay compatible with that future:

- keep `LinearExpr` as simple row-like data (`var_id`, `coef`, `constant`)
- keep deterministic canonical order for stable serialization/debugging
- keep errors tagged and machine-parseable for later tooling/indexing

If Phase 7 later wants solver telemetry or cached linear rows in `FactTable`, this shape is already suitable for lossless projection.

## 8. Step-by-Step Delivery Checklist

1. Add `linear.h` and `linear.cpp` with API and canonicalisation.
2. Implement recursive parser and helper operations.
3. Add cycle/depth guards and tagged error paths.
4. Add `%clp-linearize` builtin and mirror in `builtin_names.h`.
5. Wire `linear.cpp` into `eta/core/CMakeLists.txt`.
6. Add `eta/test/src/clp_linear_tests.cpp` and register in `eta/test/CMakeLists.txt`.
7. Add `stdlib/tests/clp_linearize_smoke.eta`.
8. Run:
   - `eta_core_test` (especially new clp tests and builtin sync)
   - stdlib smoke runner including new `clp_linearize_smoke.eta`
9. Update Stage 6.2 marker in `docs/phase6_implementation.md` after green results.

## 9. Done Criteria for Stage 6.2

Stage 6.2 is done when all are true:

- C++ lineariser exists and is VM-decoupled.
- `%clp-linearize` works for both compound and cons input trees.
- all planned unit/smoke tests pass.
- non-linear/unknown/depth failures return stable `clp.linearize.*` tags.
- 1000-term normalization meets local performance sanity target.
- no builtin registration mismatches.

