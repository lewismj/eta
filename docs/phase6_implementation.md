# Phase 6 — CLP(R/Q) Implementation Breakdown

Companion document to [`logic-next-steps.md`](logic-next-steps.md) §Phase 6.
Phase 6 (linear arithmetic over reals and rationals) is large enough that
landing it as a single PR would be unreviewable.  This file breaks it into
**seven independently shippable stages** (6.1 – 6.7), each with its own
smoke test, success criteria, and dependency arrows.  Stages 6.1 – 6.6
deliver real-valued (`double`) CLP(R); Stage 6.7 layers exact rationals
(CLP(Q)) on top behind a CMake feature flag.

> **Status legend:** ⏳ not started · 🚧 in progress · ✅ done · 🪦 deferred

---

## Guiding Principles (inherited)

All five principles from `logic-next-steps.md` §"Guiding Principles" apply
verbatim — in particular:

- **One VM, one heap, one trail, one propagation queue.**  CLP(R) reuses
  the unified `TrailEntry::Kind::Domain` from Phase 4b, the `'clp.prop`
  attribute key, and the FIFO `PropagationQueue` that drains at the
  outer-`unify` boundary.  No parallel solver plumbing.
- **Logic values = Eta values.**  Linear expressions are ordinary
  `CompoundTerm` trees (`(+ (* 2 x) y)`); no new heap kind.
- **No forked toolchains.**  Disassembler / serializer / DAP / LSP /
  fuzzer learn the new primitives in-place.  Phase 6 introduces **zero
  new opcodes** — every constraint is posted via builtins routed through
  the existing `Call` dispatch.

---

## Stage Dependency Graph

```
6.1 RDomain ──┬─► 6.2 LinearExpr ──┬─► 6.3 Fourier–Motzkin ──┐
              │                    │                          ├─► 6.4 std.clpr API ──► 6.5 Simplex ──► 6.6 r-minimize/maximize
              │                    └──────────────────────────┘                                                     │
              └────────────────────────────────────────────────────────────────────────────────────────────────────► 6.7 QDomain
```

Stage 6.7 is optional — feature-flagged and may slip without blocking
6.1 – 6.6 shipping.  Stages 6.3 and 6.4 can land in either order once
6.2 is in (FM-as-oracle vs. FM-as-runtime-fallback both supported).

---

## Stage 6.1 — `RDomain` (closed/open real intervals) ✅

**Goal:** introduce a third arm of `clp::Domain` so the existing trail
and propagation substrate can carry real-valued narrowings — *before*
any constraint engine exists.  This stage is mostly mechanical
plumbing and a `std::variant` arm extension.

### Work items

| Item | Touches |
|---|---|
| Add `RDomain { double lo, hi; bool lo_open, hi_open; }` | `eta/core/src/eta/runtime/clp/domain.h` |
| Extend `Domain = std::variant<ZDomain, FDDomain, RDomain>`; helpers `empty/contains/size` (size = `hi-lo` or ∞) | `clp/domain.h` |
| Extend `domain_intersect` for: R∩R (open/closed bound merge), R∩Z (interval intersection promoted to R), R∩FD (filter FD by R bounds; promote to R when FD becomes empty as bounds) | `clp/domain.h` |
| Audit every `std::visit` / `std::get_if` site in `core_primitives.h` and `vm.cpp`; add the `RDomain` arm or a clear UNREACHABLE | `eta/core/src/eta/runtime/{vm/vm.cpp, core_primitives.h}` |
| Trail snapshot path: confirm `TrailEntry::Kind::Domain` already stores `std::optional<clp::Domain>` — no schema change needed; add coverage for the new arm | `vm.h`, `vm.cpp` |
| New primitives `%clp-domain-r!`, extend `%clp-get-domain` printer | `core_primitives.h`, `builtin_names.h` (lockstep slot order) |
| Eta wrapper `clp:real`, `clp:domain-r` in a stub `stdlib/std/clpr.eta` | `stdlib/std/clpr.eta` (new) |
| Smoke test: post, intersect, trail unwind, cross-kind R∩Z narrowing, var–var R-domain merge via `VM::unify` | `stdlib/tests/clpr_domain_smoke.eta` (new) |

### Success criteria

- All existing CLP smoke tests still green (`clp.test.eta`,
  `clp_phase4_smoke.eta`, `clp_phase4b_arith.eta`,
  `clp_alldiff_regin_smoke.eta`, `clp_labeling_options.eta`,
  `clp_nqueens.eta`, `clp_prop_queue_smoke.eta`,
  `clp_varvar_merge_smoke.eta`).
