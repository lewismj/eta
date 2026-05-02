# CLP(R) — Constraint Logic Programming over Reals

[<- Back to README](../../../README.md) · [CLP(FD)](clp.md) · [CLP(B)](clpb.md) ·
[Logic](logic.md) · [Runtime & GC](runtime.md)

> This page is the canonical `std.clpr` reference: it documents the
> algorithms, the trail/store split, the simplex and active-set QP
> backends, and the wire-format of every `%clp-r-*` builtin.

---

## 1. Overview

`std.clpr` extends Eta's logic substrate with continuous-domain
constraint solving. Where `std.clp` reasons over integer / finite
domains and `std.clpb` over `{0,1}`, `std.clpr` posts **linear
inequalities and equalities over the reals**, plus convex **quadratic
objectives** for optimization. It runs entirely on the existing VM:

- Real variables are ordinary logic variables that carry an `RDomain`
  (interval) **plus** a row in a global *real store* (the simplex
  tableau).
- Posting goes through the unified VM trail, so every
  `findall` / `run1` / `(unwind-trail m)` rolls back the simplex log
  *together with* the binding stack — no parallel transaction system.
- Optimization (`clp:r-minimize`, `clp:rq-minimize`) runs the
  underlying simplex / active-set QP solver and returns a witness
  alist that can be replayed with `unify`.

```scheme
(import std.logic)
(import std.clpr)              ; not part of the prelude — import explicitly
```

---

## 2. Architecture

```
                   ┌──────────────────────────────────────────┐
   Eta program ──> │  std.clpr   (clp:real, clp:r+, clp:r<=)  │
                   └─────────────┬────────────────────────────┘
                                 │ %clp-r-*  builtins
                   ┌─────────────▼────────────────────────────┐
                   │  RealStore — append-log of posted rows   │
                   │  + simplex bound cache  (per variable)   │
                   └─────────────┬────────────────────────────┘
                                 │
        ┌────────────────────────┼─────────────────────────┐
        ▼                        ▼                         ▼
  ┌────────────┐          ┌────────────┐           ┌──────────────┐
  │  Simplex   │          │  FM oracle │           │  Active-set  │
  │  (LP)      │          │  (debug)   │           │  QP solver   │
  └────────────┘          └────────────┘           └──────────────┘
```

Two backends share the row store:

| Backend | File | Purpose |
|---|---|---|
| Revised simplex (Bland's rule, two-phase) | `clp/simplex.cpp` | LP feasibility, bound projection, `clp:r-minimize`/`-maximize` |
| Fourier–Motzkin elimination | `clp/fm.cpp` | Reference oracle for unit tests; not in default path |
| Active-set QP (PSD/NSD checked) | `clp/qp_solver.cpp` | `clp:rq-minimize` / `clp:rq-maximize` |

### 2.1 The Real Store

Each posted constraint becomes one row in `RealStore`:

```
row := (kind, coeffs : map<var, double>, rhs : double, strict? : bool)
   kind ∈ { LEQ, EQ }
```

Posting is a strict append — the store is monotone within one logic
branch. Rollback is therefore a single `resize(prev_len)` on
`unwind-trail`. The trail records the *previous* size as a
`RealStore` entry; the LP solver reads only `[0 .. cur_len)`.

The bound cache is keyed by variable; each `SimplexBound` trail entry
stores the previous bound so that domain narrowing performed by the
simplex (during posting) can be undone independently of the row log.

### 2.2 Why two stores?

- The **interval store** (`RDomain`) is what `clp:r-bounds` and
  `clp:r-feasible?` query. It is populated by both `clp:real …` and
  by simplex-derived bound projection.
- The **row store** holds the global linear system. It is what the
  optimizer actually solves.

Both are written by every domain-installing call (`clp:real`,
`clp:real-open`, `clp:real-half-open`), so a single
`(clp:real x 0.0 1.0)` is enough to constrain `x` end-to-end. No
separate `(clp:r>= x 0)` / `(clp:r<= x 1)` pair is needed.

---

## 3. Posting Algorithm

For every `(clp:r<= a b)` / `(clp:r= a b)` call:

1. **Linearise.** `linear.cpp::linearise(expr) -> (coeff_map, const)`
   walks the expression tree built by `clp:r+`, `clp:r-`,
   `clp:r*scalar`, accumulating `Σ kᵢ·xᵢ + c`. Anything outside that
   grammar (e.g. `(* x y)`) raises `clp.r.objective-nonlinear-unsupported`.
2. **Trail mark.** `trail_mark()` snapshots the trail depth.
3. **Append row.** `RealStore::push({coeffs, rhs - c, strict})`,
   trailing the old size as a `RealStore` entry.
4. **Bound projection.** `Simplex::tighten_bounds(row)` runs a
   single phase-1 step. If it derives a tighter bound on any
   participating variable, the previous bound is trailed as
   `SimplexBound` and `RDomain` is updated.
5. **Feasibility check.** A second phase-1 pivot run; if the LP is
   infeasible, `unwind_trail(mark)` undoes everything (row, bounds,
   propagator queue) and returns `#f`. The Eta wrapper signals
   `clp.r.infeasible`.
6. **Propagation queue.** Any pending propagators on participating
   variables are queued in the FIFO and drained at the outer `unify`
   boundary, so logic-side hooks (`freeze`, `dif`) re-fire.

Strict inequalities (`clp:r<`, `clp:r>`) are encoded as `<=` rows
with the `strict` flag set. The simplex bound reader applies an
internal epsilon (`1e-9` by default) when reading strict bounds, so
the solver returns a witness *interior* to the half-space.

---

## 4. The LP Path

`(clp:r-minimize obj)` and `(clp:r-maximize obj)`:

```
obj is linearised  ->  c, k                              (coeffs, const)
let A,b,bounds  =  current RealStore  +  bound cache
result          =  Simplex::solve(c, A, b, bounds)
case result of
  Infeasible      -> %clp-r-minimize returns #f
  Unbounded       -> returns symbol  clp.r.unbounded
  Optimal v, x*   -> returns (v + k . witness-alist)
```

The wrapper turns `Infeasible` into `#f` and `Unbounded` into a
raised `clp.r.unbounded` error (use the low-level builtin to read
the symbol directly).

The simplex itself is a textbook *revised* implementation:

- **Phase 1.** Auxiliary problem with artificial slacks, minimise
  the sum of artificials. If the optimum is non-zero the system is
  infeasible.
- **Phase 2.** Pivot with **Bland's rule** (lowest-index entering /
  leaving) to guarantee termination on degenerate problems.
- **Bound representation.** Variables hold their bound directly (no
  shifted-variable substitution); the pivot routine consults
  `SimplexBound` entries.

Numerical tolerances live in `simplex.h::Tolerance` and are tuned for
financial-scale matrices (hundreds of constraints, conditioning up to
1e8). For larger problems use the bench in `eta/qa/bench/`.

### 4.1 Worked example — small LP

From [`examples/portfolio-lp.eta`](../../../examples/portfolio-lp.eta):

```scheme
;; maximise   r·w
;; subject to risk·w  <= 0.45
;;            sum(w)   = 1
;;            0 <= w_i <= 1                        ; i = 1..10
(post-lower-bounds! weights 0.0)
(post-upper-bounds! weights 1.0)
(clp:r=  (dot ones  weights) 1.0)
(clp:r<= (dot risks weights) 0.45)

(call-with-values
  (lambda () (clp:r-maximize (dot returns weights)))
  (lambda (optimum witness)
    (apply-witness! witness)
    ...))
```

After the four posting calls the row store contains
`12 + 1 + 1 = 14` rows (10 lower, 10 upper collapsed by the bound
cache to 10 `SimplexBound` entries, 1 sum equality, 1 risk
inequality). The simplex pivots from the all-zero feasible point
into a basic feasible solution; the optimum lands at
`w₁ = w₂ = 0.5`, objective `0.135`. Backtracking past the
`r-maximize` call (e.g. inside a `findall`) restores the row store
to its pre-optimization length — the solver itself never mutates the
store.

---

## 5. The QP Path

`std.clpr` accepts convex quadratic objectives of the form

$$
f(x) = \tfrac12\, x^\top Q x + c^\top x + k
$$

with `Q` PSD for `clp:rq-minimize` and NSD for `clp:rq-maximize`.
Constraints remain linear.

### 5.1 Linearising the objective

`quadratic.cpp::linearise_quadratic` walks the expression tree, but
unlike the LP linearizer it accepts a single extra grammar production:

```
expr := constant
      | var
      | (+ expr ...)        | (- expr ...)
      | (clp:r*scalar k expr)
      | (* var var)         ; quadratic only — objective only
```

Each `(* xᵢ xⱼ)` term contributes `Q[i,j] += 1` (and `Q[j,i] += 1` by
symmetry). The resulting `Q` is checked for PSD via Cholesky; failure
raises `clp.r.qp.non-convex`.

### 5.2 Active-set algorithm

`qp_solver.cpp` runs the canonical primal active-set method:

1. Find a feasible starting point via the underlying simplex (warm
   start using the LP basis if one exists from prior posting).
2. At each iteration, solve the equality-constrained QP over the
   *currently active set* of inequalities. This is a linear KKT
   system; solved by Cholesky of `[Q  Aᵀ; A  0]`.
3. If the step is blocked by an inactive constraint, add it to the
   active set and repeat. If the dual multiplier of an active
   inequality becomes negative, drop it.
4. Terminate when the projected gradient is below `qp_tol`
   (default `1e-8`) and all multipliers are non-negative.

PSD pre-checking keeps the inner Cholesky always well-defined, so the
solver does not need a full convex/non-convex switch.

### 5.3 Worked example — minimum-variance portfolio

From [`stdlib/tests/clpr_qp_optimization.test.eta`](../../../stdlib/tests/clpr_qp_optimization.test.eta):

```scheme
(let ((x (logic-var)))
  (clp:r>= x 0)  (clp:r<= x 3)
  ;; minimise (x - 2)^2  =  x^2 - 4x + 4
  (call-with-values
    (lambda ()
      (clp:rq-minimize
        (list '+ (list '* x x) (list '* -4 x) 4)))
    (lambda (opt witness)
      ; opt     ≈ 0.0
      ; witness => ((x . 2.0))
      )))
```

The unique global minimum `x* = 2` lies strictly inside the
`[0, 3]` box, so no inequality is active at the optimum and the
solver terminates after one Newton step on the unconstrained `Q`.

---

## 6. Trail Interaction

Because every store mutation is trailed, CLP(R) composes cleanly
with the rest of the logic engine:

```scheme
(let ((x (logic-var)))
  (clp:real x 0 10)
  (clp:r<= x 5)
  (findall (lambda () (deref-lvar x))
           (list (lambda () (unify x 3))      ; ok, in [0,5]
                 (lambda () (unify x 7))      ; fails — outside store
                 (lambda () (unify x 5)))))   ; ok, on boundary
;; => (3 5)
```

Each branch is wrapped by `findall` in its own
`trail-mark` / `unwind-trail` pair. The failed `(unify x 7)` triggers
a domain-membership check inside `VM::unify` (the `RDomain` says
`x ≤ 5`), which fails the unification, which rolls back the branch.
The simplex tableau is untouched because no posting happened — the
cost is paid only at posting time.

---

## 7. Public API Summary

### Domain

| Form | Meaning |
|---|---|
| `(clp:real v lo hi)` | `v ∈ [lo, hi]` |
| `(clp:real-open v lo hi)` | `v ∈ (lo, hi)` |
| `(clp:real-half-open v lo hi lo-open?)` | mixed bounds |
| `(clp:domain-r v)` | retrieve domain object |
| `clp:domain-r?`, `clp:domain-r-lo`, `clp:domain-r-hi`, `clp:domain-r-lo-open?`, `clp:domain-r-hi-open?` | accessors |

### Expressions and posting

| Form | Meaning |
|---|---|
| `(clp:r+ e ...)`, `(clp:r- e ...)`, `(clp:r*scalar k e)` | linear expression |
| `(clp:r= a b)` | `a = b` |
| `(clp:r<= a b)` / `(clp:r< a b)` | `a ≤ b` / `a < b` (strict) |
| `(clp:r>= a b)` / `(clp:r> a b)` | symmetric |

### Optimization

| Form | Result (two values) |
|---|---|
| `(clp:r-minimize obj)` / `(clp:r-maximize obj)` | LP `optimum` + `witness` alist |
| `(clp:rq-minimize obj)` / `(clp:rq-maximize obj)` | convex QP `optimum` + `witness` |

### Queries

| Form | Returns |
|---|---|
| `(clp:r-feasible?)` | `#t` / `#f` for current store |
| `(clp:r-bounds v)` | `(lo . hi)` or `#f` |

