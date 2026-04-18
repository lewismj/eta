# Phase 6.3 Plan: Fourier-Motzkin Elimination

This document is a detailed implementation plan for Stage 6.3, aligned with:

- `docs/phase6_implementation.md` (Stage 6.3)
- `docs/logic-next-steps.md` (Phase 6 CLP(R) direction)
- Existing Stage 6.1 and 6.2 runtime layout already landed in this repo

Terminology note: this stage should be referred to as **Fourier-Motzkin Elimination (FME)**. "Fallback" describes its role relative to Simplex (Stage 6.5), not the algorithm itself.

## 1. Stage Goal and Boundaries

### Goal

Implement a small-system FME engine over `clp::LinearExpr` that provides:

1. feasibility checks (`feasible`)
2. per-variable bound tightening (`bounds_for`)
3. deterministic behavior suitable for use as a long-term oracle against Stage 6.5 Simplex

### In Scope (Stage 6.3)

- New `clp/fm.h` and `clp/fm.cpp` with a self-contained FME engine.
- Equality handling (`=`) via paired inequalities.
- Deterministic elimination ordering (Bland-style stable ordering).
- Redundancy control and row-cap explosion guard.
- Test-only runtime builtins:
  - `%clp-fm-feasible?`
  - `%clp-fm-bounds`
- Unit tests and smoke tests for feasibility, infeasibility, bound tightening, redundancy behavior, and cap behavior.

### Out of Scope (Stage 6.3)

- No public `std.clpr` API changes yet (that is Stage 6.4).
- No incremental solver state or trailing integration with `RealStore` yet.
- No optimization (`minimize`/`maximize`) yet.
- No strict inequalities in this stage (`<`, `>`), only `<=`, `>=`, `=`.

## 2. Contract for Stage 6.3

## 2.1 Canonical Row Form

All constraints are represented as:

`sum_i (a_i * x_i) + c <= 0`

using `clp::LinearExpr` (`terms + constant`) from Stage 6.2.

Equality rows are stored separately in API input, then expanded to two `<=` rows:

- `e <= 0`
- `-e <= 0`

## 2.2 C++ API Contract

Add `eta/core/src/eta/runtime/clp/fm.h`:

```cpp
namespace eta::runtime::clp {

enum class FMStatus : std::uint8_t {
    Feasible,
    Infeasible,
    CapExceeded,
};

struct FMConfig {
    std::size_t row_cap = 4096;
    double eps = 1e-12;
};

struct FMSystem {
    std::vector<LinearExpr> leq;
    std::vector<LinearExpr> eq;
};

struct FMFeasibilityResult {
    FMStatus status = FMStatus::Feasible;
};

struct FMBoundsResult {
    FMStatus status = FMStatus::Feasible;
    std::optional<RDomain> bounds;
};

FMFeasibilityResult fm_feasible(const FMSystem& sys, FMConfig cfg = {});
FMBoundsResult fm_bounds_for(const FMSystem& sys,
                             memory::heap::ObjectId var_id,
                             FMConfig cfg = {});

} // namespace eta::runtime::clp
```

Notes:

- `FMSystem` remains pure value data, no VM coupling.
- `FMConfig` exposes row-cap and epsilon for deterministic tests and cap-smoke triggers.
- `fm_bounds_for` returns:
  - `status=Feasible` + `bounds` present when projection succeeds
  - `status=Infeasible` + `bounds=nullopt` when system is inconsistent
  - `status=CapExceeded` + `bounds=nullopt` when guard trips

## 2.3 Runtime Test Builtin Contract

Add two test-only primitives in `core_primitives.h`/`builtin_names.h`:

1. `%clp-fm-feasible? constraints [row-cap]`
2. `%clp-fm-bounds var constraints [row-cap]`

Where `constraints` is a proper list of relation terms:

- `(<= lhs rhs)`
- `(>= lhs rhs)`
- `(= lhs rhs)`

`lhs` and `rhs` can be any linearizable arithmetic term accepted by Stage 6.2.

Return contract:

- `%clp-fm-feasible?` returns:
  - `#t` (feasible)
  - `#f` (infeasible)
  - `'clp.fm.cap-exceeded` (guard tripped)
- `%clp-fm-bounds` returns:
  - `(lo . hi)` for feasible projection
  - `#f` for infeasible
  - `'clp.fm.cap-exceeded` for cap-exceeded

`lo`/`hi` are flonums and may be infinities when one side is unbounded.

## 3. Algorithm Plan

## 3.1 Preprocessing