- `(clp:real x -1.0 1.0)` then `(clp:real x 0.0 0.5)` narrows to
  `[0.0, 0.5]`; `clp:domain-r` exposes the bounds.
- `(clp:real x 0.0 1.0) ; (%clp-domain-z! y 2 5) ; (unify x y)`
  narrows the survivor to a `RDomain` covering `[2.0, 1.0]` → empty
  → unify fails cleanly.
- `TrailMark` / `UnwindTrail` round-trip restores the pre-write
  `RDomain` exactly (bounds + open/closed flags).

### Deliberate scope trims

- **No constraint propagation.**  Just the domain type and trail
  hookup — propagators arrive in 6.4.
- **No `RDomain` ↔ `FDDomain` enumeration bridge.**  R∩FD only tightens
  R bounds; the FD set is not converted to a real envelope and vice
  versa beyond bound extraction.

---

## Stage 6.2 — Linear-expression normaliser ✅

**Goal:** pure C++ helper that walks a `CompoundTerm` tree and produces
a canonical sum-of-products `LinearExpr`.  Decoupled from the VM so it
is unit-testable without any solver state.

### Work items

| Item | Touches |
|---|---|
| `clp::LinearExpr { struct Term { LogicVarId var; double coef; }; std::vector<Term> terms; double constant; }` with canonicalisation (sort by var-id, merge duplicates, drop zero-coef terms) | `eta/core/src/eta/runtime/clp/linear.h` (new), `linear.cpp` (new) |
| Recursive parser handling `(+ a b …)`, `(- a b)`, `(- a)`, `(* k v)`, `(* v k)`, scalar constants (fixnum + flonum), bare logic-var leaves | `clp/linear.cpp` |
| Error path: `(* x y)` (two vars) → `LinearizeError::NonLinear`; unbound non-numeric atom → `UnknownAtom`; cycle through `CompoundTerm` → guarded by depth limit | `clp/linear.cpp` |
| Reuse existing compound-term API (`functor`, `arity`, `arg`) via the runtime helpers — no direct heap layout coupling | `clp/linear.cpp`, `runtime/types/compound.h` |
| Test-only primitive `%clp-linearize` returning a list of `(coef . var-id)` pairs plus a constant tail | `core_primitives.h`, `builtin_names.h` |
| Smoke test: associativity (`(+ x (+ y z))` ≡ `(+ (+ x y) z)`), distributivity (`(* 2 (+ x y))` → `2x + 2y`), constants folded, non-linear rejection, deep-nesting depth guard | `stdlib/tests/clp_linearize_smoke.eta` (new) |

### Success criteria

- `(%clp-linearize '(+ (* 2 x) (* -1 y) 3))` returns the canonical
  `((2.0 . x) (-1.0 . y))` with constant `3.0`.
- `(%clp-linearize '(* x y))` raises a `RuntimeError` tagged
  `clp.linearize.non-linear` with both var ids in the payload.
- 1000-term flat sum normalises in < 1 ms on the reference benchmark
  box (sanity check, not a CI gate).

### Deliberate scope trims

- **No simplification across `(- (- x))` etc. beyond a single pass.**
  Caller is expected to feed already-evaluated arithmetic; the
  normaliser only handles the linear-combination skeleton.
- **No symbolic division / reciprocal.**  `clp:r/scalar` is a 6.4-era
  Eta-level convenience that pre-multiplies before linearising.

---

## Stage 6.3 — Fourier–Motzkin fallback (oracle) ✅

**Goal:** small-systems-only feasibility + bound-tightening engine that
serves dual purposes: (a) runtime backend before Stage 6.5 ships, and
(b) permanent test oracle for the simplex implementation.

### Work items

| Item | Touches |
|---|---|
| `clp::FMSystem { std::vector<LinearExpr> leq; std::vector<LinearExpr> eq; }` + `eliminate(var)`, `feasible()`, `tighten_bounds(var)` returning `RDomain` | `eta/core/src/eta/runtime/clp/fm.h` (new), `fm.cpp` (new) |
| Bland-style ordering for elimination variable choice; redundancy detection (drop dominated constraints to keep the system from exploding); hard cap on eliminated-system row count (default 4096) | `clp/fm.cpp` |
| Test-only primitive `%clp-fm-feasible?` and `%clp-fm-bounds` (returns `(lo . hi)` or `#f`) — not exported through `std.clpr` | `core_primitives.h`, `builtin_names.h` |
| Smoke tests: triangle feasible, infeasible (`x≥0, x≤-1`), redundant (`x≤10, x≤5`), bound-tightening (`x+y≤3, x≥1, y≥1` → `x≤2, y≤2`), explosion guard | `stdlib/tests/clp_fm_smoke.eta` (new) |

