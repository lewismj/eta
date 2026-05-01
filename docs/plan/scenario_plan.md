# Featured Example Plan — Structural Stress Test & Rebalancing Under an Energy Shock

[← Back to README](../../README.md) ·
[xVA-WWR featured example](../featured_examples/xva-wwr.md) ·
[Causal plan](./causal_plan.md) ·
[AAD](../guide/reference/aad.md) ·
[Torch](../guide/reference/torch.md)

---

## 1 — Executive Summary

A new featured example, **`examples/energy-stress/`**, that takes a realistic
~$1bn global multi-asset portfolio (US / Europe / UK / Japan equity, energy
equity, oil, a full US Treasury maturity ladder SHV/SHY/IEI/IEF/TLT, and a
multi-currency FX overlay EURUSD/GBPUSD/USDJPY), **learns a structural
causal DAG from ~16 years of real historical daily returns** with a
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
| US Equity | SPY | 25 % | $250 m |
| Europe ex-UK Equity | VGK | 8 % | $80 m |
| UK Equity | EWU | 5 % | $50 m |
| Japan Equity | EWJ | 6 % | $60 m |
| Energy Equity | XLE | 6 % | $60 m |
| Commodities (oil) | USO | 5 % | $50 m |
| FX overlay — EUR/USD | EURUSD | 2 % | $20 m |
| FX overlay — GBP/USD | GBPUSD | 1.5 % | $15 m |
| FX overlay — USD/JPY | USDJPY | 1.5 % | $15 m |
| US T-Bills (≤1Y) | SHV | 4 % | $40 m |
| US Treasuries 1–3Y | SHY | 8 % | $80 m |
| US Treasuries 3–7Y | IEI | 8 % | $80 m |
| US Treasuries 7–10Y | IEF | 12 % | $120 m |
| US Treasuries 20Y+ | TLT | 8 % | $80 m |

Thirteen instruments ($d = 13$). The book has three deliberate structural
"sleeves" the shock must traverse:

1. **Regional equity** (SPY / VGK / EWU / EWJ / XLE) — exposes the
   pass-through from oil to a developed-markets equity basket with
   heterogeneous oil-content (FTSE is energy-major-heavy via BP/Shell, so
   `USO → EWU` is a *positive* edge for index level even as it is negative
   for UK consumers; TOPIX is the opposite — Japan imports ~100 % of its
   crude, so `USO → EWJ` should be a clean negative edge). This
   heterogeneity is the load-bearing reason for splitting Europe into
   `VGK` + `EWU` rather than collapsing them.
2. **Multi-currency FX overlay** — EUR / GBP / JPY against USD, total 5 %
   of NAV. JPY is included specifically because Japan is the cleanest
   "energy-importer FX" channel in DM (oil spike ⇒ ToT shock ⇒ JPY weakens
   ⇒ USDJPY rises), and a structural model that cannot recover this edge
   should not be trusted on the broader thesis. **CHF (USDCHF) is omitted
   from the demo** but listed as a P2 add-on — the safe-haven channel
   would add a fourth FX node (d=14) without changing the qualitative
   conclusions; we prefer the slimmer surface for first ship.
3. **US Treasury maturity ladder** (SHV / SHY / IEI / IEF / TLT) — bond
   bucket is **40 %** of NAV spread across a five-point curve (front, 2Y,
   5Y, 10Y, 30Y) so the shock can express **curve steepening / flattening
   / parallel shifts**, not just a single-duration repricing.

Total NAV $1,000,000,000. Equity 50 % / Commodity 5 % / FX 5 % /
Fixed-income 40 %.

**Pull script:** `scripts/fetch_returns.py` uses `yfinance` to download daily
adjusted close **2010-01-01 → 2026-04-30** (~16 years, ~4,100 trading days)
for the thirteen tickers, computes **log returns**
$r_t = \log(P_t / P_{t-1})$, aligns dates by inner-join, writes
`data/energy-stress-returns.csv` with header
`date,SPY,VGK,EWU,EWJ,XLE,USO,EURUSD,GBPUSD,USDJPY,SHV,SHY,IEI,IEF,TLT`.
FX rates are taken as `EURUSD=X`, `GBPUSD=X`, `JPY=X` (Yahoo's
USD-quoted JPY series).

**Why 2010-01-01, not earlier.** The 2008–2009 GFC window is deliberately
**excluded**, despite being available, for three PM-defensibility reasons
that any senior asset-allocation reviewer will press on:

