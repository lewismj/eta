# Causal Portfolio - Staged Next Steps

[Back to README](../README.md) | [Portfolio](portfolio.md) |
[Causal](causal.md) | [CLP](clp.md) | [Torch](torch.md) | [Stats](stats.md)

---

## Objective

Move the current portfolio pipeline from:

- "causally correct if the SCM is correct"

to:

- "decision-useful under DAG error, estimation uncertainty, and regime shift"

without losing Eta's current strengths (composability, explainability, exact constraints, and performance).

---

## Guiding Principles

1. Keep each layer independently testable.
2. Prefer bounded-risk decisions over fragile point-optimal decisions.
3. Add robustness incrementally; preserve existing examples and APIs where possible.
4. Gate each stage with measurable acceptance criteria.

---

## Stage Overview

| Stage | Theme | Primary Outcome | Status |
|------|-------|------------------|--------|
| 0 | Baseline freeze + metrics | Reproducible benchmark and diagnostics | Implemented (2026-04-20) |
| 1 | Causal identification hardening | Stronger DAG checks and adjustment-set logic | Implemented (2026-04-20) |
| 2 | Misspecification robustness | DAG sensitivity + partial-identification bounds on returns | Planned |
| 3 | Uncertainty-aware optimization | Robust portfolio choice under parameter uncertainty | Planned |
| 4 | Structural/learned covariance | Better Sigma(m) aligned with causal structure | Planned |
| 5 | Empirical stress-test suite | Evidence of graceful degradation vs baselines | Planned |
| 6 | Dynamic control loop (advanced) | Decision-dependent dynamics and sequential policy | Planned |

---

## Stage 0 - Baseline Freeze and Instrumentation

Current status: implemented in `examples/portfolio.eta` run artifact output and baseline seed reporting.

### Scope

- Freeze a reference output for `examples/portfolio.eta`.
- Add reproducibility knobs (seed capture, deterministic run mode where possible).
- Persist key diagnostics per run.

### Touchpoints

- [examples/portfolio.eta](C:/Users/lewis/develop/eta/examples/portfolio.eta)
- [docs/portfolio.md](C:/Users/lewis/develop/eta/docs/portfolio.md)

### Deliverables

- Standard run artifact format:
  - selected DAG
  - tau vector
  - Sigma(m) summary
  - chosen weights
  - scenario returns and risk
- "baseline snapshot" section in docs for before/after comparisons.

### Exit Criteria

- Two consecutive runs with same seed produce equivalent decisions.
- Metric output is machine-parseable and documented.

---

## Stage 1 - Causal Identification Hardening

Current status: implemented in `std.causal` with d-separation checks, adjustment-set enumeration, and identification metadata APIs.

### Scope

- Strengthen graph-theoretic causal checks in `std.causal`.
- Improve adjustment-set discovery quality and transparency.

### Touchpoints

- [stdlib/std/causal.eta](C:/Users/lewis/develop/eta/stdlib/std/causal.eta)
- [docs/causal.md](C:/Users/lewis/develop/eta/docs/causal.md)
- [examples/do-calculus/demo.eta](C:/Users/lewis/develop/eta/examples/do-calculus/demo.eta)

### Deliverables

- Add explicit d-separation checker API.
- Add exhaustive or configurable adjustment-set search API.
- Return richer identification metadata:
  - chosen set
  - alternative valid sets
  - assumptions checklist

### Exit Criteria

- Existing causal examples remain green.
- New tests cover:
  - multiple valid adjustment sets
  - non-identifiable queries
  - false positives reduced for back-door checks

---

## Stage 2 - DAG Sensitivity and Partial Identification

### Scope

- Evaluate causal outputs across plausible DAG variants.
- Replace single-point tau inputs with tau intervals when assumptions vary.

### Touchpoints

- [examples/portfolio.eta](C:/Users/lewis/develop/eta/examples/portfolio.eta)
- [docs/portfolio.md](C:/Users/lewis/develop/eta/docs/portfolio.md)
- optional new helper module under `stdlib/std/` (for robustness utilities)

### Deliverables

- DAG perturbation harness:
  - edge add/remove/flip scenarios
  - optional latent-confounder stress parameter
- Partial-ID outputs:
  - `tau_min`, `tau_max` per asset
  - portfolio return bounds under DAG family
- Robustness report section:
  - "stable", "moderate", "fragile" decision labels

### Exit Criteria

- Portfolio example prints both point estimate and bounds.
- Bounded outputs exist even when some candidate DAGs are non-identifiable.

---

## Stage 3 - Uncertainty-Aware Optimization

### Scope

- Upgrade optimization target from point estimate to uncertainty-aware objective.
- Introduce robust alternatives while retaining current CLP(R)+QP workflow.

### Touchpoints

