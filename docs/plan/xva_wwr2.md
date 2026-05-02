# XVA Wrong-Way-Risk Notebook v2 — Streamlined Plan

[Back to plans index](./README.md) - supersedes the ad-hoc layout in
[`examples/xva-wwr/`](../../examples/xva-wwr) and the long-form
[`docs/featured_examples/xva-wwr.md`](../featured_examples/xva-wwr.md).

---

## 1. Scope and Goals

Deliver **one** xeus-eta Jupyter notebook,
[`examples/notebooks/xva-wwr.ipynb`](../../examples/notebooks/xva-wwr.ipynb),
that reproduces — and meaningfully upgrades — the existing xVA / WWR
demo as an interactive narrative. The notebook must be runnable
top-to-bottom on a release build with `-DETA_BUILD_TORCH=ON`, finish
in under five minutes on commodity hardware, and render every result
(tables, exposure curves, elasticity heatmap, hedge witness) inline
via `std.jupyter`.

### Goals

1. A realistic **~5 000-trade** synthetic portfolio across IRS, FX
   forwards, FX options, equity TRS spread over 20–50 counterparties.
2. **Multiple netting sets per counterparty**, mixing CSA and
   non-CSA agreements with full term realism (threshold, MTA, IA,
   rounding, eligible currency, margin frequency, MPoR).
3. CVA, DVA, FVA on the **collateralised** EPE/ENE; KVA and MVA
   sketched as a stretch cell.
4. **Hazard term structures** bootstrapped from per-CP CDS spreads
   rather than flat hazard-rate literals.
5. **General WWR** via Gaussian-copula correlation between exposure
   drivers and hazard innovations, plus **specific WWR** on equity TRS
   whose underlying is the counterparty's own equity.
6. Declarative authoring via `define-record-type` schemas and 4–5
   `define-syntax` macros (`define-portfolio`, `define-csa`,
   `with-market-scenario`, `simulate-paths`, `report-xva`).
7. Causal `do(...)` and elasticity ranking retained from v1, but
   demoted to a single section so the notebook's headline is
   end-to-end XVA — not the SCM showcase.

### Non-Goals

- Real CDS / market data ingest (synthetic but plausible curves only).
- Production-grade SIMM, BA-CVA, SA-CVA capital models.
- Distributed actor sweeps; the notebook stays single-process.
- Rewriting the existing `examples/xva-wwr/*.eta` modules — the
  notebook imports them where useful and replaces the rest with
  declarative cells.
- Real-time Greek hedging dashboard.

---

## 2. Notebook Outline (Cell-by-Cell)

13 sections, ~55 cells. Each row lists the cell purpose and its
expected rendered output. `MD` = markdown cell, `EC` = Eta code cell.

