# Constraint Logic Programming

[<- Back to README](../README.md) - [Logic Programming](logic.md) -
[Runtime & GC](runtime.md) - [Architecture](architecture.md)

---

## Overview

Eta's CLP stack is now split across two main stdlib modules:

| Module | Domain | Purpose |
|--------|--------|---------|
| `std.clp` | `clp(Z)` and `clp(FD)` | Integer/fd domains, propagators, labeling, branch-and-bound |
| `std.clpr` | `clp(R)` | Real intervals, linear posting, simplex-backed feasibility/bounds/optimization |

Both modules run on top of the same VM unification/trail substrate. No new VM
opcodes are required for CLP itself; the functionality is provided by native
builtins plus Eta-level wrappers.

---

## Runtime Substrate

### Unified Trail

The VM trail is now a single `TrailEntry` stream with these kinds:

- `Bind` - logic variable binding
- `Attr` - attributed-variable slot update
- `Domain` - CLP domain write/erase
- `RealStore` - CLP(R) append-log size snapshot
- `SimplexBound` - cached simplex bound snapshot for one variable

`(trail-mark)` stores the current trail depth as a fixnum. `(unwind-trail m)`
rolls back every entry above that mark, restoring bindings, attributes, domain
state, CLP(R) posted rows, and cached simplex bounds atomically.

### Domain-Aware Unification

`VM::unify` now enforces CLP domains directly:

- Binding a domained var to a ground value checks membership before commit.
- `RDomain` accepts in-range fixnums/flonums.
- `ZDomain`/`FDDomain` reject non-integer flonums.
- Unifying two unbound domained vars intersects domains first.

Cross-kind intersection is handled by `domain_intersect` (`Z`, `FD`, `R`).

### Propagation Queue

CLP re-firing is queued, not recursively executed inside a bind. The VM keeps
an idempotent FIFO of propagator thunks keyed by closure object-id.

- Register attribute keys with `(register-prop-attr! 'key)`.
- If a bound var carries that attribute, each thunk in the attribute value list
  is queued.
- Queue drains at the outer `unify` boundary.
- Any thunk returning `#f` (or raising) fails unify and triggers rollback.

Diagnostics: `(%clp-prop-queue-size)`.

---

## `std.clp` (Integer / Finite Domain)

```scheme
(import std.clp)
```

### Domain Constructors

| Form | Meaning |
|------|---------|
| `(clp:domain x lo hi)` | `x` in integer interval `[lo, hi]` |
| `(clp:in-fd x v1 v2 ...)` | `x` in explicit finite set |

Domain introspection:

- `clp:domain-z?`, `clp:domain-fd?`
- `clp:domain-lo`, `clp:domain-hi`, `clp:domain-values`, `clp:domain-empty?`

Low-level shape from `%clp-get-domain`:

- `(z lo hi)`
- `(fd v1 v2 ...)`
- `(r lo hi lo-open? hi-open?)` for real domains

### Propagators

`std.clp` wraps native `%clp-fd-*` builtins and attaches re-propagators through
`'clp.prop`:

- Equality/arithmetic: `clp:=`, `clp:+`, `clp:plus-offset`, `clp:abs`, `clp:*`,
  `clp:sum`, `clp:scalar-product`, `clp:element`
- Ordering/distinctness: `clp:<=`, `clp:>=`, `clp:<>` (deprecated alias: `clp:!=`)
- Global: `clp:all-different`

`clp:all-different` is backed by native Regin-style value-graph matching
(domain-consistent pruning), not just pairwise filtering.

### Labeling API

`clp:solve` is a compatibility alias for first-solution labeling.

`clp:labeling` supports both legacy and plist forms:

- `(clp:labeling vars 'ff)`
- `(clp:labeling vars 'strategy 'ff 'value-ordering 'down 'solutions 'all ...)`

Supported options:

