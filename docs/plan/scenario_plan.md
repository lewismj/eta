# Featured Example Plan — Structural Stress Test & Rebalancing Under an Energy Shock

[← Back to README](../../README.md) ·
[xVA-WWR featured example](../featured_examples/xva-wwr.md) ·
[Causal plan](./causal_plan.md) ·
[AAD](../guide/reference/aad.md) ·
[Torch](../guide/reference/torch.md)

---

## 1 — Executive Summary

A new featured example, **`examples/energy-stress/`**, that takes a realistic
~$1bn global multi-asset portfolio (SPY/VGK/XLE/SHY/IEF/USO/EURUSD), **learns
a structural causal DAG from real historical daily returns** with a
NOTEARS-style neural network (Zheng 2018) using `std.torch`, applies a
**structural energy shock** (oil +40 %, energy-equity drawdown, USD-up,
rates-reprice) as a `do(...)`-intervention on the learned SCM, propagates
the shock through the per-node MLP mechanisms via Monte-Carlo, computes the
stressed P&L distribution, VaR/ES, AAD-greeks, and **solves an optimal
rebalance** (min stressed-ES s.t. weight/turnover constraints) using the
existing CLP(R)/QP machinery. The example mirrors the file layout, module
conventions, friendly-report style, and CI-friendly determinism guarantees
of [`examples/xva-wwr/`](../../examples/xva-wwr).

The example differs from `xva-wwr` in three load-bearing ways:

1. The DAG is **learned from real data**, not hand-coded.
2. The shocked unit is a **traded portfolio**, not a CVA book.
3. The output of the pipeline is an **executable rebalance vector**, not just
   risk numbers.

---

## 2 — Goals & Non-Goals

### Goals

- End-to-end runnable demo (`etai examples/energy-stress/main.eta`) producing
  a single deterministic artifact, plus a friendly stage-by-stage report.
- Demonstrate **NOTEARS-style structure learning** on real returns using
  `std.torch` (acyclicity + L1 + augmented Lagrangian).
- Show a **structural** stress propagation: shocked returns are sampled
  from the learned per-node MLP mechanisms, *not* a static covariance bump.
- Compute **AAD-greeks** (`grad`) of stressed loss w.r.t. (weights, shock
  magnitude, propagation strength).