1. **ETF microstructure breakdown.** Q4-2008 / Q1-2009 saw multi-percent
   intraday NAV dislocations in TLT, IEF, and EWJ; including those days
   produces spurious high-magnitude edges that look impressive on the
   prior-edge audit but are arbitrage-failure artefacts, not structural
   relationships.
2. **Pre-Dodd-Frank / pre-Volcker regime.** Inter-dealer funding, repo
   plumbing, and bank balance-sheet transmission differ *qualitatively*
   before vs after 2010; pooling across this break is a stationarity
   claim no buy-side PM will accept without a structural-break test we
   are not running.
3. **Yahoo data quality.** Pre-2010 daily series for EWJ, EWU, and the
   bond ETFs have more dividend / split adjustment errors and missing
   days; residual diagnostics are noticeably noisier in that window.

The 2010-01-01 → 2026-04-30 window deliberately spans **eight clean
regimes**: 2011 Eurozone crisis, 2013 taper tantrum, 2014–16 oil collapse,
2016 Brexit referendum (a clean GBP idiosyncratic shock, recovered as a
`GBPUSD → EWU` edge in the per-regime audit), 2018 Q4 risk-off, 2020
COVID, 2022 inflation / rate-hike cycle, and the joint 2023 BoJ-YCC +
SVB stress. Critically, the window contains **both the ZIRP era
(2010–2015) and the rate-normalisation era (2022–2026)** in-sample —
which is what enables the §12 per-regime appendix to *empirically
demonstrate* that the bond ↔ equity edge sign flips across the
ZIRP / post-ZIRP boundary, turning the regime-instability objection
from a PM's gotcha into a **headline capability of the framework**.

(A 2008-start variant is shipped as a **commented-out flag** in
`fetch_returns.py` for users who explicitly want the GFC tail; the
default, the committed CSV, and all acceptance criteria are
2010-onwards.)

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
                            (numeric-cols . (1 2 3 4 5 6 7 8 9 10 11 12 13))
                            (date-col . 0))))
   ;; => #<fact-table 14cols × ~4100rows>
   ```
2. **Validate**: `(fact-table-row-count raw)` ≥ 3800; assert no NaNs by
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
     (train   returns-panel-train)    ; fact-table  (T_train × 13)
     (test    returns-panel-test)     ; fact-table  (T_test  × 13)
     (scale   returns-panel-scale)    ; alist of (ticker . (μ . σ))
     (tickers returns-panel-tickers)) ; list of column symbols
   ```

**Why the fact-table and not a list-of-alists / `std.csv`:**

| Concern | List-of-alists | `fact-table` |
|---|---|---|
| Memory for 4 100 × 13 doubles | ~13 cons cells/row × 4 100 ≈ 53 k pairs + symbol keys per cell | 13 contiguous `vector<LispVal>` columns; one allocation per column |
| Column scan (μ, σ, sums) | O(n) with pointer chasing per cell | Cache-friendly column iteration |
| Date / regime lookup | Linear `assoc` per query | O(1) via `fact-table-build-index!` on the date column |
| Group/aggregate (e.g., regime-window slicing in §12) | Hand-rolled `foldl` | `fact-table-group-by`, `fact-table-partition` |
| Tensor handoff to `std.torch` | Convert via nested `map` allocations | Direct column-vector copy into a torch tensor (one strided memcpy per column) |
| GC pressure during MC inner loops | High (cons-heavy) | Low (one heap object) |

The torch tensor handoff in §5 is the load-bearing one: NOTEARS training
runs O(10⁴) inner steps and we want the input batch to come from a single
columnar source, not a re-walked alist tree.

#### Concrete memory footprint

`LispVal` is a NaN-boxed `uint64_t` (`eta/core/.../nanbox.h`), so every
cell is exactly **8 bytes**. The fact-table for this demo is 14 columns
(date + 13 numeric) × ~4,100 live rows:

| Component | Bytes | Notes |
|---|---:|---|
| `columns` data (14 × 4,100 × 8 B) | **~458 KiB** | the float / date payload |
| `std::vector` headers (14 × 24 B) | ~0.3 KiB | three pointers per column vector |
| Vector capacity slack | ≤ 458 KiB | worst-case doubled-capacity; ≈ 0 after a single `shrink_to_fit` |
| `rule_column` (4,100 × 8 B) | ~33 KiB | all `Nil` for ground rows |
| `ground_mask` + `live_mask` (4,100 × 2 B) | ~8 KiB | `uint8_t` bitmaps |
| `col_names` (14 × ~32 B) | ~0.5 KiB | small-string-optimised |
| Date-column hash index (`unordered_multimap`, ~4,100 entries × ~48 B/node) | ~200 KiB | only built if/when `fact-table-build-index!` is called on col 0 |
| **Total, no index** | **~500 KiB (≈ 0.5 MB)** | data-only path |
| **Total, with date index** | **~700 KiB (≈ 0.7 MB)** | enables O(1) date / regime-window lookups |
| **Worst-case (uncompacted vectors + index)** | **≤ 1.2 MB** | bounded by `std::vector` doubling rule |