| # | Type | Purpose | Rendered output |
|---|---|---|---|
| 0.1 | MD | Title, TL;DR, dependency banner (`-DETA_BUILD_TORCH=ON`, seed) | — |
| 0.2 | EC | `(import std.prelude std.torch std.jupyter)`; seed; `(manual-seed 20260427)` | seed echo |
| 1.1 | MD | §1 *Data Model* — record schemas | — |
| 1.2 | EC | `define-record-type` for `trade`, `netting-set`, `csa`, `counterparty`, `market-state`, `simulation-config`, `exposure-profile`, `xva-result` | `<trade>` constructor demo |
| 2.1 | MD | §2 *Portfolio Construction* (5 000 trades, breakdown table) | — |
| 2.2 | EC | `(define-portfolio book ...)` macro expanding to a `FactTable` | `jupyter:table` of first 25 rows |
| 2.3 | EC | Per-product summary `(fact-table-group-count book 'product)` | `jupyter:vega` bar chart of trade counts |
| 3.1 | MD | §3 *Counterparties & Hazards* (rating, sector, region, CDS) | — |
| 3.2 | EC | Build 30 counterparties via `define-counterparties`; assign synthetic CDS quotes per tenor | counterparty `jupyter:table` |
| 3.3 | EC | `bootstrap-hazard-curve` per CP (piecewise-flat from CDS); plot one curve | Vega step plot |
| 4.1 | MD | §4 *Netting Sets & CSAs* (CSA vs non-CSA, threshold/MTA/IA) | — |
| 4.2 | EC | `(define-csa ...)` macro; assemble netting sets (1–3 per CP) | netting-set `jupyter:table` |
| 5.1 | MD | §5 *Market State & Risk Factors* (IR, FX, equity, vol, correlation block) | — |
| 5.2 | EC | Build factor-correlation matrix incl. **WWR block**: ρ(exposure-driver, CP-hazard) | Vega heatmap |
| 6.1 | MD | §6 *Path Simulation* — Gaussian copula, antithetic sampling, MPoR offset | — |
| 6.2 | EC | `(simulate-paths config ...)` macro: 5 000 paths × 60 quarterly buckets, returns `exposure-profile` per netting set | tape stats |
| 6.3 | EC | EPE / ENE curves for one collateralised + one uncollateralised set | Vega line plot (EE, ENE, collateralised EE) |
| 7.1 | MD | §7 *xVA Aggregation* (CVA, DVA, FVA formulas; collateral haircut) | — |
| 7.2 | EC | `compute-cva`, `compute-dva`, `compute-fva` over the book; `report-xva` macro | `jupyter:table` per-CP totals |
| 7.3 | EC | KVA / MVA stretch (one-cell sketch with a TODO comment) | scalar table |
| 8.1 | MD | §8 *AAD Greeks* — full risk gradient in one backward pass | — |
| 8.2 | EC | `(grad ...)` over the entire xVA closure across (curves, hazards, recoveries, FX, vols) | `jupyter:table` of sensitivities |
| 8.3 | EC | Finite-difference cross-check on three Greeks | parity table |
| 9.1 | MD | §9 *Wrong-Way Risk* — general (correlation) vs specific (TRS-on-own-equity) | — |
| 9.2 | EC | Re-run §6/§7 with WWR copula block enabled; show CVA delta vs WWR-off | side-by-side Vega bar |
| 9.3 | EC | Specific WWR on equity TRS — exposure conditional on default | Vega plot |
| 10.1 | MD | §10 *Causal `do(...)` Sweep* (carried over from v1, condensed) | — |
| 10.2 | EC | `(with-market-scenario (do oil -0.30) ...)` then `(do oil +0.30)` | scenario `jupyter:table` |
| 10.3 | EC | Per-CP elasticity ranking, top wrong-way names | Vega heatmap, top-3 callout |
| 11.1 | MD | §11 *Hedge Suggestion* (compress.eta QP) | — |
| 11.2 | EC | `solve-compression-qp` on the dominant CP's IR-delta bucket | δ\* witness `jupyter:table` |
| 12.1 | MD | §12 *Reproducibility & Determinism* | — |
| 12.2 | EC | `rerun-shard-and-check`; assert hash match | green/red badge |
| 13.1 | MD | §13 *Summary & Next Steps* (links to causal plan M12–M15) | — |

---

## 3. Data Model & Records

All schemas are `define-record-type` (already used by
[`stats.eta`](../../stdlib/std/stats.eta) and
[`market.eta`](../../examples/xva-wwr/market.eta)). FactTable rows are
keyed by `id`; collections of records are stored as alists or
`FactTable`s where group-by/index access is needed.

```scheme
(define-record-type <trade>
  (make-trade id cp-id netting-set-id product notional ccy
              maturity-years fixed-rate floating-index strike
              vol underlying-id)
  trade?
  (id trade-id) (cp-id trade-cp) (netting-set-id trade-ns)
  (product trade-product)        ; 'irs | 'fx-fwd | 'fx-opt | 'eq-trs
  (notional trade-notional)
  (ccy trade-ccy)
  (maturity-years trade-maturity)
  (fixed-rate trade-fixed-rate)
  (floating-index trade-float-idx)
  (strike trade-strike)
  (vol trade-vol)
  (underlying-id trade-underlying))   ; equity ticker for TRS / FX pair

(define-record-type <counterparty>
  (make-counterparty id name rating sector region cds-curve recovery)
  counterparty?
  (id cp-id) (name cp-name) (rating cp-rating)
  (sector cp-sector) (region cp-region)
  (cds-curve cp-cds-curve)            ; alist tenor->spread
  (recovery cp-recovery))

(define-record-type <csa>
  (make-csa id cp-id netting-set-id threshold mta ia rounding
            eligible-ccy margin-freq mpor-days)
  csa? ...)

(define-record-type <netting-set>
  (make-netting-set id cp-id csa-id trade-ids) netting-set? ...)

(define-record-type <market-state>
  (make-market-state ois-curves fx-spots equity-spots vols
                     factor-corr wwr-corr)
  market-state? ...)

(define-record-type <simulation-config>
  (make-simulation-config seed n-paths n-steps dt
                          antithetic? wwr-on?)
  simulation-config? ...)

(define-record-type <exposure-profile>
  (make-exposure-profile netting-set-id ee ene collateralised-ee
                         times)
  exposure-profile? ...)

(define-record-type <xva-result>
  (make-xva-result cp-id cva dva fva kva mva greeks)
  xva-result? ...)
```