- `strategy`: `'leftmost`, `'ff`/`'smallest`, `'largest`
- `value-ordering`: `'up` (default), `'down`
- `solutions`: `'first` (default), `'all`, or integer `N`
- `on-solution`: callback receiving each solution snapshot

Return shape:

- `solutions='first` -> `#t`/`#f` with bindings kept on success
- `solutions='all` or integer `N` -> list of snapshots, vars left unbound

### Integer Optimization

- `(clp:minimize cost goal-thunk)`
- `(clp:maximize cost goal-thunk)`

These implement branch-and-bound over an integer `cost` variable by repeatedly
calling `goal-thunk` and tightening the cost domain.

---

## `std.clpr` (Real Linear Constraints)

```scheme
(import std.clpr)
```

`std.clpr` is not imported by `std.prelude`; import it explicitly.

### Real Domains

| Form | Meaning |
|------|---------|
| `(clp:real x lo hi)` | Closed interval `[lo, hi]` |
| `(clp:real-open x lo hi)` | Open interval `(lo, hi)` |
| `(clp:real-half-open x lo hi)` | Half-open `[lo, hi)` |

Accessors:

- `clp:domain-r`, `clp:domain-r?`
- `clp:domain-r-lo`, `clp:domain-r-hi`
- `clp:domain-r-lo-open?`, `clp:domain-r-hi-open?`

### Linear Expressions and Posting

Expression builders:

- `clp:r+`, `clp:r-`, `clp:r*scalar`

Relations:

- `clp:r<=`, `clp:r=`, `clp:r<`, `clp:r>=`, `clp:r>`

Internally posting calls:

- `%clp-r-post-leq!`
- `%clp-r-post-eq!`

Each post is atomic:

1. Trail mark
2. Append row to `RealStore`
3. Run simplex feasibility + bound projection
4. On failure, rollback everything to the mark

Strict inequalities in `std.clpr` are encoded with a small epsilon shift.

### Feasibility, Bounds, Optimization

- `(clp:r-feasible?)` -> rerun propagation, return `#t`/`#f`
- `(clp:r-bounds x)` -> `(lo . hi)` or `#f`
- `(clp:r-minimize objective)` / `(clp:r-maximize objective)` -> two values:
  `optimum` and `witness`
- `(clp:rq-minimize objective)` / `(clp:rq-maximize objective)` -> two values:
  `optimum` and `witness` for convex QP (or linear objectives)

`witness` is an alist `((var . value) ...)` suitable for replay via `unify`.

Low-level optimization builtins return:

- `#f` for infeasible
- symbol `clp.r.unbounded` for unbounded
- `(opt . witness)` for optimal

Low-level names:

- `%clp-r-minimize`, `%clp-r-maximize` (linear objective path)
- `%clp-r-qp-minimize`, `%clp-r-qp-maximize` (convex QP path)

The high-level `std.clpr` wrappers turn `clp.r.unbounded` into an error.

### Convex QP Optimization

Quadratic optimization in `std.clpr` uses objective shape:

- `0.5*x^T*Q*x + c^T*x + k`

Constraints remain linear (`<=`, `=`, bounds), and only convex QP is accepted:

- `clp:rq-minimize` expects `Q` PSD
- `clp:rq-maximize` expects `Q` NSD (equivalent PSD check on `-Q`)

Supported arithmetic forms for objective linearization:

- constants
- variables
- `+`, `-`
- scalar multiplication
- variable-variable products (`(* x y)`) in the objective only

Representative user-error tags:

- `clp.r.qp.non-convex`
- `clp.r.qp.objective-nonlinear-unsupported`
- `clp.r.qp.numeric-failure`
- `clp.r.qp.backend-unavailable`

### QP Backend Flag

`ETA_CLP_QP_BACKEND` controls the runtime QP backend and is enabled by default:

- CMake option: `option(ETA_CLP_QP_BACKEND ... ON)`
- when disabled, quadratic objectives return `clp.r.qp.backend-unavailable`
- linear optimization (`clp:r-minimize`/`clp:r-maximize`) is unaffected

### Backend Notes