For each input row:

1. canonicalize terms (`LinearExpr::canonicalize`)
2. drop near-zero coefficients (`abs(coef) <= eps`)
3. detect constant contradiction early:
   - if `terms.empty()` and `constant > eps`, system is immediately infeasible

Expand equalities into paired inequalities before elimination begins.

## 3.2 Elimination Variable Order

Use deterministic ascending `ObjectId` order of variables present in active rows.

Rationale:

- deterministic outputs for tests
- stable oracle behavior across runs
- low-complexity Bland-style tie-breaker

## 3.3 Eliminate-One-Variable Step

To eliminate variable `x`:

1. Partition rows into:
   - `P`: coefficient of `x` > `eps`
   - `N`: coefficient of `x` < `-eps`
   - `Z`: coefficient approximately zero
2. Keep all `Z` rows unchanged.
3. For each `(p in P, n in N)`, produce one combined row without `x`:
   - If `p: a_p x + r_p <= 0`, `n: a_n x + r_n <= 0` (`a_p > 0`, `a_n < 0`)
   - Build `(-a_n) * p + a_p * n <= 0`
   - `x` cancels exactly in exact arithmetic
4. Canonicalize the combined row and drop near-zero coefficients.
5. Run contradiction check on constant-only rows.

If `P` or `N` is empty, no cross-product rows are generated; only `Z` survives.

## 3.4 Redundancy and Dominance Control

Implement practical (not complete) redundancy pruning:

1. Exact-structure dedup:
   - rows with identical `(var_id, coef)` sequences keep only the tightest constant
2. Single-variable bound dominance:
   - for rows of form `a*x + c <= 0` with same `x` and sign of `a`, keep only tightest bound
3. Drop tautologies:
   - constant-only rows with `constant <= eps`

This provides significant row-count control with modest implementation complexity.

## 3.5 Row-Cap Guard

After each elimination round and pruning pass:

- if `row_count > row_cap`, stop and return `CapExceeded`

No attempt is made to continue partially once the cap trips.

## 3.6 Feasibility Result

Eliminate all variables. Final feasible iff no contradiction row exists:

- contradiction row is `0 <= 0` violated, represented as `terms.empty()` and `constant > eps`

## 3.7 Bounds for One Variable

To compute bounds for target `v`:

1. eliminate all variables except `v`
2. parse each remaining row:
   - `a*v + c <= 0`, `a > eps` implies upper bound `v <= -c/a`
   - `a*v + c <= 0`, `a < -eps` implies lower bound `v >= -c/a`
   - zero-coef constant contradiction implies infeasible
3. aggregate:
   - `lo = max(all lower bounds)` (or `-inf`)
   - `hi = min(all upper bounds)` (or `+inf`)
4. if `lo > hi + eps`, treat as infeasible
5. return `RDomain{lo, hi, false, false}` (closed bounds for this stage)

## 4. Runtime Integration Plan for Test Builtins

## 4.1 Parsing Constraints in `core_primitives.h`

Within the existing CLP builtin block, add local helpers:

- parse proper list of relation terms
- decode relation operator symbol
- linearize relation into `LinearExpr` by constructing `lhs - rhs` and calling `clp::linearize`

Mapping:

- `(<= lhs rhs)` -> `linearize(lhs - rhs)` as one `leq`
- `(>= lhs rhs)` -> `linearize(rhs - lhs)` as one `leq`
- `(= lhs rhs)`  -> `linearize(lhs - rhs)` as one `eq`

`%clp-fm-bounds` requires first arg to dereference to an unbound logic var.

## 4.2 Error Behavior

Builtin parse/linearize failures should surface as `RuntimeErrorCode::UserError` with stable tag prefixes:

- `clp.fm.parse.*`
- `clp.fm.linearize.*`

Do not throw raw untagged messages for stage-specific failures.

## 5. Repo Changes

## 5.1 New Files

- `eta/core/src/eta/runtime/clp/fm.h`
- `eta/core/src/eta/runtime/clp/fm.cpp`
- `eta/test/src/clp_fm_tests.cpp`
- `stdlib/tests/clp_fm_smoke.eta`
- `docs/phase6_3_plan.md` (this file)

## 5.2 Modified Files

- `eta/core/src/eta/runtime/core_primitives.h`
  - add `%clp-fm-feasible?`, `%clp-fm-bounds`
- `eta/core/src/eta/runtime/builtin_names.h`
  - add names/arities in lockstep with `core_primitives.h`