### Success criteria

- 5-var, 10-constraint LPs solved feasibility-wise instantly with hand
  calculations matching.
- Explosion guard fires (returning `'clp.fm.cap-exceeded`) before
  blowing past the row cap, instead of OOM.
- All bound-tightening results match the smoke-test ground truth.

### Deliberate scope trims

- **No optimisation.**  FM is feasibility + bound-tightening only.
- **No incrementality.**  Each `feasible?` call rebuilds elimination
  state.  6.5 (Simplex) is the incremental engine.

---

## Stage 6.4 — `std.clpr` API + posting (FM-backed) ⏳

**Goal:** ship the public Eta API for CLP(R) using the FM oracle as the
backend.  This freezes the user-facing surface so 6.5 can swap engines
without touching example code.

### Work items

| Item | Touches |
|---|---|
| Promote `stdlib/std/clpr.eta` from stub to full module exporting `clp:r=`, `clp:r<=`, `clp:r<`, `clp:r>=`, `clp:r>`, `clp:r+`, `clp:r-`, `clp:r*scalar`, `clp:r-bounds`, `clp:r-feasible?` | `stdlib/std/clpr.eta` |
| Per-VM `RealStore` (initially `clp::FMSystem` instance, GC-rooted via VM root set) tracking posted constraints and their participating var-ids | `eta/core/src/eta/runtime/clp/real_store.h` (new), `vm.h`, `vm.cpp` |
| Posting flow: linearise via 6.2 → push into `RealStore` → re-run FM `feasible?` → if `#f`, fail unify (constraint is rejected at post time); otherwise tighten bounds and write back via existing `VM::trail_set_domain` | `core_primitives.h` (new `%clp-r-post-leq!`, `%clp-r-post-eq!`), `clp/real_store.cpp` |
| Re-firing thunk attached under the existing `'clp.prop` attribute key on every participating var, so subsequent bindings re-tighten and re-check feasibility — *zero* new attribute key, *zero* new queue | `stdlib/std/clpr.eta`, reuses `register-prop-attr!` from Phase 4b |
| Trail integration: `RealStore` keeps an append-only constraint log; on `UnwindTrail` to mark `M`, truncate the log back to length-at-mark.  Snapshot the length onto the unified trail via a thin new `TrailEntry::Kind::RealStore` (single `size_t` payload — cheaper than snapshotting the full system) | `vm.h`, `vm.cpp`, `real_store.cpp` |
| Smoke test: post `(clp:r<= (clp:r+ x y) 10)`; bind `y = 4`; expect `x` narrowed to `(-∞, 6]`; backtrack; expect `x` and `y` restored | `stdlib/tests/clpr_post_smoke.eta` (new) |
| Smoke test: infeasibility on post fails the surrounding `unify` cleanly with full trail rollback (mirrors Phase 4b var-var merge fail path) | `stdlib/tests/clpr_post_smoke.eta` |

### Success criteria

- End-to-end real constraints work via the FM oracle.
- API stable enough that 6.5 can swap the backend with **no edits to
  `stdlib/std/clpr.eta` or `examples/portfolio-lp.eta`**.
- Backtracking through a `RealStore`-touching unify restores the
  constraint set bit-for-bit (verified by re-querying `r-bounds`).

### Deliberate scope trims

- **No `clp:r-minimize` yet.**  Optimisation is 6.6.
- **No mixed CLP(FD)+CLP(R) cross-domain propagation.**  An FD var
  participating in an R constraint is currently rejected with
  `clp.r.fd-mixing-not-supported` — opens a clean track for later
  cross-solver work.

---

## Stage 6.5 — Incremental Simplex (Dutertre–de-Moura) ⏳

**Goal:** replace the FM backend with a real incremental tableau.
This is the largest C++ deliverable in Phase 6 and the primary
performance gate.

### Work items