- [examples/portfolio.eta](C:/Users/lewis/develop/eta/examples/portfolio.eta)
- [stdlib/std/stats.eta](C:/Users/lewis/develop/eta/stdlib/std/stats.eta)
- [docs/clp.md](C:/Users/lewis/develop/eta/docs/clp.md)

### Deliverables

- At least one robust objective option:
  - Worst-case over confidence set of tau, or
  - Mean-variance with uncertainty penalty on tau/Sigma
- Confidence inputs sourced from:
  - bootstrap, or
  - ensemble spread, or
  - dropout-based predictive dispersion
- Decision output includes uncertainty diagnostics:
  - objective gap to nominal optimum
  - regret under worst-case tau

### Exit Criteria

- Robust mode is selectable by parameter.
- Under injected noise, robust mode shows lower downside tail than nominal mode in stress scenarios.

---

## Stage 4 - Structural or Learned Sigma(m)

### Scope

- Replace thin empirical 5-point covariance estimation with stronger second-order modeling.
- Keep PSD and optimizer compatibility guarantees.

### Touchpoints

- [examples/portfolio.eta](C:/Users/lewis/develop/eta/examples/portfolio.eta)
- [stdlib/std/torch.eta](C:/Users/lewis/develop/eta/stdlib/std/torch.eta)
- [docs/portfolio.md](C:/Users/lewis/develop/eta/docs/portfolio.md)

### Deliverables

- Option A (structural): parametric Sigma(m) tied to macro regime and sector structure.
- Option B (learned): two-head model for mean and covariance factors.
- PSD enforcement strategy documented (e.g. factor form or Cholesky-style parameterization).
- Runtime knob to choose:
  - empirical grid Sigma(m)
  - structural Sigma(m)
  - learned Sigma(m)

### Exit Criteria

- New Sigma(m) path is stable and PSD in all tested scenarios.
- Portfolio solve remains convex and numerically stable.

---

## Stage 5 - Empirical Validation Under Broken Assumptions

### Scope

- Prove practical value via comparative stress tests, not only in-DGP correctness.

### Touchpoints

- [examples/portfolio.eta](C:/Users/lewis/develop/eta/examples/portfolio.eta)
- [docs/portfolio.md](C:/Users/lewis/develop/eta/docs/portfolio.md)
- optional benchmark script in `scripts/`

### Deliverables

- Evaluation matrix:
  - DGP-correct
  - DAG misspecified
  - latent confounding injected
  - noise/regime shift
- Baselines:
  - mean-variance with empirical moments
  - simple factor-tilt heuristic
  - non-causal ML predictor + optimizer
- Metrics:
  - OOS return
  - downside risk
  - regret vs structural oracle
  - degradation slope under misspecification

### Exit Criteria

- Demonstrated "fails less badly" behavior for robust causal mode on at least one misspecified regime family.
- Results are reproducible from documented commands.

---

## Stage 6 - Dynamic Causal Control (Advanced Track)

### Scope

- Add feedback: actions affect market state and future rewards.
- Move from one-shot optimization to sequential causal decision-making.

### Touchpoints

- new dynamic example (recommended): `examples/portfolio_dynamic.eta`
- actor/runtime docs: [message-passing.md](C:/Users/lewis/develop/eta/docs/message-passing.md)

### Deliverables

- Dynamic SCM/state transition with action-dependent terms:
  - market impact
  - liquidity penalty
  - crowding feedback
- Multi-step objective and policy evaluation loop.
- Scenario actors for parallel rollouts.

### Exit Criteria

- Closed-loop simulation shows policy behavior adapts over time.
- Static portfolio mode remains available and unchanged.

---

## Recommended Implementation Order (Maximum Impact, Minimum Scope Creep)

1. Stage 2 (DAG sensitivity + partial-ID bounds)
2. Stage 3 (uncertainty-aware optimization)
3. Stage 4 (structural/learned Sigma(m))

This trio gives the fastest path to "useful when wrong" while preserving current architecture.

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Over-complex APIs in `std.causal` | Slower adoption | Add new APIs without breaking existing `do:identify` usage |
| Robust optimization too conservative | Lower headline returns | Expose risk-aversion and uncertainty-size knobs |
| Learned covariance instability | Solver failures | Enforce PSD by construction and keep fallback Sigma path |
| Validation overfitting to synthetic worlds | Weak external credibility | Include multiple misspecification families and baseline comparators |

---

## Definition of "Exceptional" for Eta Causal Portfolio

Eta reaches the target when it can produce allocations with:

1. Explicit causal assumptions and sensitivity to assumption failures.
2. Structurally coherent first- and second-order modeling (mean and covariance).
3. Decision rules that remain competitive under uncertainty and misspecification.
4. Reproducible evidence that robust causal mode degrades more gracefully than standard alternatives.