`<trade>` and `<csa>` instances are accumulated into `FactTable`s so
the per-product breakdown (§2.3), per-CP CVA roll-up (§7.2) and
top-N elasticity ranking (§10.3) all use a uniform query surface.

### Portfolio Breakdown (target shape)

| Product | # Trades | Avg notional | Maturity range |
|---|---:|---:|---|
| IRS (fixed-vs-float) | 2 800 | 1.5M USD | 1Y–10Y |
| FX forward | 1 100 | 0.8M USD | 1M–2Y |
| FX option (vanilla) | 600 | 0.5M USD | 3M–3Y |
| Equity TRS | 500 | 1.0M USD | 6M–5Y |
| **Total** | **5 000** | — | — |

---

## 4. Stdlib Usage Map

Every notebook stage is anchored to concrete stdlib modules already
present in [`stdlib/std/`](../../stdlib/std).

| Stage | Stdlib module(s) | Key functions |
|---|---|---|
| Records & alists | `std.core`, `std.collections` | `define-record-type`, `assoc-ref`, `map*`, `foldl`, `filter`, `sort`, `take` |
| Portfolio table | `std.fact_table` | `make-fact-table`, `fact-table-insert!`, `fact-table-group-count`, `fact-table-group-by`, `fact-table-filter` |
| CSV ingest (stretch) | `std.csv` (uses `std.fs`) | `csv:read-file`, `csv:rows->fact-table` |
| Hazard bootstrap | `std.math`, `std.stats` | `exp`, `log`, `stats:ols` (sanity slope on bootstrapped curve) |
| Random shocks | `std.torch` | `manual-seed`, `randn`, `mvnormal`, `cholesky`, `to-list` |
| Path simulation | `std.torch`, `std.collections` | `t+`, `t*`, `matmul`, `tsum`, `mean`, antithetic via `neg` |
| AAD greeks | `std.aad` (auto-import via prelude) | `grad`, `with-checkpoint`, `check-grad-report` (FD cross-check) |
| Causal do(...) sweep | `std.causal`, `std.causal.identify` | `dag:mutilate-do`, `do-rule1-applies?`, `do:identify`, `id` |
| Hedge QP | `std.clp` (and `std.logic`) | `clp:domain`, `clp:+`, `clp:<=`, `clp:solve` (real domain wrapper from `compress.eta`) |
| Statistics | `std.stats` | `stats:mean`, `stats:variance`, `stats:percentile`, `stats:cor-matrix` |
| Time / determinism | `std.time` | `time:monotonic-ms`, `time:elapsed-ms` |
| Logging | `std.log` | `log:info`, `log:warn` for each section banner |
| Display | `std.jupyter` | `jupyter:table`, `jupyter:vega`, `jupyter:dag`, `jupyter:markdown`, `jupyter:html` |
| Tests (stretch) | `std.test` | `test`, `assert-near` for in-notebook regression assertions |

### Gaps that need a small notebook-local helper

| Gap | Helper to write in-notebook (~20 LoC each) |
|---|---|
| Piecewise-flat **hazard curve bootstrap** from CDS quotes | `bootstrap-hazard-curve : alist tenor->spread → alist tenor->λ` |
| **Antithetic** Gaussian copula sampler (combine `randn` + `cholesky`) | `(draw-correlated-shocks corr n-paths n-steps)` |
| **Collateralised EPE** under MPoR + threshold + MTA + rounding | `(collateralise ee csa)` |
| **CSA-aware netting** rollup at netting-set level | `(net-trades ns trade-pvs)` |
| Vega-Lite spec builder for the elasticity heatmap | `(heatmap rows cols values)` |
| In-notebook macro hygiene shim if `define-syntax` proves too thin | fall back to `defmacro` style helpers wrapping the record constructors |