For comparison, the naive list-of-alists representation would be
~4,100 rows × 13 cells × (cons-cell ≈ 24 B + symbol-key entry ≈ 24 B) ≈
**2.5 MB of pointer overhead alone**, before counting the boxed doubles
themselves — so the fact-table is **~3–5× smaller in resident memory**
and, more importantly, has roughly **two orders of magnitude fewer
GC-traceable objects** (≈ 30 vs ≈ 50,000), which is what actually
matters during the NOTEARS inner loop.

The `(fact-table->tensor ft col-idxs)` bridge from §11 produces a
contiguous float64 tensor of `4,100 × 13 × 8 B ≈ 426 KiB` — same order
of magnitude as the source table; NOTEARS Adam optimiser state for
13 per-node MLPs at hidden=96 (~17 k parameters total) adds another
~270 KB (parameters + 1st & 2nd moments). **Total resident set during
training is comfortably < 5 MB**, well inside L2 on any modern CPU and
trivially shippable in a CI artifact.

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

### Tensor shapes (d = 13)

| Object | Shape | Note |
|---|---|---|
| Input batch | `(B, 13)` | standardised returns |
| Per-node MLP | `Linear(13, 96) → ReLU → Linear(96, 1)` | column $i$ of first layer masked at row $i$; hidden width raised to 96 to absorb the larger fan-in (regional + FX + curve channels) |
| First-layer stack | `(13, 13, 96)` | reduced to `W: (13, 13)` via column 2-norm |
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
             hidden 96 epochs-inner 400
             outer-iters 12 lr 1e-3
             threshold 0.30 seed 20260427))