| Item | Touches |
|---|---|
| `clp::Simplex` over `double`: tableau, basic / non-basic indices, `Bound { double value; bool strict; }` records, `assertUpper(var, bound)`, `assertLower(var, bound)`, `check()`, pivot via Bland's anti-cycling rule | `eta/core/src/eta/runtime/clp/simplex.h` (new), `simplex.cpp` (new) |
| Slack-variable introduction for `≤` / `<` constraints; equality posted as paired bounds on a fresh slack | `clp/simplex.cpp` |
| Trailable bound deltas: new `TrailEntry::Kind::SimplexBound { var-id, std::optional<Bound> prev_lo, std::optional<Bound> prev_hi }` written via `VM::trail_assert_simplex_bound`.  Pivots themselves are deterministic from the bound state and re-derived as needed — *no full tableau snapshot* | `vm.h`, `vm.cpp` |
| Swap `RealStore` backend FM → Simplex behind the same `RealStore` interface; FM stays compiled-in behind `#ifdef ETA_CLP_FM_ORACLE` (default-off in release, default-on in `ctest`) | `clp/real_store.cpp`, `cmake/` |
| Cross-check harness: in oracle mode, every `RealStore` mutation runs both engines and asserts identical feasibility + bound results; divergence aborts with a dump of the constraint log | `clp/real_store.cpp` (oracle-only path) |
| Smoke test: incremental add/retract over 50-var system, redundant-bound idempotence, infeasibility detection, FM-vs-Simplex agreement on the entire 6.4 smoke corpus | `stdlib/tests/clpr_simplex_smoke.eta` (new) |

### Success criteria

- 50-variable LP feasibility within 5 ms wall on the reference box
  (matches `logic-next-steps.md` Phase 6 success criterion).
- Trail unwind correctly restores bounds (verified by the oracle
  harness when enabled).
- FM ≡ Simplex agreement on the full 6.4 smoke suite — zero
  divergences over a 1000-iteration property test (random posting
  order, random backtracks).

### Deliberate scope trims

- **No exact arithmetic.**  `double` only; rational scalars arrive in
  6.7 if/when that stage lands.
- **No dual simplex.**  Primal-only; sufficient for the Phase 6
  feasibility + optimisation use cases.
- **No presolve / scaling.**  Posting code does naive insertion;
  pathological coefficient ranges are not in scope for the MVP.

---

## Stage 6.6 — `clp:r-minimize` / `clp:r-maximize` ⏳

**Goal:** add an objective row to the simplex tableau and surface
LP optimisation through Eta-level wrappers.

### Work items

| Item | Touches |
|---|---|
| Objective row in `clp::Simplex`; `optimize(LinearExpr objective, Direction { Min, Max })` → `OptResult { Status { Optimal, Unbounded, Infeasible }; double value; std::vector<std::pair<LogicVarId,double>> witness; }` | `clp/simplex.cpp` |
| Eta wrappers `clp:r-minimize` / `clp:r-maximize` returning `(values opt witness)` on `Optimal`, `#f` on `Infeasible`, raising `clp.r.unbounded` on `Unbounded` | `stdlib/std/clpr.eta` |
| Witness application is **outside** any committing trail-mark — caller decides whether to bind the witness vars | `stdlib/std/clpr.eta` |
| New example demonstrating a small portfolio LP (≤ 10 assets, expected-return / risk caps) | `examples/portfolio-lp.eta` (new) |
| Smoke test: bounded / unbounded / infeasible, 10-asset LP correctness vs hand-computed optimum, optimisation-then-backtrack leaves no residual bounds | `stdlib/tests/clpr_optimize_smoke.eta` (new) |

### Success criteria

- `examples/portfolio-lp.eta` terminates with the documented optimal
  allocation.
- `clp:r-minimize` / `clp:r-maximize` correctly classify the three
  status cases on the smoke corpus.
- Backtrack after optimisation restores `RealStore` and bounds intact
  (no objective-row residue).

### Deliberate scope trims

- **No multi-objective / lexicographic LP.**  Single linear objective
  only.
- **No sensitivity analysis / duals exposed to Eta.**  Internal
  dual values may be retained but are not surfaced through the API.

---

## Stage 6.7 — `QDomain` + rationals (optional, gated) 🪦 deferrable

**Goal:** layer exact rational arithmetic on top of the Stage 6.5
simplex so finance/causal code that needs reproducible non-rounding
results has it.

### Work items

| Item | Touches |
|---|---|
| CMake feature flag `ETA_WITH_BOOST_MP` (default-off; mirrors the existing `cmake/Fetch*.cmake` pattern) pulling `boost::multiprecision::cpp_rational` (header-only) | `cmake/FetchBoostMP.cmake` (new), top-level `CMakeLists.txt` |
| `QDomain { boost::multiprecision::cpp_rational lo, hi; bool lo_open, hi_open; }` parallel to `RDomain`; `Domain` becomes 4-arm variant when the flag is on | `clp/domain.h` (`#ifdef`-guarded) |
| Templatise `clp::Simplex<Scalar>` on the numeric scalar; instantiate `Simplex<double>` always, `Simplex<cpp_rational>` when the flag is on.  Decision rationale logged inline next to the template definition | `clp/simplex.h`, `simplex.cpp` |
| Eta API `clp:rational`, `clp:q=`, `clp:q<=`, `clp:q-minimize` in a sibling `stdlib/std/clpq.eta` (separate module so disabling the flag removes a single import, not surgical edits in `clpr.eta`) | `stdlib/std/clpq.eta` (new) |
| Smoke test: exact arithmetic regressions where `double` would round wrong (`1/3 + 1/3 + 1/3 = 1` exactly), and re-runs of the 6.5 smoke corpus under `Q` to verify identical optimisation outcomes for rational inputs | `stdlib/tests/clpq_smoke.eta` (new) |

