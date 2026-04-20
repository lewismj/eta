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
| 2 | Misspecification robustness | DAG sensitivity + partial-identification bounds on returns | Implemented (2026-04-20) |
| 3 | Uncertainty-aware optimization | Robust portfolio choice under parameter uncertainty | Implemented (2026-04-20) |
| 4 | Structural/learned covariance | Better Sigma(m) aligned with causal structure | Implemented (2026-04-20) |
| 5 | Empirical stress-test suite | Evidence of graceful degradation vs baselines | Implemented (2026-04-20) |
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

Current status: implemented in `std.causal` and `examples/portfolio.eta` with
DAG perturbation analysis, observed-set-aware identification, tau bounds, and
decision-sensitivity diagnostics.

### Scope

- Evaluate causal outputs across plausible DAG variants.
- Replace single-point tau inputs with tau intervals when assumptions vary.

### Touchpoints

- [examples/portfolio.eta](C:/Users/lewis/develop/eta/examples/portfolio.eta)
- [stdlib/std/causal.eta](C:/Users/lewis/develop/eta/stdlib/std/causal.eta)
- [stdlib/tests/causal.test.eta](C:/Users/lewis/develop/eta/stdlib/tests/causal.test.eta)
- [docs/portfolio.md](C:/Users/lewis/develop/eta/docs/portfolio.md)

### Deliverables

- DAG perturbation harness:
  - edge add/remove/flip scenarios
  - optional latent-confounder stress parameter
- Partial-ID outputs:
  - `tau_min`, `tau_max` per asset
  - portfolio return bounds under DAG family
- Decision sensitivity outputs:
  - argmax stability analysis across DAG variants
  - frequency of optimal-allocation changes
  - decision regret across the DAG family
- Robustness report section:
  - "stable", "moderate", "fragile" decision labels

### Exit Criteria

- Portfolio example prints both point estimate and bounds.
- Bounded outputs exist even when some candidate DAGs are non-identifiable.
- Portfolio example reports decision-sensitivity diagnostics (stability + regret).

---

## Stage 3 - Uncertainty-Aware Optimization

Current status: implemented in `examples/portfolio.eta` with selectable
optimization modes (`nominal`, `worst-case`, `uncertainty-penalty`), where
confidence inputs come from Stage 2 DAG-family `tau_min`/`tau_max` bounds and
scenario-dependent Sigma dispersion.

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

Current status: implemented in `examples/portfolio.eta` with four runtime
Sigma modes (`empirical-grid`, `structural`, `learned-residual`, `hybrid`)
plus sampled PSD/stability diagnostics for all scenario covariances.

### Scope

- Replace thin empirical 5-point covariance estimation with stronger second-order modeling.
- Keep PSD and optimizer compatibility guarantees.

### Touchpoints

- [examples/portfolio.eta](C:/Users/lewis/develop/eta/examples/portfolio.eta)
- [stdlib/std/torch.eta](C:/Users/lewis/develop/eta/stdlib/std/torch.eta)
- [docs/portfolio.md](C:/Users/lewis/develop/eta/docs/portfolio.md)

### Deliverables

- Step 1 (implemented): Option A (structural) Sigma(m) tied to beta exposures,
  macro volatility regime, and sector correlation structure.
- Step 2 (implemented): validation checks that Sigma(boom) exceeds
  Sigma(recession) under the structural path and optimisation remains stable.
- Step 3 (implemented): Option B (learned) residual covariance via a
  residual factor model learned from NN forecast errors.
- Step 4 (implemented): hybrid structural + learned covariance blend with
  configurable structural weight.
- Runtime knob supports:
  - empirical grid Sigma(m)
  - structural Sigma(m)
  - learned residual Sigma(m)
  - hybrid Sigma(m)

### Exit Criteria

- Learned and hybrid Sigma(m) paths are stable and sampled-PSD in all tested scenarios.
- Portfolio solve remains convex and numerically stable.

---

## Stage 5 - Empirical Validation Under Broken Assumptions

Current status: implemented in `examples/portfolio.eta` with an explicit
evaluation matrix (`dgp-correct`, `dag-misspecified`,
`latent-confounding`, `noise-regime-shift`), baseline comparators, and
machine-readable stress-validation diagnostics in `run-pipeline`.

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
- Keep the showcase centered on `examples/portfolio.eta` and `docs/portfolio.md`.

### Touchpoints

- primary showcase code: [examples/portfolio.eta](C:/Users/lewis/develop/eta/examples/portfolio.eta)
- primary showcase docs: [docs/portfolio.md](C:/Users/lewis/develop/eta/docs/portfolio.md)
- optional supporting actor/runtime reference:
  [message-passing.md](C:/Users/lewis/develop/eta/docs/message-passing.md)

### Deliverables

- Dynamic SCM/state transition with action-dependent terms:
  - market impact
  - liquidity penalty
  - crowding feedback
- Multi-step objective and policy evaluation loop exposed from
  `examples/portfolio.eta` (for example as a dynamic mode or sibling entrypoint).
- Scenario actors for parallel rollouts as an implementation option, not a
  documentation-first requirement.

### Exit Criteria

- Closed-loop simulation shows policy behavior adapts over time.
- `docs/portfolio.md` documents static and dynamic usage from a single showcase path.
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