None of the gaps justify a new stdlib module; all should live in the
notebook's preamble cell or in a thin `examples/notebooks/xva_wwr_helpers.eta`
module imported at cell 0.2.

---

## 5. Macros / DSL Design

Five `define-syntax` macros keep the body declarative. Sketches show
the source form and its expansion target.

### 5.1 `define-portfolio`

```scheme
(define-portfolio book
  (irs    2800 :tenor (1 . 10) :notional (500e3 . 5e6))
  (fx-fwd 1100 :tenor (0.08 . 2) :notional (250e3 . 2e6))
  (fx-opt  600 :tenor (0.25 . 3) :notional (250e3 . 1e6) :vol (0.10 . 0.30))
  (eq-trs  500 :tenor (0.5 . 5) :notional (500e3 . 2e6)
                :underlying-of-cp #t))         ; specific WWR hook
```

→ expands to a `let*` that builds a `FactTable`, deterministically
seeded, calling `make-trade` for each row, with `:underlying-of-cp #t`
pinning the TRS underlying to the trade's counterparty equity ticker.

### 5.2 `define-csa`

```scheme
(define-csa csa-jp-morgan
  :cp        'CP-07
  :threshold 250000  :mta 50000  :ia 0
  :rounding  10000   :ccy 'USD
  :margin    'daily  :mpor-days 10)
```

→ `(define csa-jp-morgan (make-csa ...))` plus auto-registration in a
notebook-level `*csa-registry*` alist.

### 5.3 `with-market-scenario`

```scheme
(with-market-scenario ((do oil -0.30) (do usd-10y +0.01))
  (cva-pipeline book counterparties market sim-config))
```

→ rebinds `market-state` via `adjust-hazards` + `simulate-paths-tilted`
inside the dynamic extent of the body. Pure function — no
side-effects on the global market record.

### 5.4 `simulate-paths`

```scheme
(simulate-paths
  :config        sim-config
  :market        market
  :netting-sets  netting-sets
  :antithetic?   #t
  :wwr?          #t)
```

→ expands to a `let*` that calls the helper sampler, runs torch
ops, returns a list of `<exposure-profile>` records — one per
netting set.

### 5.5 `report-xva`

```scheme
(report-xva results
  :group-by 'cp
  :columns  '(cva dva fva)
  :format   'table
  :plot     'vega-bar)
```

→ folds `<xva-result>` records into a `FactTable`, hands it to
`jupyter:table` and (if `:plot` is set) emits a Vega-Lite spec via
`jupyter:vega`. Returns the `FactTable` so the cell value is still
inspectable.

---

## 6. Causal Integration — M13/M15 Discussion

The original `causal_plan.md` numbers stop at **M11**. The block
labelled M12–M15 is an *extension* covering CATE meta-learners (M12),
trees / Causal Forest (M13), Cross-fit / DML (M14), and Qini /
policy value (M15). The plan below addresses each by content, not by
number.

| Extension milestone | What it delivers | Use in this notebook | Verdict |
|---|---|---|---|
| **M12 — CATE meta-learners** (S/T/X/R/DR-learner) | Heterogeneous treatment effects per-row | Per-CP heterogeneous WWR sensitivity (replace the linear hazard-tilt β with τ̂(z) from a DR-learner) | **Optional stretch** — a nice §10 sidebar but not required for the headline. |
| **M13 — CART / Random Forest / Causal Forest** | Tree-based nuisance models and α-weighted local AIPW | Forest-based ε(CP-i) under `do(oil)`; better than the linear `nn.Linear(3, 30)` in v1 when the panel is non-linear | **Optional stretch** — only valuable once we move past synthetic linear-Gaussian DGPs. |
| **M14 — Cross-fitting / DML** | Asymptotically valid CIs around AIPW / R-learner targets | Confidence intervals around the headline elasticity ε(CP-17) | **Recommended optional** — the most useful of the four because it gives the desk a defensible CI on the WWR ranking, but still not load-bearing for the v2 notebook. |
| **M15 — Uplift / Qini / policy value** | Off-policy evaluation of treatment policies | Not a natural fit — XVA hedging is a constrained QP, not an uplift policy | **Out of scope.** |