;; => ((W . #<13x13 tensor>)
;;     (W-thresholded . ((SPY . (XLE USO)) (XLE . (USO))
;;                       (EWJ . (USO USDJPY)) (EWU . (USO GBPUSD)) ...))
;;     (h-final . 4.2e-9)
;;     (loss-trace . (((epoch . 1) (loss . _)) ...))
;;     (per-node-mlps . #<list of 13 nn-modules>))
```

### Validation against domain prior

A small **prior-edge audit** prints which expected directional edges were
recovered. With thirteen nodes spanning regional equity, FX, and a curve
ladder, the prior set grows to **eighteen** edges that any defensible
learned DAG should contain:

**Energy / equity**
1. `USO → XLE`           (oil price → energy equity)
2. `USO → SPY`           (oil pass-through to broad equity, weak but signed)
3. `USO → EURUSD`        (commodity-currency channel; weak)
4. `XLE → SPY`           (energy sector → index)
5. `USO → EWJ` (**negative**) — Japan is the cleanest energy-importer
   equity in DM; oil up ⇒ EWJ down
6. `USO → EWU` (sign-ambiguous; FTSE has heavy oil-major weight via
   BP/Shell so the *index* response is often positive even though the
   consumer impact is negative — the audit reports the sign rather than
   asserting it)

**FX**
7. `EURUSD → VGK`        (USD-EUR translation into European equity)
8. `GBPUSD → EWU`        (USD-GBP translation into UK equity)
9. `USDJPY → EWJ`        (USD-JPY translation; signed: JPY weakening lifts
   USD-denominated EWJ)
10. `USO → USDJPY` (**positive**) — energy-importer terms-of-trade channel
    (oil up ⇒ JPY weakens ⇒ USDJPY rises)

**Curve ladder**
11. `SHV → SHY`          (front-end → 2Y; curve adjacency)
12. `SHY → IEI`          (2Y → 5Y)
13. `IEI → IEF`          (5Y → 10Y)
14. `IEF → TLT`          (10Y → long bond)
15. `SHY → IEF`          (level-factor co-movement across the belly)
16. `SHV → SHY → IEI → IEF → TLT` chain ⇒ a **non-trivial path**, audited
    via `(dag:descendants 'SHV)` ⊇ `{SHY,IEI,IEF,TLT}`.

**Cross-block**
17. `TLT → SPY`          (rates → equity discount-rate channel; signed)
18. `USDJPY → TLT` (**negative**) — JPY weakness historically coincides
    with US long-end repricing higher (BoJ-divergence channel); a signed
    edge whose presence distinguishes a structural model from a
    correlation snapshot.

Pass criterion: **≥ 13 of 18** prior edges present after thresholding
**and** the curve-adjacency chain (11→12→13→14) is recovered intact
(cycle-free, in order) **and** the JPY pair `{USO→USDJPY, USDJPY→EWJ}` is
present with the expected signs. Printed as a green/red panel in
`report.eta`. The chain check and the JPY-pair check are the load-bearing
PM-credibility tests: if the model cannot recover that adjacent maturities
co-move more than distant ones, or that a Japanese energy-importer's FX
weakens under an oil shock, the propagated stress is not trustworthy.

---

## 6 — Structural Shock Propagation

Encode the learned DAG as the alist format used by
[`examples/do-calculus/dag.eta`](../../examples/do-calculus/dag.eta) so the
existing `dag:topo-sort`, `dag:ancestors`, `dag:descendants`,
`dag:satisfies-backdoor?` primitives are reusable verbatim:

```scheme
;; Convert thresholded W into edge list:
(define learned-edges
  '((USO -> XLE) (USO -> SPY) (USO -> EURUSD) (USO -> USDJPY)
    (USO -> EWJ) (USO -> EWU)
    (XLE -> SPY) (EURUSD -> VGK) (GBPUSD -> EWU) (USDJPY -> EWJ)
    (SHV -> SHY) (SHY -> IEI) (IEI -> IEF) (IEF -> TLT) (SHY -> IEF)
    (TLT -> SPY) (USDJPY -> TLT) ...))   ;; produced by (dag:from-W W tickers threshold)
```

`shock.eta` defines a structural shock as a `do`-alist plus a
**propagation strength** $\kappa \in [0,1]$. Because the bond ladder and
multi-currency overlay are now explicit, the shock is expressed across
three coupled blocks rather than three scalars — bear-flattener curve
(front-end up more than long end), broad **USD-up** FX move with the
classic energy-importer asymmetry (JPY weakens more than EUR, GBP
in-between):

```scheme
(define energy-shock
  '((USO     .  0.40)      ; +40% oil (root intervention)
    ;; Multi-currency USD-up: JPY worst-hit (oil-importer ToT), GBP
    ;; mid (oil-major equity offset cushions FX), EUR mildest:
    (EURUSD  . -0.040)     ; EUR weakens 4% vs USD
    (GBPUSD  . -0.055)     ; GBP weakens 5.5% vs USD
    (USDJPY  .  0.080)     ; USDJPY rises 8% (JPY weakens 8%)
    ;; Bear-flattener curve shock as do-interventions on each tenor's
    ;; standardised return; magnitudes are calibrated to a +75bp 2Y /
    ;; +50bp 5Y / +35bp 10Y / +20bp 30Y parallel-plus-twist move and
    ;; converted to ETF-return space via approximate (−duration × Δy):
    (SHV     . -0.0010)    ; ~0.5y duration × 20bp ≈ -10bp return
    (SHY     . -0.0140)    ; ~1.9y × 75bp     ≈ -1.4%
    (IEI     . -0.0220)    ; ~4.4y × 50bp     ≈ -2.2%
    (IEF     . -0.0270)    ; ~7.6y × 35bp     ≈ -2.7%
    (TLT     . -0.0340)))  ; ~17y × 20bp     ≈ -3.4%
```

A **second shock variant** (`bull-steepener`) is shipped alongside as a
sanity-contrast: same oil/FX root, opposite-signed curve. Both run in CI;
the report panel diffs their stressed-ES vectors so reviewers see that the
mechanism is sign-sensitive (i.e. the model is not just amplifying noise).

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

Run `(do:identify-details learned-edges 'XLE 'USO)` and
`(do-rule2-applies? ...)` against the learned edges and print the status
line, mirroring the §6 audit panel of xVA-WWR. Most queries collapse to
`direct` because the structural shock targets a root. Two additional
panels are produced to exercise non-trivial paths:

- `(do:identify-details learned-edges 'IEF 'SHV)` — multi-hop **curve
  path** (`SHV → SHY → IEI → IEF`).
- `(do:identify-details learned-edges 'EWJ 'USO)` — multi-channel
  **FX-mediated path** (direct `USO → EWJ` plus indirect
  `USO → USDJPY → EWJ`); demonstrates that the structural model
  correctly attributes the EWJ stressed loss to *both* the energy and
  the FX channel, which a correlation-only baseline conflates.

---

## 7 — Portfolio P&L & Risk Metrics

`portfolio.eta`:

$$
\mathrm{PnL}_t \;=\; \sum_i w_i \cdot N_{\mathrm{total}} \cdot r_{i,t}
\qquad N_{\mathrm{total}} = 1{,}000{,}000{,}000.
$$

`stress.eta` consumes the MC matrix `R: (n-paths, 13)`:

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
  ;; 13 weight greeks + κ + USO-shock magnitude = 15 partials
  (grad
    (lambda args
      ;; args = (w_1 ... w_13 κ oil-mag)
      (let ((ws (take args 13))
            (k  (list-ref args 13))
            (om (list-ref args 14)))
        (es-of-portfolio R ws k om)))
    (append weights (list kappa (assoc-ref 'USO shock)))))
```

The function returns `(value . #(grad-vector))` in one tape sweep —
identical pattern to `cva-with-greeks` in
[`examples/xva-wwr/xva.eta`](../../examples/xva-wwr/xva.eta).

---

## 8 — Rebalancing Optimisation

**Decision variable:** new weight vector $w' \in \mathbb{R}^{13}$.
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
- **Bond-bucket band:** $0.30 \le \sum_{i \in \text{bonds}} w'_i \le 0.50$
  (mandate-style constraint; the ladder can re-shape internally but the
  total fixed-income allocation is bounded so the optimiser cannot solve
  the problem by trivially dumping all duration)
- **Equity-bucket band:** $0.40 \le \sum_{i \in \text{equity}} w'_i \le 0.65$
  where $\text{equity} = \{\text{SPY, VGK, EWU, EWJ, XLE}\}$
- **FX-overlay band:** $0.02 \le \sum_{i \in \text{FX}} |w'_i| \le 0.10$
  where $\text{FX} = \{\text{EURUSD, GBPUSD, USDJPY}\}$ (FX overlay is
  bounded; the optimiser cannot hedge the entire equity beta via a
  pathological FX position)
- **Per-tenor caps:** $w'_{\text{TLT}} \le 0.15$, $w'_{\text{SHV}} \le 0.10$
  (no single-tenor concentration above 1.5× starting weight)
- **Per-region caps:** $w'_{\text{EWU}} \le 0.10$, $w'_{\text{EWJ}} \le 0.12$
  (mandate diversification across DM regions)
- $\hat\mu^{\top} w' \ge \mu_{\text{target}}$  (optional return floor)
- $\tau_i = \tau_i^+ - \tau_i^-, \; \tau_i^\pm \ge 0$

$\hat\Sigma_{\text{stressed}}$ is the **sample covariance of the MC stressed
return matrix** from §7 — NOT a historical covariance. This is the load-bearing
choice that makes the rebalance *causal*: the optimiser sees the structural
shock's covariance, not unconditional history.

**Solver:** reuse `clp:rq-minimize` from `std.clpr`, exactly as
[`examples/xva-wwr/compress.eta`](../../examples/xva-wwr/compress.eta) does
for the SIMM-Σ QP. Output (illustrative; thirteen weights, ordered
`SPY VGK EWU EWJ XLE USO EURUSD GBPUSD USDJPY SHV SHY IEI IEF TLT` —
note the FX overlay weights here are *signed* relative to a 0-baseline
overlay, so the magnitudes appear small):

```scheme
((tickers          . (SPY VGK EWU EWJ XLE USO
                      EURUSD GBPUSD USDJPY
                      SHV SHY IEI IEF TLT))
 (old-weights      . (0.25 0.08 0.05 0.06 0.06 0.05
                      0.02 0.015 0.015
                      0.04 0.08 0.08 0.12 0.08))
 (new-weights      . (0.20 0.07 0.04 0.03 0.02 0.02   ; cut EWJ + XLE + USO hardest
                      0.025 0.02 0.030               ; lean *into* USDJPY (long USD vs JPY)
                      0.05 0.10 0.11 0.14 0.10))     ; bond ladder up
 (turnover         . 0.198)
 (bond-bucket-old  . 0.40)
 (bond-bucket-new  . 0.50)
 (equity-bucket-old . 0.50)
 (equity-bucket-new . 0.36)         ; clipped to 0.40 by the band; reported pre-clip
 (fx-overlay-old   . 0.050)
 (fx-overlay-new   . 0.075)
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
| **Overfitting the DAG** to ~16 years × 13 assets | Med | Spurious edges; misleading propagation | L1 + L2 reg; 18-edge prior audit incl. curve-adjacency chain *and* JPY-pair signed check; 80/20 train/test; report test-set reconstruction MSE per node; cross-validate `λ1` over 3 grid points; **stability-selection** — refit on 5 bootstrap resamples, keep edges present in ≥ 4. |
| **Look-ahead bias** | Med | Inflated apparent quality | Time-ordered split; **never** standardise using test-set stats; document explicitly. |
| **Non-stationarity** of returns (regime shifts: 2011 Eurozone, 2013 taper, 2014–16 oil collapse, 2016 Brexit, 2020 COVID, 2022 rates / **ZIRP→normalisation break**, 2023 BoJ-YCC, 2023 SVB) | High | Learned DAG mixes regimes; in particular the bond↔equity edge sign is *known* to flip across the ZIRP / post-ZIRP boundary | Mandatory **per-regime DAG appendix**: refit on the four windows `{2010–2013 (post-GFC reflation), 2014–2019 (mid-cycle ZIRP), 2020–2021 (COVID + final ZIRP), 2022–2026 (rate normalisation)}` and report which edges are stable across all four windows vs window-specific. PM-credibility thresholds: the curve-adjacency chain (SHV→SHY→IEI→IEF→TLT) and the JPY pair (USO→USDJPY, USDJPY→EWJ) must be present in **all** windows; `USO→XLE` in ≥ 3 of 4. **Headline finding** that the demo deliberately surfaces: the `TLT → SPY` edge sign **flips** between the ZIRP windows (negative — bonds hedge equities) and the 2022–2026 window (positive — bonds and equities sell off together). This is the empirical motivation for the structural rebalance vs a naive Markowitz one. |
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
2. **DAG quality.** Prior-edge audit recovers ≥ 13 of 18 expected
   directional edges **and** the curve-adjacency chain
   `SHV→SHY→IEI→IEF→TLT` is intact **and** the JPY pair
   `{USO→USDJPY (positive), USDJPY→EWJ (positive)}` is recovered with the
   expected signs; reported test-set reconstruction $R^2 \ge 0.20$ on
   stressed target columns (XLE, USO, EWJ, IEF, TLT).
2a. **Stability selection.** ≥ 80 % of the recovered edges (and **100 %** of
    the curve-adjacency chain *and* the JPY pair) reappear in ≥ 4 of 5
    bootstrap refits.
2b. **Cross-regime stability.** The curve-adjacency chain *and* the JPY
    pair are recovered in **all four** regime windows of §12; `USO→XLE`
    recovered in ≥ 3 of 4. The Brexit-window `{GBPUSD→EWU}` edge must be
    present in the 2014–2019 window (sanity: the 2016 referendum shock is
    in-sample there). The `TLT → SPY` edge sign is reported per window
    and **must flip** between the ZIRP windows (2010–2013, 2014–2019,
    2020–2021) and the 2022–2026 normalisation window — this is a
    positive criterion: failure to detect the flip means the model is
    over-smoothing across the regime break and is not fit for use.
3. **Acyclicity.** $h(W_{\text{final}}) < 10^{-6}$ after thresholding;
   topological sort succeeds without cycle break.
4. **AAD vs finite-difference.**
   $\|\nabla_{w}\mathrm{ES}_{\text{AAD}} - \nabla_{w}\mathrm{ES}_{\text{FD}}\|_\infty < 10^{-3}$
   at $h=10^{-4}$ on a fixed-seed MC.
5. **Rebalance convexity.** $\hat\Sigma_{\text{stressed}} \succeq 0$
   (diagnostic before solve) and `clp:rq-minimize` returns a finite
   optimum; the new ES is **strictly less** than the old ES.
6. **Structural vs correlation baseline.** Side-by-side panel shows the
   structural shock concentrates loss on XLE / USO / **EWJ** (Japan
   energy-importer channel) **and** re-shapes both the bond ladder *and*
   the FX overlay (the correlation-only baseline cannot distinguish
   front-end from long-end repricing, nor JPY-specific weakness from a
   broad USD move); the structural rebalance differs from the correlation
   rebalance by **at least one ladder tenor weight ≥ 3pp** **and** at
   least one FX-overlay weight differs by ≥ 1pp, and **at least one greek
   differs in sign or > 30 % in magnitude.**
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