- Linearization accepts arithmetic terms built from `+`, `-`, `*`.
- Non-linear terms (`var * var`) are rejected in linear optimization paths.
- CLP(R) posting uses a simplex backend (`clp/simplex.*`).
- Convex QP optimization uses the active-set backend (`clp/qp_solver.*`).
- Optional FM cross-checks can be enabled with `ETA_CLP_FM_ORACLE`.

---

## CLP + Fact Tables

CLP(R) is commonly fed from fact-table data at Eta level. The runtime does not
require a special CLP/fact-table bridge primitive: table reads produce normal
numbers, which you can combine into CLP expressions.

See:

- [`examples/portfolio.eta`](../examples/portfolio.eta)
- [`examples/portfolio-lp.eta`](../examples/portfolio-lp.eta)

---

## Current Limitations

- CLP(R) posting/propagation remains linear; only the optimize path accepts
  convex quadratic objectives.
- CLP(R) posting rejects vars that already carry non-`RDomain` CLP domains.
- `std.clpr` optimization wrappers signal unbounded objectives as errors (use
  low-level `%clp-r-maximize`/`%clp-r-minimize` if you need the raw symbol).

---

## Source Locations

| Component | File |
|-----------|------|
| Domain types (`Z`, `FD`, `R`) + intersection | [`clp/domain.h`](../eta/core/src/eta/runtime/clp/domain.h) |
| Domain store | [`clp/constraint_store.h`](../eta/core/src/eta/runtime/clp/constraint_store.h) |
| Linearizer | [`clp/linear.h`](../eta/core/src/eta/runtime/clp/linear.h), [`clp/linear.cpp`](../eta/core/src/eta/runtime/clp/linear.cpp) |
| Quadratic objective linearizer + matrix checks | [`clp/quadratic.h`](../eta/core/src/eta/runtime/clp/quadratic.h), [`clp/quadratic.cpp`](../eta/core/src/eta/runtime/clp/quadratic.cpp) |
| CLP(R) append log + simplex-bound cache | [`clp/real_store.h`](../eta/core/src/eta/runtime/clp/real_store.h), [`clp/real_store.cpp`](../eta/core/src/eta/runtime/clp/real_store.cpp) |
| Simplex backend | [`clp/simplex.h`](../eta/core/src/eta/runtime/clp/simplex.h), [`clp/simplex.cpp`](../eta/core/src/eta/runtime/clp/simplex.cpp) |
| Active-set QP backend | [`clp/qp_solver.h`](../eta/core/src/eta/runtime/clp/qp_solver.h), [`clp/qp_solver.cpp`](../eta/core/src/eta/runtime/clp/qp_solver.cpp) |
| FM engine | [`clp/fm.h`](../eta/core/src/eta/runtime/clp/fm.h), [`clp/fm.cpp`](../eta/core/src/eta/runtime/clp/fm.cpp) |
| Unification + rollback + propagation queue | [`vm/vm.h`](../eta/core/src/eta/runtime/vm/vm.h), [`vm/vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp) |
| CLP builtins (`%clp-domain-*`, `%clp-fd-*`, `%clp-r-*`, `%clp-r-qp-*`) | [`core_primitives.h`](../eta/core/src/eta/runtime/core_primitives.h) |
| `std.clp` wrappers | [`stdlib/std/clp.eta`](../stdlib/std/clp.eta) |
| `std.clpr` wrappers | [`stdlib/std/clpr.eta`](../stdlib/std/clpr.eta) |
| CLP tests | [`stdlib/tests/clp.test.eta`](../stdlib/tests/clp.test.eta), [`stdlib/tests/clpr_simplex.test.eta`](../stdlib/tests/clpr_simplex.test.eta), [`stdlib/tests/clpr_optimization.test.eta`](../stdlib/tests/clpr_optimization.test.eta), [`stdlib/tests/clpr_qp_optimization.test.eta`](../stdlib/tests/clpr_qp_optimization.test.eta) |