- Solve a **convex rebalancing QP** with weight bounds and turnover budget
  via `clp:rq-minimize` (same surface as `xva-wwr`'s `compress.eta`).
- Compare structural vs **correlation-only** stress, mirroring §7b of the
  xVA-WWR doc, to make the value-add legible.

### Non-Goals

- Production data feed / live broker integration.
- Multi-period portfolio optimisation (single-shot rebalance only).
- Tax / transaction-cost modelling beyond a linear turnover penalty.
- A full causal-discovery survey (we pick **one** method — NOTEARS — and
  reference the others).
- Re-implementation of the existing `std.causal` identification machinery;
  we **reuse** `examples/do-calculus/dag.eta` primitives and
  `stdlib/std/causal.eta`.

---

## 3 — Directory Layout

```text
examples/energy-stress/
├── main.eta            ;; orchestrator: returns single alist artifact (run-demo)
├── data.eta            ;; CSV loader + log-returns + standardisation + train/test split
├── market.eta          ;; asset universe, weights, notional, baseline stats
├── portfolio.eta       ;; portfolio P&L given a return vector and a weight vector
├── dag-learn.eta       ;; NOTEARS-style NN learner over std.torch — outputs W (d×d)
├── dag.eta             ;; learned-DAG container, threshold, topo-sort, do(...) helper
│                       ;; (extends/wraps examples/do-calculus/dag.eta)
├── shock.eta           ;; shock specification + intervention encoding (do-vector)
├── stress.eta          ;; SCM forward-sim, MC P&L, VaR/ES, marginal contributions
├── rebalance.eta       ;; convex rebalancing QP via std.clpr (clp:rq-minimize)
├── workers.eta         ;; deterministic shard replay + parallel MC via std.net
├── report.eta          ;; friendly stage-by-stage console report (mirrors xva-wwr)
├── sample-output.txt   ;; committed seeded run for diff-as-CI signal
└── notebook.ipynb      ;; optional Jupyter view: learned DAG, P&L hist, Qini-style chart

scripts/
└── fetch_returns.py    ;; one-off Yahoo Finance puller -> data/energy-stress-returns.csv

data/
└── energy-stress-returns.csv   ;; committed snapshot (small ~ 100 KB) for reproducibility

docs/featured_examples/
└── energy-stress.md    ;; produced **after** implementation; mirrors xva-wwr.md
```

`main.eta` exports `run-demo` (side-effect-free, returns the artifact alist)
and a CLI `(defun main ...)` hook, matching the `xva-wwr` convention.

---

## 4 — Data Sources

| Asset class | Instrument | Weight | Notional |
|---|---|---:|---:|
| US Equity | SPY | 35 % | $350 m |
| Europe Equity | VGK | 15 % | $150 m |
| Energy Equity | XLE | 10 % | $100 m |
| US Bonds 2Y | SHY | 10 % | $100 m |
| US Bonds 10Y | IEF | 15 % | $150 m |
| Commodities | USO | 10 % | $100 m |
| FX Overlay | EUR/USD | 5 % | $50 m |

**Pull script:** `scripts/fetch_returns.py` uses `yfinance` to download daily
adjusted close 2015-01-01 → 2025-12-31 for the seven tickers, computes
**log returns** $r_t = \log(P_t / P_{t-1})$, aligns dates by inner-join,
writes `data/energy-stress-returns.csv` with header
`date,SPY,VGK,XLE,SHY,IEF,USO,EURUSD`. EUR/USD is taken as `EURUSD=X`.

**Eta-side preprocessing** (`data.eta`) — uses **`std.fact_table`**, not
`std.csv` + alists. The fact-table is column-major (`std::vector<LispVal>`
per column), GC-managed, supports per-column hash indexes, native
`fact-table-load-csv`, and `fact-table-group-*` aggregations — exactly the
shape this workload wants. See
[`docs/guide/reference/fact-table.md`](../guide/reference/fact-table.md)
and [`examples/fact-table.eta`](../../examples/fact-table.eta).

1. **Load** with the C++-side parser:
   ```scheme
   (define raw
     (fact-table-load-csv "data/energy-stress-returns.csv"
                          '((header? . #t)
                            (numeric-cols . (1 2 3 4 5 6 7))
                            (date-col . 0))))
   ;; => #<fact-table 8cols × ~2700rows>
   ```
2. **Validate**: `(fact-table-row-count raw)` ≥ 2000; assert no NaNs by
   `fact-table-fold` over each numeric column (cheap column-major scan,
   no per-row allocation).
3. **Index** the date column for shard-replay lookups:
   `(fact-table-build-index! raw 0)`.
4. **Standardise** each numeric column in place via a column-scan helper
   (`fact-table-fold` for $\mu_i, \sigma_i$, then a fresh `returns` table
   built with `fact-table-insert!`). Persist `((SPY . (μ . σ)) ...)` as
   the `scale` alist for back-transform.
5. **Train/test split 80/20 by time** without shuffling. Implement as
   `fact-table-partition` on a synthesised `'split` column (`'train` /
   `'test`) appended once during standardisation — yields two
   sub-`fact-table`s sharing the same column schema.
6. Return a small record bundling the two tables plus metadata:

   ```scheme
   (define-record-type <returns-panel>
     (make-returns-panel train test scale tickers)
     returns-panel?
     (train   returns-panel-train)    ; fact-table  (T_train × 7)
     (test    returns-panel-test)     ; fact-table  (T_test  × 7)
     (scale   returns-panel-scale)    ; alist of (ticker . (μ . σ))
     (tickers returns-panel-tickers)) ; list of column symbols
   ```

**Why the fact-table and not a list-of-alists / `std.csv`:**

| Concern | List-of-alists | `fact-table` |
|---|---|---|
| Memory for 2 700 × 7 doubles | ~7 cons cells/row × 2 700 = 19 k pairs + symbol keys per cell | 7 contiguous `vector<LispVal>` columns; one allocation per column |
| Column scan (μ, σ, sums) | O(n) with pointer chasing per cell | Cache-friendly column iteration |
| Date / regime lookup | Linear `assoc` per query | O(1) via `fact-table-build-index!` on the date column |
| Group/aggregate (e.g., regime-window slicing in §12) | Hand-rolled `foldl` | `fact-table-group-by`, `fact-table-partition` |
| Tensor handoff to `std.torch` | Convert via nested `map` allocations | Direct column-vector copy into a torch tensor (one strided memcpy per column) |
| GC pressure during MC inner loops | High (cons-heavy) | Low (one heap object) |

The torch tensor handoff in §5 is the load-bearing one: NOTEARS training
runs O(10⁴) inner steps and we want the input batch to come from a single
columnar source, not a re-walked alist tree.

---

## 5 — NN-Based DAG Learning (NOTEARS-style)

`dag-learn.eta` implements **NOTEARS-MLP** (Zheng et al. 2018, 2020):

For each asset $i$ we fit an MLP $f_i: \mathbb{R}^{d-1} \to \mathbb{R}$ that
predicts $\tilde r_{i,t}$ from $\tilde r_{-i, t}$. The **first-layer weight
matrix** of each MLP — masked so that $W_{ii}=0$ — encodes the directed
parent-child relationships. Stack the per-node first-layer column-norms into
$W \in \mathbb{R}^{d \times d}$ where $W_{ji}$ is the strength of edge
$j \to i$.

### Loss

$$
\mathcal{L}(\theta, W) \;=\;
\underbrace{\tfrac{1}{2T}\sum_t \lVert \tilde r_t - f_\theta(\tilde r_t) \rVert_2^2}_{\text{reconstruction}}
\;+\; \lambda_1 \lVert W \rVert_1
\;+\; \lambda_2 \lVert \theta \rVert_2^2
\;+\; \tfrac{\rho}{2} h(W)^2
\;+\; \alpha \, h(W)
$$

with the **acyclicity constraint** (Zheng 2018):
$$
h(W) \;=\; \mathrm{tr}\bigl(e^{W \circ W}\bigr) - d \;=\; 0 \;\Longleftrightarrow\; W \text{ is a DAG.}
$$

### Augmented-Lagrangian outer loop

```text
ρ ← 1.0 ; α ← 0.0
repeat for k = 1..K_outer:
  inner_loop: minimise L(θ, W) for N_inner Adam steps at fixed (ρ, α)
  if h(W) > 0.25 · h_prev: ρ ← 10·ρ
  α ← α + ρ · h(W)
  h_prev ← h(W)
until h(W) < 1e-8 or k = K_max
```

### Tensor shapes (d = 7)

| Object | Shape | Note |
|---|---|---|
| Input batch | `(B, 7)` | standardised returns |
| Per-node MLP | `Linear(7, 32) → ReLU → Linear(32, 1)` | column $i$ of first layer masked at row $i$ |
| First-layer stack | `(7, 7, 32)` | reduced to `W: (7, 7)` via column 2-norm |
| `h(W)` | scalar | needs **matrix exponential** |

### Matrix exponential

`std.torch` does **not** currently expose `torch/matrix-exp` (verified by
grep). Two options — pick (b) for the demo:

- **(a) Add binding.** Surface `torch/matrix_exp` from libtorch as
  `torch:matrix-exp`; add to `std.torch` exports. ~30 LoC C++ glue.
- **(b) Power-series approximation.** The acyclicity term tolerates a
  truncated series; use Yu et al. (2019)
  $\tilde h(W) = \mathrm{tr}\bigl((I + \tfrac{1}{d} W \circ W)^{d}\bigr) - d$
  which only needs `matmul` and is already available. Document that the
  approximation is a deliberate choice; add (a) as a follow-up.

### Output

```scheme
(learn-dag panel
           '(lambda1 0.05 lambda2 1e-4
             hidden 32 epochs-inner 200
             outer-iters 8 lr 1e-3
             threshold 0.30 seed 20260427))
;; => ((W . #<7x7 tensor>)
;;     (W-thresholded . ((SPY . (XLE USO)) (XLE . (USO)) ...))   ; alist of parents
;;     (h-final . 4.2e-9)
;;     (loss-trace . (((epoch . 1) (loss . _)) ...))
;;     (per-node-mlps . #<list of 7 nn-modules>))
```

### Validation against domain prior

A small **prior-edge audit** prints which expected directional edges were
recovered: `oil → energy-equity`, `oil → broad-equity`, `rates → bonds`,
`fx → europe-equity`. Pass criterion: ≥ 5 of 7 prior edges present after
thresholding; printed as a green/red panel in `report.eta`.

---

## 6 — Structural Shock Propagation

Encode the learned DAG as the alist format used by
[`examples/do-calculus/dag.eta`](../../examples/do-calculus/dag.eta) so the
existing `dag:topo-sort`, `dag:ancestors`, `dag:descendants`,
`dag:satisfies-backdoor?` primitives are reusable verbatim:

```scheme
;; Convert thresholded W into edge list:
(define learned-edges
  '((oil -> XLE) (oil -> USO) (oil -> SPY) (rates -> SHY) (rates -> IEF)
    (fx  -> VGK) ...))   ;; produced by (dag:from-W W tickers threshold)
```

`shock.eta` defines a structural shock as a `do`-alist plus a
**propagation strength** $\kappa \in [0,1]$:

```scheme
(define energy-shock
  '((oil      .  0.40)     ; +40% oil
    (rates    .  0.0050)   ; +50bps repricing
    (fx       . -0.05)))   ; USD strengthens 5%
```

### Forward simulation (one MC draw)

```text
order ← (dag:topo-sort learned-edges)
for v in order:
   if v ∈ do-keys: tilde_r[v] ← do[v]            ;; surgical intervention
   else:           tilde_r[v] ← f_v(tilde_r[parents(v)]) + ε_v
                                  (ε_v sampled from residual distribution)
returns ← unstandardise(tilde_r, scale)
```

Repeat for `n-paths` (default 5000) seeded by `manual-seed seed`. The
residuals $\varepsilon_v$ are drawn from the **empirical residual** of the
training fit (block-bootstrap to preserve mild residual autocorrelation —
optional; default i.i.d.).

### Identification audit (cheap)

Run `(do:identify-details learned-edges 'XLE 'oil)` and
`(do-rule2-applies? ...)` against the learned edges and print the status
line, mirroring the §6 audit panel of xVA-WWR. Most queries collapse to
`direct` because the structural shock targets a root.

---

## 7 — Portfolio P&L & Risk Metrics

`portfolio.eta`:

$$
\mathrm{PnL}_t \;=\; \sum_i w_i \cdot N_{\mathrm{total}} \cdot r_{i,t}
\qquad N_{\mathrm{total}} = 1{,}000{,}000{,}000.
$$

`stress.eta` consumes the MC matrix `R: (n-paths, 7)`:

| Metric | Definition |
|---|---|
| Stressed mean P&L | $\mathbb{E}[\mathrm{PnL}]$ over MC |
| Stressed median P&L | quantile 0.5 |
| VaR 95 / 99 | $-Q_\alpha(\mathrm{PnL})$ |
| ES 95 / 99 | $-\mathbb{E}[\mathrm{PnL} \mid \mathrm{PnL} \le Q_\alpha]$ |
| Marginal contribution per asset | $\partial \mathrm{ES}/\partial w_i$ via AAD |
| Greek vector | $\nabla_{w, \kappa, \text{shock}} \mathrm{ES}$ via `(grad ...)` |

```scheme
(defun stressed-es-with-greeks (R weights shock kappa)
  (grad
    (lambda (w1 w2 w3 w4 w5 w6 w7 k oil-mag)
      (es-of-portfolio R (list w1 w2 w3 w4 w5 w6 w7) k oil-mag))
    (append weights (list kappa (assoc-ref 'oil shock)))))
```

The function returns `(value . #(grad-vector))` in one tape sweep —
identical pattern to `cva-with-greeks` in
[`examples/xva-wwr/xva.eta`](../../examples/xva-wwr/xva.eta).

---

## 8 — Rebalancing Optimisation

**Decision variable:** new weight vector $w' \in \mathbb{R}^7$.
**Auxiliary:** trade vector $\tau = w' - w_0$ (signed), with absolute trade
$|\tau_i|$ encoded via two non-negative auxiliaries $\tau_i^+, \tau_i^-$ so
the QP stays convex / linear.

$$
\min_{w'} \; \tfrac12 \, w'^{\top} \hat\Sigma_{\text{stressed}} \, w'
\;+\; \lambda_{\text{cost}} \sum_i (\tau_i^+ + \tau_i^-)
$$

subject to:

- $\sum_i w'_i = 1$
- $\underline w_i \le w'_i \le \overline w_i$  (per-class bounds)
- $\sum_i (\tau_i^+ + \tau_i^-) \le T_{\max}$  (turnover budget, default 0.20)
- $\hat\mu^{\top} w' \ge \mu_{\text{target}}$  (optional return floor)
- $\tau_i = \tau_i^+ - \tau_i^-, \; \tau_i^\pm \ge 0$

$\hat\Sigma_{\text{stressed}}$ is the **sample covariance of the MC stressed
return matrix** from §7 — NOT a historical covariance. This is the load-bearing
choice that makes the rebalance *causal*: the optimiser sees the structural
shock's covariance, not unconditional history.

**Solver:** reuse `clp:rq-minimize` from `std.clpr`, exactly as
[`examples/xva-wwr/compress.eta`](../../examples/xva-wwr/compress.eta) does
for the SIMM-Σ QP. Output:

```scheme
((old-weights      . (0.35 0.15 0.10 0.10 0.15 0.10 0.05))
 (new-weights      . (0.28 0.13 0.04 0.14 0.21 0.06 0.14))
 (turnover         . 0.183)
 (stressed-es-old  . 8.42e7)
 (stressed-es-new  . 4.91e7)
 (es-improvement   . 0.417)         ; 41.7% reduction
 (objective        . 1.23e-3))
```

A **second variant** (projected-gradient via AAD) is offered as an
alternative for non-convex extensions; the QP is the default.

---

## 9 — Reporting

### Console (`report.eta`)

Mirrors `examples/xva-wwr/report.eta` structure: nine numbered panels
(`[1/9] Data Load`, `[2/9] DAG Learning`, `[3/9] Prior-Edge Audit`,
`[4/9] Identification`, `[5/9] Shock Specification`, `[6/9] MC Stress`,
`[7/9] Risk & Greeks`, `[8/9] Rebalance QP`, `[9/9] Determinism`) each
with **WHAT / WHY / MATH / CAUSAL** prose blocks and a fixed-width table.

`sample-output.txt` is committed for seed `20260427` and acts as a
diff-as-CI signal in the same way as the xVA-WWR sample output.

### Optional notebook (`notebook.ipynb`)

Uses `std.jupyter` (xeus kernel) to render:

- The learned DAG via `dag:->mermaid` from `std.causal.render`.
- A histogram of stressed P&L overlaid with VaR/ES bars.
- Old vs new weights bar chart.
- A "structural vs correlation-only" stress comparison panel.

---

## 10 — Implementation Milestones

| ID | Scope | Deps | Effort | Risk |
|---:|---|---|---:|---|
| **M1** | `scripts/fetch_returns.py` + `data.eta` (CSV load, returns, split) | — | 0.5 d | Low |
| **M2** | `market.eta` + `portfolio.eta` + baseline (no-shock) MC sanity check | M1 | 0.5 d | Low |
| **M3** | `dag-learn.eta`: per-node MLPs, NOTEARS loss, augmented-Lagrangian outer loop, prior-edge audit | M2, `std.torch` | 3 d | **High** (numerical) |
| **M4** | `dag.eta`: learned-DAG container, thresholding, topo-sort over learned edges, `do(...)` encoding | M3, `examples/do-calculus/dag.eta` | 1 d | Med |
| **M5** | `shock.eta` + `stress.eta`: SCM forward-sim, VaR/ES, AAD greeks | M4 | 1.5 d | Med |
| **M6** | `rebalance.eta`: QP via `clp:rq-minimize`, turnover encoding, return-floor optional | M5, `std.clpr` | 1.5 d | Med |
| **M7** | `workers.eta` + `report.eta` + `main.eta` + `sample-output.txt` + `docs/featured_examples/energy-stress.md` | M1–M6 | 1.5 d | Low |

**Total:** ~10 working days of focused effort.

Each milestone ships with TAP-style tests under `examples/energy-stress/tests/`
(or `stdlib/tests/` if a primitive lands in stdlib).

---

## 11 — Required New Stdlib / Torch Bindings

| Item | Where | Status | Mitigation |
|---|---|---|---|
| `torch/matrix-exp` (libtorch `matrix_exp`) | `std.torch` | **Missing** | Use Yu et al. 2019 truncated power-series for `h(W)` (no binding required). Add the binding as a P2 follow-up. |
| `fact-table → torch tensor` bridge | `std.torch` (or `std.fact_table`) | **Missing** thin helper | Add `(fact-table->tensor ft col-idxs)` — one strided copy per column into a `(rows × len(col-idxs))` `float64` tensor. ~15 LoC; reuses `%fact-table-ref` in a tight loop or memcpys the column vector. Without it we'd fall back to a per-cell loop. |
| Empirical-residual sampler | `dag-learn.eta` (local) | New | ~20 LoC; stores residuals as a fact-table column and samples row-ids via `std.stats` `randint`. |
| `torch:column-l2-norm` (along axis) | `std.torch` thin wrapper | Missing alias | Compose from `tsum`, `t*`, `tsqrt`; or add 5-line wrapper. |
| Block-bootstrap helper | `std.stats` (optional) | Missing | Inline in `stress.eta` over the residual fact-table; promote to `std.stats` only if M5 demands it. |
| Yahoo-Finance puller | `scripts/fetch_returns.py` | Out-of-tree | Pure Python; no Eta change. |

No changes are required to the **causal** stack — we reuse
`stdlib/std/causal.eta`, `stdlib/std/causal/identify.eta`, and
`examples/do-calculus/dag.eta` as-is.

---

## 12 — Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **Overfitting the DAG** to 11 years × 7 assets | High | Spurious edges; misleading propagation | L1 + L2 reg; prior-edge audit; 80/20 train/test; report test-set reconstruction MSE; cross-validate `λ1` over 3 grid points. |
| **Look-ahead bias** | Med | Inflated apparent quality | Time-ordered split; **never** standardise using test-set stats; document explicitly. |
| **Non-stationarity** of returns (regime shifts: 2020 COVID, 2022 rates) | High | Learned DAG mixes regimes | Optional regime-window slicing (`'window 2018-01-01 2024-12-31`); compare DAGs across windows in an appendix; flag as a known limitation. |
| **Acyclicity approximation error** (no `matrix-exp`) | Med | Tiny residual cycles | Truncated $h$ + final hard threshold + cycle-check; if cycles remain, drop the lowest-weight back-edge. |
| **Shock plausibility** (no historical analogue at exactly +40 % oil) | Med | Decision-relevance challenged | Provide a side-by-side "milder" scenario (oil +20 %); cite OPEC-1973 / 2008 / 2022 ranges; treat shock as a hypothetical, not a forecast. |
| **MC noise in ES greeks** | Med | Noisy gradients | Antithetic variates; common random numbers across MC for FD cross-check; assert `‖AAD - FD‖∞ < tol` in CI. |
| **NN training non-determinism** | Low | Bit-drift across runs | `manual-seed` + CPU-only inference path; same shard-replay pattern as `examples/xva-wwr/workers.eta`. |
| **Identification claim overreach** | Med | Confuses readers vs xVA-WWR | Mirror the `[!CAUTION]` box from `xva-wwr.md` verbatim: a learned DAG is conditional on faithfulness, sufficiency, and the function class; quote what is and is not claimed. |

---

## 13 — Acceptance Criteria & Validation

1. **Reproducibility.** `(rerun-shard-and-check 2)` returns
   `(match . #t)` for the committed seed; `sample-output.txt` is
   bit-identical across runs of the same binary.
2. **DAG quality.** Prior-edge audit recovers ≥ 5 of 7 expected directional
   edges; reported test-set reconstruction $R^2 \ge 0.20$ on stressed
   target columns.
3. **Acyclicity.** $h(W_{\text{final}}) < 10^{-6}$ after thresholding;
   topological sort succeeds without cycle break.
4. **AAD vs finite-difference.**
   $\|\nabla_{w}\mathrm{ES}_{\text{AAD}} - \nabla_{w}\mathrm{ES}_{\text{FD}}\|_\infty < 10^{-3}$
   at $h=10^{-4}$ on a fixed-seed MC.
5. **Rebalance convexity.** $\hat\Sigma_{\text{stressed}} \succeq 0$
   (diagnostic before solve) and `clp:rq-minimize` returns a finite
   optimum; the new ES is **strictly less** than the old ES.
6. **Structural vs correlation baseline.** Side-by-side panel shows the
   structural shock concentrates loss on XLE / USO and recommends a
   different rebalance than the correlation-only shock; **at least one
   reported greek differs in sign or > 30 % in magnitude.**
7. **Determinism of report layout.** Diff of `sample-output.txt` is
   empty across two CI runs.

---

## 14 — References

1. **Zheng, Aragam, Ravikumar, Xing (2018).** *DAGs with NO TEARS:
   Continuous Optimization for Structure Learning.* NeurIPS. — primary
   method for `dag-learn.eta`.
2. **Zheng, Dan, Aragam, Ravikumar, Xing (2020).** *Learning Sparse
   Nonparametric DAGs.* AISTATS. — MLP variant used here.
3. **Yu, Chen, Gao, Yu (2019).** *DAG-GNN: DAG Structure Learning with
   Graph Neural Networks.* ICML. — alternative; source of the truncated
   acyclicity approximation.
4. **Pearl (2009).** *Causality: Models, Reasoning, and Inference* (2nd
   ed.). Cambridge University Press. — `do(...)` semantics.
5. **Shpitser & Pearl (2006).** *Identification of Joint Interventional
   Distributions in Recursive Semi-Markovian Causal Models.* AAAI. —
   identification audit panel reuses this stack.
6. **BIS (2009 / 2018).** *Principles for sound stress testing practices
   and supervision.* — framing for §7 risk metrics.
7. **Chen et al. (2018).** *Neural Ordinary Differential Equations.* —
   for the matrix-exponential discussion in §11.
8. **Markowitz (1952).** *Portfolio Selection.* J. Finance. — baseline
   for the rebalancing QP in §8.

---

<!-- Implementation note: this plan is **not yet implemented**. When the
example lands, mirror the `xva-wwr.md` companion document under
`docs/featured_examples/energy-stress.md` and add a row to the README's
"Featured examples" table linking the two. -->