**Recommendation.** Treat all four as **out-of-scope for v2** to keep
the notebook's scope tight; reference them in §13 as "Next Steps —
when M14 lands, swap the §10.3 elasticity ranking for a DML PLR
estimate with bootstrap CIs (≈ 30 lines of cell code)." This keeps the
notebook deliverable independent of the causal stack's roadmap.

The notebook already uses M0–M9 functionality (identification, ID/IDC,
adjustment, AIPW where useful) via `std.causal` and
`std.causal.identify`; nothing in v2 requires a not-yet-shipped
milestone.

---

## 7. Implementation Plan

Six PR-sized increments. Each ends with the notebook in a runnable
state — no half-built cells across PRs.

| # | Slice | Cells touched | Stdlib touch |
|---|---|---|---|
| 1 | Records, helpers module, cell 0.2 + §1 | 0.x, 1.x | none |
| 2 | `define-portfolio` macro + §2 | 2.x | none (helpers in `xva_wwr_helpers.eta`) |
| 3 | Counterparties, CDS bootstrap, §3 + §4 + `define-csa` | 3.x, 4.x | none |
| 4 | Market state, copula sampler, `simulate-paths` macro, §5 + §6 | 5.x, 6.x | none |
| 5 | xVA aggregation + AAD + §7 + §8 + `report-xva` | 7.x, 8.x | none |
| 6 | WWR + causal sweep + hedge + reproducibility (§9–§13) | 9.x–13.x | none |

If the notebook surfaces a missing primitive (e.g. real Vega heatmap
helper, FactTable groupby-mean), file the gap as a separate stdlib
ticket — do **not** inline-patch stdlib in this PR series.

---

## 8. Testing

In-notebook assertions (cells flagged `;; TEST`) plus one CI script.

1. **Closed-form bucket parity** — pick one IRS, one FX option, one
   TRS; compute one bucket by hand (in markdown) and `assert-near`
   the simulated value within 1 %.
2. **AAD vs FD parity** — `check-grad-report` on three of the
   sensitivities returned in §8.2; tolerance 1 e−4.
3. **WWR sign check** — turning the WWR copula block on must raise
   total CVA monotonically vs WWR-off; assertion in §9.2.
4. **Determinism** — §12 hash compare across two re-runs of one
   shard; `assert (= h1 h2)`.
5. **Portfolio shape** — `(fact-table-row-count book)` must equal
   5 000; per-product counts match the breakdown table.
6. **Causal identifiability** — `do:identify` on the §10 SCM returns
   `direct` / `ident`; assertion in §10.2.
7. **CI script** `scripts/check_xva_wwr_notebook.py` runs the notebook
   headless via `jupyter nbconvert --execute`, fails if any cell
   raises or any `;; TEST` line prints `FAIL`.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| 5 000 trades × 5 000 paths × 60 buckets × backward AAD blows the tape | Bucket the book into ~50 chunks and call `with-checkpoint` at chunk boundaries; AAD-friendly because partial sums concatenate linearly. |
| `define-syntax` macro hygiene gaps (only one example in stdlib today) | Keep macros thin; fall back to plain function constructors if expansion misbehaves; cover each macro with one expansion-only test. |
| Vega-Lite specs are verbose to hand-write | Encapsulate in 4 small helpers (`bar`, `line`, `step`, `heatmap`) in the helpers module. |
| Notebook execution time exceeds 5 min | Reduce default `n-paths` to 1 000 in the published notebook; document the 5 000-path "production" knob. |
| Specific WWR (TRS-on-own-equity) requires per-trade equity-default jump model | Use a simple deterministic jump-on-default multiplier (×3) and document it as illustrative — full jump-to-default is out of scope. |
| Bootstrap of CDS curve underspecified | Use a piecewise-constant hazard between CDS pillars with the standard `λ_i = s_i / (1 − R)` first-order approx; flag as approximate. |
| `xeus-eta` MIME-rendering surface evolves | Fall back to ASCII-table prints if a `jupyter:*` mime fails; assertions stay valid. |

---

## 10. Effort Estimate