### Low-level (the `%clp-r-*` builtins)

| Builtin | Returns |
|---|---|
| `%clp-r-post-leq!` / `%clp-r-post-eq!` | `#t` on success, `#f` on infeasibility |
| `%clp-r-minimize` / `%clp-r-maximize` | `#f`, `clp.r.unbounded`, or `(opt . witness)` |
| `%clp-r-qp-minimize` / `%clp-r-qp-maximize` | as above |
| `%clp-r-feasible?` / `%clp-r-bounds` | as the wrappers |

The high-level wrappers raise `clp.r.unbounded` as an error; the
low-level builtins return the symbol directly. Use the latter when
you want to handle unboundedness without `with-handlers`.

---

## 8. Error Tags

| Tag | When raised |
|---|---|
| `clp.r.infeasible` | Posting produces an infeasible LP |
| `clp.r.unbounded` | Optimization yields an unbounded objective |
| `clp.r.objective-nonlinear-unsupported` | LP path saw a `(* var var)` term |
| `clp.r.qp.non-convex` | `Q` not PSD (minimize) / NSD (maximize) |
| `clp.r.qp.numeric-failure` | Active-set inner Cholesky failed |
| `clp.r.qp.backend-unavailable` | Built without QP backend (not the default) |
| `clp.r.domain-conflict` | `clp:real` on a var that already carries a non-`R` CLP domain |

---

## 9. Limitations (v1)

- **Linear posting only.** `(* x y)` rows are rejected at post time;
  use the QP path for quadratic objectives, or pre-compute the
  product if one factor is ground.
- **No SOCP / SDP / MIP.** Integer/finite-domain modelling lives in
  `std.clp` and is not coupled to the simplex.
- **No incremental dual.** Every `clp:r-minimize` call solves from
  scratch; warm-start uses only the LP basis from posting, not from
  the previous optimization. For tight optimization loops, batch
  posting before calling minimize.

---

## 10. Source Locations

| Component | File |
|-----------|------|
| Real domain type | [`clp/domain.h`](../../../eta/core/src/eta/runtime/clp/domain.h) |
| Append-log + bound cache | [`clp/real_store.h`](../../../eta/core/src/eta/runtime/clp/real_store.h), [`clp/real_store.cpp`](../../../eta/core/src/eta/runtime/clp/real_store.cpp) |
| Linear / quadratic linearizer | [`clp/linear.cpp`](../../../eta/core/src/eta/runtime/clp/linear.cpp), [`clp/quadratic.cpp`](../../../eta/core/src/eta/runtime/clp/quadratic.cpp) |
| Simplex backend | [`clp/simplex.h`](../../../eta/core/src/eta/runtime/clp/simplex.h), [`clp/simplex.cpp`](../../../eta/core/src/eta/runtime/clp/simplex.cpp) |
| Active-set QP backend | [`clp/qp_solver.h`](../../../eta/core/src/eta/runtime/clp/qp_solver.h), [`clp/qp_solver.cpp`](../../../eta/core/src/eta/runtime/clp/qp_solver.cpp) |
| FM reference oracle | [`clp/fm.h`](../../../eta/core/src/eta/runtime/clp/fm.h), [`clp/fm.cpp`](../../../eta/core/src/eta/runtime/clp/fm.cpp) |
| `%clp-r-*` builtins | [`core_primitives.h`](../../../eta/core/src/eta/runtime/core_primitives.h) |
| `std.clpr` wrapper | [`stdlib/std/clpr.eta`](../../../stdlib/std/clpr.eta) |
| Tests — simplex | [`stdlib/tests/clpr_simplex.test.eta`](../../../stdlib/tests/clpr_simplex.test.eta) |
| Tests — LP optimization | [`stdlib/tests/clpr_optimization.test.eta`](../../../stdlib/tests/clpr_optimization.test.eta) |
| Tests — QP optimization | [`stdlib/tests/clpr_qp_optimization.test.eta`](../../../stdlib/tests/clpr_qp_optimization.test.eta) |
| Worked example — LP | [`examples/portfolio-lp.eta`](../../../examples/portfolio-lp.eta) |
| Featured example — QP + causal | [`examples/portfolio.eta`](../../../examples/portfolio.eta), [`docs/featured/portfolio.md`](../../featured/portfolio.md) |