- `eta/core/CMakeLists.txt`
  - add `src/eta/runtime/clp/fm.cpp`
- `eta/test/CMakeLists.txt`
  - add `src/clp_fm_tests.cpp`

## 5.3 Builtin Ordering Discipline

Insert the new `%clp-fm-*` builtins immediately after `%clp-linearize` and before `%clp-fd-*`.
Mirror exactly in `builtin_names.h`.
Run `builtin_sync_tests` to verify no registration drift.

## 6. Testing Plan

## 6.1 C++ Unit Tests (`eta/test/src/clp_fm_tests.cpp`)

Core solver tests against `fm_feasible` and `fm_bounds_for`:

1. triangle feasible system (`x>=0`, `y>=0`, `x+y<=3`)
2. direct infeasible system (`x>=0`, `x<=-1`)
3. equality expansion correctness (`x+y=2` plus bounds)
4. redundant constraints (`x<=10`, `x<=5`) preserve tighter one
5. bound tightening:
   - `x+y<=3`, `x>=1`, `y>=1` gives `x<=2`, `y<=2`
6. unbounded side:
   - only lower bound on `x` returns `(lo, +inf)`
7. cap exceeded:
   - with tiny `row_cap` and elimination cross-product trigger, status is `CapExceeded`
8. deterministic result:
   - permuting input row order yields same status and same bounds within tolerance

## 6.2 Eta Smoke Test (`stdlib/tests/clp_fm_smoke.eta`)

Exercise the `%clp-fm-*` primitives end-to-end:

1. feasible triangle returns `#t`
2. infeasible pair returns `#f`
3. redundant constraints still feasible and bounded correctly
4. bounds case returns expected `(lo . hi)` values
5. cap guard path returns `'clp.fm.cap-exceeded` (using optional small cap argument)

Smoke should follow existing `std.test` conventions and avoid dependence on Stage 6.4 APIs.

## 6.3 Regression Gate

Run all existing CLP suites plus new tests:

- `eta_core_test` (including `clp_linear_tests`, `clp_fm_tests`, `builtin_sync_tests`)
- existing stdlib CLP smokes remain green
- new `clp_fm_smoke.eta` passes

## 7. Numeric Policy

Use a fixed epsilon policy for Stage 6.3:

- zero-coefficient threshold: `eps = 1e-12`
- contradiction check: `constant > eps` means violated

No adaptive scaling or presolve in this stage. Keep behavior deterministic and simple.

## 8. Risks and Mitigations

1. Row explosion in elimination cross-products
   - Mitigation: hard row cap, pruning, deterministic fail-fast status
2. Floating-point noise causing unstable bound edges
   - Mitigation: single epsilon policy + canonicalization + deterministic ordering
3. Test primitive parser complexity
   - Mitigation: narrow accepted relation grammar (`<=`, `>=`, `=` only), tagged parse errors
4. Builtin slot/order drift
   - Mitigation: mirror edits in `builtin_names.h`, rely on sync tests

## 9. Stage 6.4 and 6.5 Handoff

This plan intentionally keeps FME as a reusable backend/oracle:

- Stage 6.4 can call the same `fm_feasible`/`fm_bounds_for` through `RealStore`.
- Stage 6.5 can keep FME compiled under an oracle flag for differential testing.
- No public Eta API is frozen here; only internal contracts and test primitives are added.

## 10. Delivery Checklist

1. Add `fm.h` and `fm.cpp` with API and elimination core.
2. Implement preprocessing, elimination, pruning, contradiction checks, cap guard.
3. Implement `fm_feasible` and `fm_bounds_for`.
4. Add `%clp-fm-feasible?` and `%clp-fm-bounds` in runtime builtins.
5. Mirror builtin entries in `builtin_names.h`.
6. Wire `fm.cpp` into `eta/core/CMakeLists.txt`.
7. Add `clp_fm_tests.cpp` and register in `eta/test/CMakeLists.txt`.
8. Add `stdlib/tests/clp_fm_smoke.eta`.
9. Run unit + smoke test matrix.
10. Update Stage 6.3 status marker in `docs/phase6_implementation.md` after green runs.

## 11. Done Criteria

Stage 6.3 is done when all are true:

- FME feasibility and bounds APIs are implemented and deterministic.
- Cap-exceeded is surfaced explicitly before uncontrolled growth.
- `%clp-fm-feasible?` and `%clp-fm-bounds` work on relation-term lists.
- New unit/smoke tests pass, with no regressions in existing CLP suites.
- Builtin registration remains synchronized and validated.

