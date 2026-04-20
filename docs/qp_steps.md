# QP MVP Plan (Staged)

[<- Back to README](../README.md) - [CLP](clp.md) - [Next Steps](next-steps.md)

---

## Overview

This document proposes a staged MVP for convex quadratic programming (QP) in
Eta's CLP(R) stack.

Current gap:

- CLP(R) is linear-only.
- Portfolio optimization currently solves a linear proxy objective and reports
  quadratic risk post-hoc.

MVP goal:

- Solve objectives of the form `0.5*x^T*Q*x + c^T*x + k` under existing linear
  CLP(R) constraints, with witness output compatible with current workflows.

---

## Scope

### In scope (MVP)

- Convex QP only (`Q` positive semidefinite within tolerance).
- Continuous real variables only (no integer/mixed-integer).
- Linear equality/inequality constraints from existing CLP(R) posting.
- Existing witness shape: `((var . value) ...)`.
- New/extended optimize path only; existing linear posting semantics unchanged.

### Out of scope (MVP)

- SOCP and general cone constraints.
- Non-convex QP.
- Nonlinear constraints.
- Mixed-integer QP (MIQP).
- New VM opcodes.

---

## Design Principles

1. Preserve CLP(R) substrate: trail, `RealStore`, domain narrowing, and rollback
   remain unchanged for posting/propagation.
2. Keep linear APIs backward-compatible.
3. Introduce QP through optimization path first; avoid destabilizing unify/propagation.
4. Keep deterministic output ordering and error tags in existing `clp.r.*` style.

---

## Stages

### Stage 0 - Contract and Success Criteria

Deliverables:

- Freeze objective/constraint contract for QP optimize builtins.
- Define numerical tolerances and accepted statuses.
- Decide backend strategy (recommended: external convex QP backend behind CMake option, plus graceful fallback error when unavailable).

Exit criteria:

- Written spec approved.
- Error taxonomy agreed.

Proposed statuses:

- `optimal` -> `(opt . witness)`
- `infeasible` -> `#f`
- `unbounded` -> `clp.r.unbounded`
- numeric/backend failures -> user error (`clp.r.qp.*`)

---

### Stage 1 - Quadratic Objective Representation

Deliverables:

- Add quadratic expression model in runtime CLP layer:
  - quadratic terms `(i,j,coef)`
  - linear terms `(i,coef)`
  - constant
- Add canonicalization:
  - merge duplicate terms
  - symmetric normalization for `(i,j)` / `(j,i)`
  - zero pruning
- Add objective linearizer that accepts:
  - constants
  - variable terms
  - scalar multiplications
  - variable-variable products (objective only)
- Keep constraint linearizer strict (constraints remain linear-only).

Suggested files:

- `eta/core/src/eta/runtime/clp/quadratic.h`
- `eta/core/src/eta/runtime/clp/quadratic.cpp`
- `eta/core/src/eta/runtime/core_primitives.h` (integration)

Exit criteria:

- Unit tests pass for canonicalization and parse/linearize error paths.

---

### Stage 2 - Matrix Materialization and Convexity Checks

Deliverables:

- Convert canonical objective to matrix/vector form:
  - `Q`, `c`, `k`
- Reuse existing materialized linear constraint extraction from `RealStore`.
- Add convexity check for `Q` (PSD check with tolerance/jitter policy).
- Enforce variable domain compatibility (`RDomain` only), matching current CLP(R) behavior.

Exit criteria:

- Deterministic matrix assembly verified by tests.
- Non-convex objectives rejected with explicit error tag.

Proposed error tags:

- `clp.r.qp.non-convex`
- `clp.r.qp.objective-nonlinear-unsupported`
- `clp.r.qp.numeric-failure`

---

### Stage 3 - QP Solver Backend Integration

Deliverables:

- Add QP solve path in optimize builtins:
  - `%clp-r-qp-minimize`
  - `%clp-r-qp-maximize`
- Backend adapter layer:
  - build matrices
  - run solver
  - map status to Eta conventions
  - recover witness map by logic-var id
- CMake integration for optional backend dependency, with clear compile-time flag.

Exit criteria:

- Backend builds on CI platforms.
- Returned witness is replayable via `unify`.
- Status mapping matches Stage 0 contract.

---

### Stage 4 - Stdlib Surface and Migration Path

Deliverables:

- Add stdlib wrappers in `stdlib/std/clpr.eta`:
  - `clp:rq-minimize`
  - `clp:rq-maximize`
- Preserve existing `clp:r-minimize`/`clp:r-maximize` behavior for linear objectives.
- Add example migration in portfolio flow:
  - replace linear risk proxy objective with direct quadratic risk term.

Exit criteria:

- Existing CLP(R) linear examples unchanged.
- New QP example demonstrates objective parity with reported risk metric.

---

### Stage 5 - Test and Validation Hardening

Deliverables:

- New C++ unit tests:
  - `clp_qp_linearize_tests.cpp`
  - `clp_qp_solver_tests.cpp`
  - edge cases: infeasible, unbounded, non-convex rejection, ill-conditioned matrices
- New stdlib tests:
  - `stdlib/tests/clpr_qp_optimization.test.eta`
- Regression:
  - existing `clpr_*` tests remain green
  - portfolio optimization regression checks solver consistency and constraints.

Exit criteria:

- Full test suite green.
- Deterministic outputs across repeated runs within tolerance.

---

### Stage 6 - Performance and Rollout Gate

Deliverables:

- Baseline benchmark scripts for representative QP sizes.
- Baseline benchmark executable:
  - `eta_qp_bench`
- Benchmark runner scripts:
  - `scripts/qp-benchmark.ps1`
  - `scripts/qp-benchmark.sh`
- Compare against existing LP-proxy workflow for:
  - objective quality
  - runtime
  - numerical stability.
- Documentation updates:
  - `docs/clp.md` QP section
  - `docs/portfolio.md` optimization section
  - `docs/release-notes.md`
- Optional rollout gate mode (`--gate`) with threshold checks on:
  - objective parity
  - stability drift
  - LP-vs-QP quality delta.

Exit criteria:

- Performance and stability acceptable for target workloads.
- QP path enabled by default (or explicitly documented feature flag if staged rollout is preferred).

---

## Suggested API (MVP)

Low-level builtins:

- `%clp-r-qp-minimize objective`
- `%clp-r-qp-maximize objective`

High-level wrappers:

- `(clp:rq-minimize objective)`
- `(clp:rq-maximize objective)`

Return convention (same as existing CLP(R) optimization):

- infeasible -> `#f`
- unbounded -> symbol `clp.r.unbounded` (or wrapper-level error, consistent with current style)
- optimal -> `(values optimum witness)`

---

## Why QP Before SOCP

1. Directly closes the current portfolio optimizer gap.
2. Reuses existing CLP(R) linear constraint pipeline and witness contract.
3. Lower implementation risk than cone modeling and solver interface expansion.

SOCP should be a post-MVP stage once concrete cone use-cases are validated.

---

## Risks and Mitigations

- Numerical fragility:
  - Mitigation: strict status mapping, tolerance policy, deterministic tests, PSD checks.
- Backend dependency complexity:
  - Mitigation: adapter abstraction, optional feature flag, explicit backend-unavailable errors.
- Behavior drift in existing CLP(R):
  - Mitigation: keep linear path untouched and separately tested.

---

## Post-MVP Extensions

- SOCP support (`||Ax+b||_2 <= c^T x + d`).
- Warm-start from cached witnesses.
- Incremental QP solves across scenario sweeps.
- MIQP exploration for discrete allocation decisions.