| Slice | Effort |
|---|---|
| Helpers module (`xva_wwr_helpers.eta`) | 0.5 d |
| Records + macros | 0.5 d |
| Portfolio + CSA + counterparties cells | 1.0 d |
| Hazard bootstrap + market state + sampler | 1.5 d |
| xVA aggregation (CVA / DVA / FVA / KVA stretch) | 1.5 d |
| AAD greeks + FD parity | 0.5 d |
| WWR (general + specific) | 1.0 d |
| Causal sweep + hedge QP wiring (re-use existing modules) | 0.5 d |
| Reproducibility, narrative polish, plot styling | 1.0 d |
| Headless CI runner + assertions | 0.5 d |
| **Total** | **≈ 8.5 engineering days** |

---

## 11. Delivery Order

1. **PR 1** — `examples/notebooks/xva_wwr_helpers.eta` + cells 0.x, 1.x.
2. **PR 2** — `define-portfolio`, `define-csa`, cells 2.x–4.x.
3. **PR 3** — Market state, sampler, `simulate-paths` macro, cells 5.x–6.x.
4. **PR 4** — xVA aggregation + AAD + `report-xva`, cells 7.x–8.x.
5. **PR 5** — WWR (general + specific) + causal sweep + hedge, cells 9.x–11.x.
6. **PR 6** — Reproducibility, summary, headless CI, doc cross-links.

---

## 12. Open Questions

1. **CSA modelling depth** — full path-dependent collateral with MPoR
   spike, or first-order haircut on EPE? *Proposal:* path-dependent
   for one set (illustrative), first-order for the rest (speed).
2. **CDS bootstrap** — pin λ piecewise-constant between pillars or fit
   a Nelson-Siegel hazard? *Proposal:* piecewise-constant; NS later.
3. **Specific WWR scope** — only equity TRS, or also FX options on
   sovereign-issued currencies? *Proposal:* equity TRS in v2; FX as a
   sidebar in §9.
4. **Plotting backend** — commit to Vega-Lite via `jupyter:vega`, or
   add a thin matplotlib-via-process call? *Proposal:* Vega-Lite only.
5. **Notebook seed contract** — single global seed, or one per stage?
   *Proposal:* single global, derived per-stage by `(+ seed offset)`.
6. **Macro hygiene** — should `define-portfolio` be a `define-syntax`
   (preferred) or a function returning a `FactTable`? *Proposal:*
   `define-syntax` for the declarative feel, with a function fallback
   if hygiene proves brittle.

---

## Appendix — What Changed vs the Existing v1 Material

There is no `xva_wwr.md` plan on disk; the comparison baseline is
`docs/featured_examples/xva-wwr.md` plus the working code in
`examples/xva-wwr/`.

| Area | v1 (featured example + code) | v2 (this plan) |
|---|---|---|
| Deliverable | Long-form prose doc + standalone `.eta` orchestrator | One xeus-eta notebook (`examples/notebooks/xva-wwr.ipynb`) |
| Trade count | 200, deterministic closed-form | 5 000, declarative DSL across 4 product types |
| Counterparties | 30, no rating / sector / region | 20–50, with rating, sector, region, CDS curve |
| Netting sets | implicit (1-per-CP) | explicit, multiple per CP, mixed CSA / non-CSA |
| CSA terms | threshold / MTA / MPoR only | threshold, MTA, IA, rounding, ccy, margin freq, MPoR |
| Hazards | flat `λ_i = base-hazard(i)` literal | bootstrapped from per-CP CDS spreads |
| WWR | causal `do(...)` only | general (copula) + specific (TRS-on-own-equity) + causal sweep |
| xVA stack | CVA only | CVA / DVA / FVA, KVA / MVA stretch |
| Records | one (`<cp-betas>`) | eight (`<trade>`, `<csa>`, `<netting-set>`, `<counterparty>`, `<market-state>`, `<simulation-config>`, `<exposure-profile>`, `<xva-result>`) |
| DSL | none — alists everywhere | five `define-syntax` macros (`define-portfolio`, `define-csa`, `with-market-scenario`, `simulate-paths`, `report-xva`) |
| ML calibration of β | `nn.Linear(3, 30)` recoverability check (§3a) | dropped from headline; available as a sidebar — focus shifts to the WWR copula |
| Causal coverage | ~9 sections of SCM/causal narrative | one §10 — kept tight, deferred CATE/DML to "Next Steps" |
| Plotting | none (text reports) | Vega-Lite via `std.jupyter` (bar, line, step, heatmap) |
| Reproducibility | shard-replay hash | retained, reduced to one cell |
| Doc length | ~1 700 lines of markdown | one notebook, ~55 cells |