### Success criteria

- Flag default-off in CI quick build, default-on in nightly extended
  build; both builds green on the full Phase 6 test corpus.
- 6.5's portfolio LP, when posted with rational-literal coefficients,
  produces a witness with rational components that exactly satisfy the
  constraints (no `≈` tolerance).

### Deliberate scope trims

- **No `QDomain ↔ RDomain` cross-promotion.**  A constraint mixing
  rational and double scalars rejects with
  `clp.q.scalar-mixing-not-supported`.
- **No alternative big-rational backend.**  Boost MP is the only
  vendor; if the dependency proves unwelcome, this stage stays
  permanently gated.

---

## Cross-Cutting Checklist

Mirrors `logic-next-steps.md` §"Cross-Cutting Concerns".  Every stage
must tick the applicable rows before being declared done.

| Concern | Per-stage requirement |
|---|---|
| **Bytecode** | Phase 6 introduces **zero new opcodes**.  CI grep on `OpCode::` deltas in any 6.x PR fails review. |
| **Disassembler / serializer** | No changes expected; if any are needed, the stage justifies them in its PR description. |
| **DAP** | New `TrailEntry::Kind::RealStore` (6.4) and `Kind::SimplexBound` (6.5) appear in `enumerate_gc_roots` categories so backtrack state is visible in the debugger. |
| **LSP** | `builtin_names.h` extended in lockstep with `core_primitives.h` for every new `%clp-r-*` / `%clp-q-*` primitive — preserves the file's slot-index invariant. |
| **Docs** | Per stage: amend a single new `docs/clp-r.md` (and `docs/clp-q.md` for 6.7).  Cross-link from `docs/clp.md`, `docs/portfolio.md`, `docs/causal.md`. |
| **Examples** | 6.6 ships `examples/portfolio-lp.eta`.  Earlier stages ship test-only examples in `stdlib/tests/`. |
| **Benchmarks** | `bench/clp/{r-feasibility, r-lp50, portfolio-lp}` added with 6.5 / 6.6; tracked in CI's perf table. |
| **Fuzzing** | Extend `eta/fuzz/src/clp_fuzz.cpp` with random `LinearExpr` generation; invariant oracles: FM ≡ Simplex (6.5+), backtrack idempotence, posting-order independence of feasibility. |
| **Performance counters** | Extend `(vm-stats)` with `simplex-pivots`, `simplex-bound-asserts`, `r-store-feasibility-checks`. |

---

## Further Considerations

1. **Numeric scalar choice for Simplex**: `double` (6.5) vs templated
   `double | cpp_rational` from day one.  *Recommend* double-only in
   6.5, template in 6.7 — keeps 6.5 reviewable and lets us defer the
   Boost MP decision.
2. **FM lifetime**: keep as runtime fallback for tiny systems, or
   delete after 6.5 lands?  *Recommend* keep as compile-time-flagged
   oracle (`ETA_CLP_FM_ORACLE`) — invaluable for fuzz invariants and
   regression triage.
3. **New trail entry kinds**: `Kind::RealStore` (6.4) and
   `Kind::SimplexBound` (6.5).  Could both reuse `Kind::Domain` with a
   discriminated payload, but dedicated kinds are clearer at GC-root
   walk time and follow the precedent set by `Kind::Attr` (Phase 3).
4. **Boost dependency footprint**: `boost::multiprecision` is
   header-only but heavy.  Gating behind `ETA_WITH_BOOST_MP` matches
   the existing optional-fetch pattern and keeps default builds lean.
5. **Cross-domain CLP(FD)+CLP(R)**: explicitly out of scope for Phase 6
   (rejected with a clear error in 6.4).  Tracked separately as a
   potential Phase 6c once 6.6 ships and a real workload demands it.
6. **Stage ordering flexibility**: 6.3 (FM) and 6.4 (Eta API) can
   swap.  6.7 (Q) is independent and may slip indefinitely without
   blocking the CLP(R) shipment.

---

*Draft plan — stages, APIs, and success criteria are open for review.
Update each stage's status legend as it lands; mirror progress in the
Phase 6 row of `logic-next-steps.md`.*

