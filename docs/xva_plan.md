# Plan: XVA "Elite-Tier Wow" Demo for Eta

A single ~1 200 LoC demo (`examples/xva-wow/`, one `main.eta`) built
around **one headline idea**, with the supporting machinery shown
inline so the language story is also visible:

> **"CVA is wrong because it ignores causality — here is how it changes
> under a real shock."**

If a viewer remembers only one sentence afterwards it must be:
*"They can compute CVA under interventions, not just correlations."*
If they remember a second, it should be:
*"All of it — symbolic, AD, QP, supervisor — was one program."*

The run is one binary, one entry point, ~35 s end-to-end. Sections §1–§5
are the headline narrative; §6–§9 are tight (≤30 s each) "earn your
keep" sections that answer the obvious objections from a sophisticated
viewer.

---

## Scope: five demo components, one program

### 1. Exposure engine (credibility plumbing)

- 200 swaps, 30 counterparties, CSA (threshold/MTA/MPoR).
- HW / G2++ short-rate model + FX, correlated factors.
- Monte Carlo paths → per-netting-set EE curve.
- Just enough realism to be defensible — **no narration of the maths**.

### 2. Fast CVA + AAD sensitivities (desk relevance)

- Total CVA + top-N counterparty contributions.
- All ΔCVA risk-factor sensitivities in **one backward pass** on the
  scalar AAD tape (~0.4 s budget).
- One printed speed number. That's it.

### 3. Causal WWR via do-calculus (the punch)

- A small SCM linking `macro-oil → {usd-rates, fx, energy-credit} →
  {exposure, hazard}`.
- Run `do(oil = -30%)`: re-simulate exposure and hazards under the
  intervention, recompute CVA per counterparty.
- Print the **ranking change** and the **counterparty that explodes**
  (e.g. `CP-17: 6.1M → 11.4M, +87%`).
- One paragraph of plain-finance interpretation.

### 4. Counterfactual sweep (the depth behind the punch)

- Run a small battery of `do(...)` interventions:
  `do(oil=-30%)`, `do(oil=+30%)`, `do(usd-10y=+100bp)`,
  `do(usd-10y=-100bp)`, `do(eur-usd=-15%)`.
- For each, compute book CVA and per-CP CVA from the same tilted
  re-simulation pipeline.
- Rank counterparties by **causal elasticity** ε = ∂log CVA / ∂(do-var)
  estimated from the symmetric pair.
- Print a single ranked table: top wrong-way names, top right-way names,
  most macro-insensitive names. One paragraph: *this is the heat-map a
  desk actually wants*.

### 5. Symbolic kernel + Compression QP + Supervisor

- The three "earn your keep" sections (§6–§9 in the output) live inline
  in the same run. See "objection-answering sections" below.

---

## Out-of-scope (cut entirely)

These remain Eta capabilities but contribute nothing to *this* demo:

- ❌ CLP(B) / Prolog legal-cycle selection (no decision-relevant story
  without a multi-jurisdiction narrative the audience does not want).
- ❌ FactTable / ad-hoc query engine (slow, distracting; doesn't prove
  anything Pandas can't).
- ❌ DVA / ColVA / MVA / KVA — ship CVA (and optionally FVA) only.
- ❌ Multi-currency book beyond what the oil-shock story needs
  (USD + an energy-heavy CP cluster + EUR/GBP for one FX `do(...)`
  query is enough).

---

## Module layout — `examples/xva-wow/`

One entry point, one binary, ~1 200 LoC:

| #   | File | Responsibility |
| --- | --- | --- |
| 1   | `main.eta` | One narrated run, §1–§9 below. |
| 2   | `book.eta` | Seeded deterministic 200-swap / 30-CP book + CSA terms. |
| 3   | `market.eta` | OIS curves, G2++ params, FX, hazard term-structures, factor correlation. |
| 4   | `paths.eta` | Torch batched G2++ × FX path generator → MtM cube → per-NS EE. Also exposes a `simulate-paths-tilted` entry point used by the counterfactual sweep. |
| 5   | `xva.eta` | CVA per CP + total + AAD sensitivities (`grad` over hazards, recoveries, curve pillars, FX). |
| 6   | `wwr-causal.eta` | SCM definition, `do(...)` intervention, tilted re-simulation, single-shock CVA, **counterfactual sweep + elasticity ranking**. |
| 7   | `symbolic.eta` | Build swap-leg pricer as a symbolic expression, `simplify*`, derive `∂V/∂r` symbolically, lower to a closure that records onto the **same** AAD tape used in `xva.eta`. |
| 8   | `compress.eta` | Convex QP (`clp:rq-minimize`) for IM-/funding-optimal unwind set against the WWR-identified hot CPs. Inputs: per-trade DV01 buckets, FX deltas, SIMM-style Σ literal. |
| 9   | `workers.eta` | One `path-worker` actor under `std.supervisor`. The path-gen call in `paths.eta` is routed through it. §9 of the run kills the worker mid-shard, supervisor restarts, result hash is asserted unchanged. |

No fact table, no Prolog, no CLP(B), no DVA/ColVA/MVA/KVA.

---

## The demo flow (terminal output, single run)

§1–§5 are the headline narrative (~15 s). §6–§9 are the
"earn your keep" sections (~20 s combined), each tied to a specific
objection a sophisticated viewer will raise.

```
══════════════════════════════════════════════════════════════
 §1 BOOK         200 swaps · 30 counterparties · CSA loaded
 §2 BASELINE     Monte Carlo exposure · 5 000 paths
                 Book CVA = 18.42 M USD
                 Top 3:  CP-17  6.10 M
                         CP-04  3.80 M
                         CP-22  2.60 M
 §3 SENSITIVITIES  110 risk factors · one AAD backward pass · 0.41 s
 §4 INTERVENTION   do(oil = -30%)
                   re-simulating exposure + hazard under SCM ...
                   Book CVA = 24.90 M USD   (+35%)
                   Top 3:  CP-17 11.40 M  (+87%)   ← energy name
                           CP-09  4.95 M  (+62%)
                           CP-04  4.10 M  ( +8%)
 §5 INTERPRETATION
   Oil ↓ → energy credit spreads ↑ → hazard(CP-17) ↑
   Oil ↓ → USD rates ↓ + FX shift → exposure(CP-17) ↑
   Both legs widen jointly. This is wrong-way risk driven by a
   *causal* macro shock, not by a static correlation multiplier.

 §6 COUNTERFACTUAL SWEEP   5 do(...) queries · 1 000 paths each
   do(oil       = -30%)  ΔBook +35.3%   topΔ CP-17 +87%
   do(oil       = +30%)  ΔBook -19.1%   topΔ CP-17 -41%
   do(usd-10y   = +100bp) ΔBook  +8.4%  topΔ CP-04 +22%
   do(usd-10y   = -100bp) ΔBook  -7.6%  topΔ CP-04 -19%
   do(eur-usd   = -15%)  ΔBook  +6.1%   topΔ CP-22 +29%
   causal elasticity ε = ∂log CVA / ∂(do-var):
     wrong-way:  CP-17 (oil 2.91) · CP-09 (oil 2.07) · CP-22 (eur-usd 1.94)
     right-way:  CP-31 (oil -0.62) · CP-12 (usd-10y -0.48)
   This heat-map is conditional on intervention, not correlation.

 §7 SYMBOLIC KERNEL   (answers: "JAX already does AAD")
   building swap-leg pricer as expression tree ...
   simplify* -> 11 nodes
   derived ∂V/∂r symbolically:
     (* notional (* dcf (- 1 (exp (* (- 0 r) tau)))))
   lowered to closure, recorded onto the SAME tape as §3
   re-priced book with derived kernel: max |Δ| vs numeric = 3.2e-13 ✓

 §8 COMPRESSION QP    (answers: "so what — what do I do about CP-17?")
   targets: CP-17, CP-09 (WWR-identified in §4 + §6)
   candidate unwinds: 23 trades · DV01-bucket + FX-delta equivalence
   solving convex QP: ½Δᵀ Σ Δ + λ·funding ...
   solution: 9 unwinds · IM saved $14.2 M · CVA(CP-17) 11.4 → 6.8 M
   risk-equivalence residual: 1e-9 across all buckets ✓

 §9 SUPERVISOR        (answers: "is this production-grade?")
   path-worker pid=#<actor 0x7f..> running shard 2/4 ...
   ✱ injected fault: kill path-worker shard 2
   supervisor: restart (one-for-one) ... ok in 38 ms
   re-ran shard 2 with seeded RNG
   final book-CVA hash: 0x9a3f...c12  (matches §2) ✓
══════════════════════════════════════════════════════════════
 wall-clock 34.6 s (laptop, 8 cores, no GPU)
```

Stage timings: §1+§2 ≈ 8 s, §3 ≈ 0.4 s, §4 ≈ 4 s, §5 instant,
§6 ≈ 8 s (5 × 1 000-path tilted re-runs, batched on the same tensor
buffers as §4), §7 ≈ 2 s, §8 <1 s, §9 ≈ 12 s (dominated by re-running
one path shard).

---

## API sketches

**Path generator** — `paths.eta`

```scheme
(defun simulate-paths (curves g2pp-params fx-corr n-paths n-steps dt seed)
  ;; correlated G2++ × FX shocks via Cholesky on a single
  ;; (steps × paths × factors) tensor; returns per-NS EE curves
  ;; as plain Eta numeric vectors (so the AAD tape can consume them).
  ...)
```

**AAD CVA** — `xva.eta`

```scheme
(defun cva-with-greeks (ee-curves market-scalars)
  (grad
    (lambda (hazards recoveries . curve-pillars)
      (sum-over cps
        (lambda (cp) (cva-cp cp hazards recoveries curve-pillars))))
    market-scalars))
;; -> (cva-value . grad-vec) in one backward sweep
```

**Causal intervention** — `wwr-causal.eta`

```scheme
(define scm
  '((macro-oil -> usd-rates)
    (macro-oil -> fx-usd)
    (macro-oil -> credit-energy)
    (usd-rates -> exposure)
    (fx-usd    -> exposure)
    (credit-energy -> hazard-energy-cps)))

(defun cva-under-do (do-var do-value)
  ;; 1. (do:identify scm 'cva do-var)  -> print adjustment formula.
  ;; 2. Tilt exogenous noise so do-var = do-value (Gaussian copula on the
  ;;    factor correlation matrix).
  ;; 3. Re-run simulate-paths with tilted shocks (1 000 paths).
  ;; 4. Recompute per-CP CVA.
  (values total-cva per-cp-cva-table))

(defun counterfactual-sweep (queries baseline)
  ;; queries : '((oil -0.30) (oil 0.30) (usd-10y 0.01) ...)
  ;; For each: cva-under-do, store ΔBook and per-CP Δ.
  ;; Estimate elasticity ε(cp, var) from symmetric ± pairs.
  ;; Returns a ranked report: wrong-way / right-way / insensitive.
  ...)
```

That is the entire surface area the viewer sees.

---

## Required Eta additions (minimal)

| #   | Capability | Where | LoC |
| --- | --- | --- | --- |
| 1   | `%torch-mvnormal` (multivariate-normal sampler) | `eta/torch/src/eta/torch/torch_primitives.h` + `stdlib/std/torch.eta` | ~30 |
| 2   | `%torch-cholesky` | same | ~20 |
| 3   | `stats:normal-quantile` (for copula tilt) | `stats_math.h` + wrapper | ~15 |
| 4   | `%torch-manual-seed` (per-actor reproducibility for §9 hash assertion) | `eta/torch/src/eta/torch/torch_primitives.h` | ~10 |
| 5   | Numeric SCM forward-sim helper (`scm:sample`, topo-sort + linear-Gaussian mechanisms) | pure Eta in `wwr-causal.eta` | ~80 |
| 6   | Counterfactual-sweep + elasticity reducer | pure Eta in `wwr-causal.eta` | ~60 |
| 7   | Symbolic-algebra helpers: `-`, `/`, `sqrt`, chain-rule extension reused from `examples/symbolic-diff.eta` | pure Eta in `symbolic.eta` | ~120 |
| 8   | "Lower expression to AAD-recording closure" — walk the simplified tree, emit calls into the active tape | pure Eta in `symbolic.eta` | ~60 |
| 9   | SIMM-style IR-delta covariance literal (~10×10) for the QP | pure Eta in `compress.eta` | ~30 |

Total runtime change: **~75 LoC C++** (items 1–4). Everything else is
Eta in `examples/xva-wow/`. Dropped from earlier drafts: day-count
library beyond ACT/365F, `clpb` cardinality wrapper, FactTable wiring.

---

## Performance budget

| Stage | Laptop (8c) | Notes |
| --- | --- | --- |
| §1 + §2 Path gen 200 × 5 000 × 120, agg + CSA | ~10 s | torch batched + tensor reductions |
| §3 AAD CVA + Greeks | ~0.4 s | ~110 risk factors, one backward sweep |
| §4 `do(oil)` re-simulation | ~4 s | 1 000 tilted paths |
| §6 Counterfactual sweep (5 × 1 000 paths) | ~8 s | reuses tilted-path pipeline; batched |
| §7 Symbolic build + lower + reprice check | ~2 s | tree rewrite + one pricing pass |
| §8 Compression QP | <1 s | active-set on ~25 vars |
| §9 Kill + restart + re-shard | ~12 s | dominated by re-running one path shard |
| **Total** | **~37 s** |     |

---

## Why this version works

- **One clear innovation**: causal WWR via `do(...)`, deepened by the
  counterfactual sweep that turns a single shock into a ranked
  intervention heat-map.
- **One credibility anchor**: XVA + AAD on a realistic book.
- **One decision implication**: ranking flips, hedge target changes,
  compression QP produces the actual unwind set.
- **One language story**: §7–§9 prove that symbolic algebra, AD,
  convex optimisation and supervised actors live in the same program,
  on the same tape, in the same source file.

Each "earn your keep" section answers exactly one objection:

| Section | Objection answered |
| --- | --- |
| §6 Counterfactual sweep | *"One shock could be cherry-picked."* — here are five, with elasticities. |
| §7 Symbolic→AAD | *"JAX already does AAD."* — JAX cannot host CAS on the same tape without a Python source-to-source step. |
| §8 Compression QP | *"OK, the CVA changed — so what?"* — here is the actionable unwind set. |
| §9 Supervisor | *"Is this a toy?"* — kill a worker, hash matches. |

Mental test: if the viewer recalls *"there was some Prolog"* the demo
failed. If they recall *"causal CVA, and all of it was one program"*,
the demo won.

---

## Open questions

1. **CVA-only or CVA + FVA?** Recommend CVA-only for v1 — FVA adds
   ~50 LoC and another funding-spread sensitivity but no narrative lift.
2. **GPU path?** Silent CPU fallback; do not branch the demo on
   hardware.
3. **Counterfactual sweep size.** 5 queries × 1 000 paths is the sweet
   spot for ~8 s. Expose `--counterfactual-paths` and
   `--counterfactual-queries` flags but default to the demo values.
4. **§9 hash assertion.** Must use seeded torch RNG and a deterministic
   reduction order; fail loudly if the hash diverges so the demo cannot
   silently lie.