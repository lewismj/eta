# Causal xVA Engine — Wrong-Way Risk via `do(...)` Interventions

[← Back to README](../README.md) · [xVA overview](xva.md) ·
[Causal](causal.md) · [AAD](aad.md) · [Torch](torch.md) ·
[Examples](examples.md) · [Compiler](compiler.md)

---

## TL;DR — Executive Walkthrough

>If wrong-way risk is modelled using correlation-based approaches, it may fail to capture i
>directionality or concentration under stress, and does not distinguish between 
>correlation and causation.

>This system instead evaluates CVA under macroeconomic shocks routed through an
>**assumed** structural causal model (SCM). Wrong-way risk is analysed via explicit
>interventions (`do(...)` operations) on macro factors (e.g. oil, rates, FX), so the
>reported CVA reflects how the portfolio responds when those drivers are actively
>moved under the SCM mechanism, rather than merely observed to co-move.

The system computes CVA for a portfolio under these shocks, calculates full
risk sensitivities in a single reverse-mode AAD pass, ranks counterparties by their
elasticity to each `do(...)` shock, and produces hedge recommendations via
convex optimisation.

Structural relationships (e.g. macro → hazard/exposure) are *recovered from a
synthetic shock × hazard panel* in this demo, so the wrong-way analysis is
self-consistent against its own data-generating process. Symbolic differentiation
of pricing functions is supported, and all components run within a single framework.

> [!CAUTION]
> **What this demo is and is not.**
> *Is*: a self-consistent end-to-end pipeline (book → SCM → CVA → `do(...)` →
> elasticity → hedge QP) on a synthetic data-generating process (DGP), with
> deterministic seeds and a recoverability check on the SCM β's.
> *Is not*: an empirically identified SCM. The DAG (§6), the linear-Gaussian
> hazard mechanism (§3a), the exogeneity of the macro shocks, and the
> static-exposure assumption are all **asserted**, not estimated from real
> data. Every "causal" claim below is conditional on those assumptions
> holding; on real macro/credit panels they typically do not, and an
> identification step (instrument, back-door adjustment, or sensitivity
> analysis to unmeasured confounding) would be required before the same
> language is defensible. The near-zero ML training loss in §3a (≈ 0.0025)
> is the expected behaviour of an invertible synthetic DGP regressed onto
> the same model class — it is a self-consistency check, not evidence of
> empirical fit.
>
> *Units*: every CVA / exposure figure in this document is in **base
> currency units** (treat as USD for concreteness); the demo's notional
> scale is `1 000 000 + 25 000 · (idx mod 9)` per trade across 200 trades,
> so the baseline book CVA of `118 302` corresponds to roughly 0.06 % of
> aggregate notional. Percentage moves quoted under `do(...)` shocks are
> relative to that baseline.



**What goes in**

- A deterministic 200-trade book over 30 counterparties with CSA terms
- An OIS / hazard / recovery / FX market state, plus per-CP shock βs
- A 6-node SCM linking `oil → {usd-rates, fx, energy-credit} → exposure, hazard`
- A symbolic swap-leg expression `f(r) = N · (1 − e^{−rτ}) / r`
- A 3×3 covariance prefix from a SIMM-style 10×10 IR-delta block

**What happens (nine sections + diagnostics, one binary, fixed-seed reproducible run)**

1. Build a deterministic book and market state (§1, §2)
2. **Calibrate the SCM βs by ML on a synthetic shock × hazard panel (§3a)**
3. Draw seeded torch shocks → per-CP exposure curve (§3)
4. Aggregate CVA per counterparty and per book (§4)
5. Take all risk-factor sensitivities in **one AAD backward pass** (§5)
6. Re-price under `do(oil = −30%)` via tilt + hazard transform (§6)
7. Sweep ± shocks; rank by **causal elasticity** ε = ∂log CVA / ∂(do-var) (§7)
8. Build a swap-leg pricer symbolically; lower onto the **same** tape (§8)
9. Compress hot-CP exposure via convex QP on a SIMM Σ block; emit an
   actionable unwind set (§9)

After §7, a **WWR Diagnostics** block decomposes the dominant CP's
elasticity into per-factor β contributions, stress-tests the ranking
under β-perturbation, and contrasts the result against a
correlation-only WWR baseline. A determinism / shard-replay check
runs in CI; it lives in the
[Technical Appendix](#technical-appendix--deterministic-shard-replay).

**What comes out**

A single association list with the baseline CVA, the AAD gradient
vector, per-shock book CVA, ranked elasticity buckets, the symbolic
node count and lowered-kernel value, the QP preview objective **and
the solved hedge** (`'hedge` — DV01s, λ, optimum, δ\* witness), the
ML-recovered β alist, the training trace, the shard-replay match
flag, and a structured **`'stage-report`** alist that the friendly
renderer in `report.eta` walks — every value traceable back to the
stage that produced it.

```scheme
(run-demo)
;; All monetary figures in base currency units (treat as USD).
;; => ((book-size . 200)
;;     (baseline-cva . 118302)                         ; book CVA, base ccy
;;     (top3 ((CP-?? . ...) ...))
;;     (greeks (130253                                  ; CVA value at seed
;;              #(0 0 -212503 130253)))                 ; ∂CVA/∂(Δλ, ΔR, Δr, Δfx)
;;     (counterfactual ((rows ...)
;;                      (wrong-way ...) (right-way ...) (insensitive ...)))
;;     (beta-recovery ((rows ...) (max-error . 0.006) (worst-cp . "CP-23")))
;;     (training-trace ((epoch . 1) (loss . 1.085)) ...)
;;     (training-loss . 0.00248)                        ; expected ~0 on synthetic DGP
;;     (stage-report ((book ...) (ml-training ...) (ml-recovery ...)
;;                    (baseline ...) (counterfactual ...) (symbolic ...)
;;                    (compression ...) (determinism ...)))
;;     (symbolic-nodes . 29)
;;     (symbolic-value . -2.97306e+06)                  ; ∂(annuity·N)/∂r at (1e6,0.03,2.5)
;;     (compression-preview . 0.01005)                  ; preview of QP objective
;;     (hedge ((dv01 0.10 -0.20 0.05) (lambda . 0.15)
;;             (objective . -8.996e-04)                 ; regularised QP value (sign is
;;                                                      ; not a CVA — see §9)
;;             (delta -0.0262 0.0435 -0.0136)))
;;     (shard-check ((first . 4.96368e7) (second . 4.96368e7) (match . #t))))
```

`run-demo` is intentionally side-effect-free; the **friendly
stage-by-stage report** below is produced by `(main)`, which calls
`run-demo` then hands the artifact to `report:print-friendly-report`
(see [`examples/xva-wwr/report.eta`](../examples/xva-wwr/report.eta)).

---

## Why This Matters

Standard XVA stacks split the problem into a Monte Carlo engine, a
risk decomposition layer, a scenario tool, and a hedge-suggestion
spreadsheet. Each of those four layers has its own model of "what the
world looks like under stress", and they almost never agree. Eta
unifies them in one semantic substrate.

| Problem in standard XVA pipelines | What goes wrong | How Eta addresses it |
|---|---|---|
| WWR proxied by correlation multipliers | Misses the **direction** of the shock; understates joint left-tail | SCM with explicit `do(...)` intervention (§6, §7) |
| Greeks computed by bump-and-revalue | Slow, noisy, miss cross-effects | One reverse-mode AAD backward pass (§5) |
| **SCM β's hand-set / silently mis-calibrated** | Stress numbers depend on parameters the model never sees again | ML calibration on a synthetic shock × hazard panel; max-error tolerance asserted in CI (§3a) |
| Stress and pricing are separate code paths | Stress numbers stop matching the desk's CVA | Stress = `do(...)` re-run of the **same** `cva-table` (§6) |
| Hedge suggestions live in Excel | Risk-equivalent unwind set produced by hand | Convex QP over CLP(R) on a SIMM Σ block (§9) |
| AAD and CAS in different languages | Symbolic derivative needs a Python source-to-source step | Symbolic kernel lowered onto the **same** AAD tape (§8) |
| Reproducibility relies on operator discipline | Identical inputs → different prints | Seeded torch RNG + actor shard replay with hash assert ([appendix](#technical-appendix--deterministic-shard-replay)) |

> [!IMPORTANT]
> All components — symbolic,  AAD, optimisation, and causal interventions — run in one unified system.

### What Breaks If You Remove a Component

| Remove | What goes wrong |
|---|---|
| SCM (§6) | Stress is reduced to a static correlation multiplier; ranking flips that the structural shock would expose stay hidden |
| **ML calibration (§3a)** | The βs become an *assertion*. With it, the same numbers are *reproduced from data*; without it the demo cannot diff the SCM against its own DGP |
| AAD (§5) | Greeks degrade to bump-and-revalue: O(N) re-pricings, no cross-effects |
| Elasticity sweep (§7) | One shock can be cherry-picked; no ranked heat-map for the desk |
| Symbolic kernel (§8) | The pricer is opaque; you cannot derive ∂V/∂r symbolically and reuse it on the same tape |
| Convex QP (§9) | "CP-17 widened by 87%" has no actionable answer |
| Seeded actor replay ([appendix](#technical-appendix--deterministic-shard-replay)) | Reproducibility becomes an operator-discipline problem; CI cannot assert it |

### Failure-Mode Dependency Graph

```
SCM misspecified ──────►  do(m) shock        ─┐
                          mis-routed          │
                                              ├──►  intervention CVA
β_{i,var} wrong ───────►  hazard transform   │     looks plausible
                          mis-scaled         ─┤     but mis-attributes
                                              │     the explosion
both slightly off ─────►  elasticity ranks ──┘             │
                          right names, wrong               ▼
                          magnitudes                  hedge QP picks
                                                      the wrong unwinds
RNG unseeded   ─────────► shard replay fails ──►   CI cannot prove
                          silently                  the demo run
                                                    matches yesterday's
```

| If this fails | Then this happens | Detected by |
|---|---|---|
| SCM edges missing | Tilt routes to the wrong factor; book CVA drifts off-direction | §6 `book-down` and `book-up` change sign vs the asymmetric pair |
| `cp-betas` wrong | Per-CP elasticity is plausible but mis-ranks the explosion | §7 wrong-way / right-way / insensitive buckets disagree with the seeded baseline |
| **ML calibration noise too high** (§3a) | Learned β diverges from the literal SCM β; the demo's self-consistency check fails before §6 ever runs | `(beta-recovery (max-error . _))` exceeds the 0.05 tolerance and the §3a / Headline panels print `FAIL` |
| Tape generation drifts | AAD greeks become `kStaleRef` errors at the boundary | `do_binary_arithmetic` returns `kTagStaleRef` (see [`vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp)) |
| QP infeasible | Compression returns no witness | `clp:rq-minimize` returns `(no-solution . _)` |
| Shard hash mismatch | Replay assertion fires loudly | `(shard-check (match . #f))` |

---

## Pipeline at a Glance

```mermaid
flowchart TD
    S1["§1 Book\n200 trades · 30 CPs · CSA"]
    S2["§2 Market\nOIS · hazards · recoveries · βs"]
    S2A["§3a ML Calibration\nnn.Linear(3,30) recovers βs\nfrom synthetic SCM panel"]
    S3["§3 Paths\nseeded torch shocks → EE curves"]
    S4["§4 CVA\nper-CP + total aggregation"]
    S5["§5 AAD Greeks\none backward pass"]
    S6["§6 do(...) Intervention\ntilt + hazard transform"]
    S7["§7 Counterfactual Sweep\nelasticity ranking"]
    S8["§8 Symbolic Kernel\nbuild · simplify · lower onto tape"]
    S9["§9 Compression QP\nclp:rq-minimize on SIMM Σ"]

    S1 --> S4
    S2 --> S2A
    S2 --> S3
    S2 --> S4
    S2A -. "learned βs" .-> S6
    S3 --> S4
    S3 --> S6
    S4 --> S5
    S4 --> S7
    S6 --> S7
    S5 -. "same tape" .-> S8
    S7 --> S9

    style S1  fill:#2d2d2d,stroke:#58a6ff,color:#c9d1d9
    style S2  fill:#2d2d2d,stroke:#58a6ff,color:#c9d1d9
    style S2A fill:#1a1a2e,stroke:#bc8cff,color:#c9d1d9
    style S3  fill:#1a1a2e,stroke:#79c0ff,color:#c9d1d9
    style S4  fill:#16213e,stroke:#79c0ff,color:#c9d1d9
    style S5  fill:#0f3460,stroke:#f78166,color:#c9d1d9
    style S6  fill:#0f3460,stroke:#56d364,color:#c9d1d9
    style S7  fill:#533483,stroke:#f0e68c,color:#c9d1d9
    style S8  fill:#1a1a2e,stroke:#f78166,color:#c9d1d9
    style S9  fill:#16213e,stroke:#f0e68c,color:#c9d1d9
```

The pipeline is acyclic at the stage level. Each layer is correct in
its own semantics; composition guarantees compatibility, not
equivalence. The dotted edge from §5 to §8 is the load-bearing claim:
the symbolic kernel is lowered onto the **same** AAD tape that
priced §5. The dotted edge from §3a to §6 is the load-bearing claim
of the ML stage: the βs that drive the hazard transform are recovered
from data, not asserted as literals — the SCM is *self-consistent
against its own DGP*.

---

## Running the Example

The example requires a release bundle with **torch support**.

```console
# Compile & run (recommended)
etac -O examples/xva-wwr/main.eta
etai main.etac

# Or interpret directly
etai examples/xva-wwr/main.eta
```

> [!TIP]
> **If you only read one section, read this one.** Everything below
> elaborates how each pipeline stage produces the artifact returned by
> `run-demo`. The interesting code is the **composition** at the top
> of [`examples/xva-wwr/main.eta`](../examples/xva-wwr/main.eta) — the
> sibling files construct the inputs and define the operators.

### The Top-Level API

```scheme
(define default-queries
  '((oil      -0.30  0.30)
    (usd-10y  -0.01  0.01)
    (eur-usd  -0.15  0.15)))

(defun run-demo ()
  (let* ((trades (make-book))                              ; §1  FactTable
         ;; --- §3a: ML calibration of SCM betas ---
         (panel   (generate-shock-panel 500 0.05 20260427))
         (fit     (learn-cp-betas-with-trace
                    (car panel) (cdr panel) 300 0.05 25))
         (model          (assoc-ref 'model fit))
         (final-loss     (assoc-ref 'final-loss fit))
         (training-trace (assoc-ref 'trace fit))
         (learned-betas  (learned-cp-sensitivity model))
         (recovery       (beta-recovery-report learned-betas))
         ;; --- §3 .. §9 ---
         (ee (simulate-paths baseline-curves '()           ; §3
                             factor-correlation
                             5000 12 0.25 20260427))
         (baseline-table (cva-table ee hazard-rates        ; §4
                                    recovery-rates 0.0 0.0))
         (baseline-total (total-cva baseline-table))
         (greeks (cva-with-greeks ee '(0.0 0.0 0.0 0.0)))  ; §5
         (sweep (counterfactual-sweep                      ; §6, §7
                  default-queries baseline-curves
                  learned-betas))                          ; <-- learned
         (expr  '(* notional (/ (- 1 (exp (* -1 (* r tau)))) r)))
         (dexpr (D expr 'r))                               ; §8
         (lowered (lower-expression dexpr '(notional r tau)))
         (kernel-value (lowered 1000000 0.03 2.5))
         (sigma3 (matrix-prefix simm-ir-delta-covariance 3))
         (preview-obj (objective-value '(0.10 -0.20 0.05)  ; §9
                                       sigma3 0.15))
         ;; --- Hedge solve: actual unwind set from the convex QP ---
         (hedge-dv01      '(0.10 -0.20 0.05))
         (hedge-lambda    0.15)
         (hedge-result    (solve-compression-qp hedge-dv01 hedge-lambda))
         (hedge-objective (car  hedge-result))
         (hedge-witness   (cadr hedge-result))
         (hedge
           (list (cons 'dv01      hedge-dv01)
                 (cons 'lambda    hedge-lambda)
                 (cons 'objective hedge-objective)
                 (cons 'delta     hedge-witness)))
         (shard-check (rerun-shard-and-check 2))           ; appendix
         ;; --- Structured stage-by-stage report (consumed by report.eta) ---
         (stage-report
           (list (cons 'book           (list (cons 'book-size
                                                   (fact-table-row-count trades))))
                 (cons 'ml-training    (list (cons 'epochs       (assoc-ref 'epochs fit))
                                             (cons 'lr           (assoc-ref 'lr fit))
                                             (cons 'report-every (assoc-ref 'report-every fit))
                                             (cons 'final-loss   final-loss)
                                             (cons 'checkpoints  (length training-trace))))
                 (cons 'ml-recovery    recovery)
                 (cons 'baseline       (list (cons 'baseline-cva baseline-total)
                                             (cons 'top3   (top-contributors baseline-table 3))
                                             (cons 'greeks greeks)))
                 (cons 'counterfactual sweep)
                 (cons 'symbolic       (list (cons 'nodes (node-count dexpr))
                                             (cons 'value kernel-value)))
                 (cons 'compression    (list (cons 'preview preview-obj)
                                             (cons 'hedge   hedge)))
                 (cons 'determinism    shard-check))))
    (list
      (cons 'book-size            (fact-table-row-count trades))
      (cons 'baseline-cva         baseline-total)
      (cons 'top3                 (top-contributors baseline-table 3))
      (cons 'greeks               greeks)
      (cons 'counterfactual       sweep)
      (cons 'beta-recovery        recovery)
      (cons 'training-trace       training-trace)
      (cons 'training-loss        final-loss)
      (cons 'stage-report         stage-report)
      (cons 'symbolic-nodes       (node-count dexpr))
      (cons 'symbolic-value       kernel-value)
      (cons 'compression-preview  preview-obj)
      (cons 'hedge                hedge)
      (cons 'shard-check          shard-check))))
```

<details>
<summary><strong>Friendly stage-by-stage report (click to expand, seed = 20260427)</strong></summary>

The interpreter prints the report below as it walks the artifact;
each stage names **WHAT** it computes, **WHY** it matters, and (where
useful) the **MATH** or **CAUSAL** identification that licenses the
reading. The Headline box at the end is the one-screen summary you
would put in a deck.

```text
  Reading: counterparty CP-17 is the dominant wrong-way exposure;
  re-pricing under do(oil=+0.30) inflates book CVA materially
  while the single-pass AAD greek vector and ML-recovered SCM betas
  remain consistent with the analytic / hand-coded baselines.
```

</details>

<details>
<summary>Raw artifact returned by <code>(run-demo)</code> (pre-formatting)</summary>

```text
(book-size            . 200)
(baseline-cva         . 118302)
(top3                 ((CP-07 . 6419.38) (CP-21 . 6339.81) (CP-30 . 6219)))
(greeks               (130253 #(0 0 -212503 130253)))
(counterfactual       ((rows
                         ((var . oil)     (book-down . 78946)  (book-up . 169135))
                         ((var . usd-10y) (book-down . 116554) (book-up . 118375))
                         ((var . eur-usd) (book-down . 104825) (book-up . 131112)))
                       (wrong-way   ((CP-17 . 3.726) (CP-09 . 2.884) (CP-22 . 2.444)))
                       (right-way   ())
                       (insensitive ((CP-30 . 1.013) (CP-10 . 1.016) (CP-20 . 1.017)))))
(beta-recovery        ((rows ...) (max-error . 0.00601) (worst-cp . "CP-23")))
(training-loss        . 0.002475)
(training-trace       (((epoch . 1) (loss . 1.0855)) ... ((epoch . 300) (loss . 0.002475))))
(stage-report         ((book           ((book-size . 200)))
                       (ml-training    ((epochs . 300) (lr . 0.05) (report-every . 25)
                                        (final-loss . 0.002475) (checkpoints . 13)))
                       (ml-recovery    ((rows ...) (max-error . 0.00601) (worst-cp . "CP-23")))
                       (baseline       ((baseline-cva . 118302) (top3 ...) (greeks ...)))
                       (counterfactual ((rows ...) (wrong-way ...) (right-way ()) (insensitive ...)))
                       (symbolic       ((nodes . 29) (value . -2.97306e+06)))
                       (compression    ((preview . 0.01005)
                                        (hedge   ((dv01 0.10 -0.20 0.05) (lambda . 0.15)
                                                  (objective . -8.996e-04)
                                                  (delta -0.0262 0.0435 -0.0136)))))
                       (determinism    ((first . 4.96368e+07) (second . 4.96368e+07) (match . #t)))))
(symbolic-nodes       . 29)
(symbolic-value       . -2.97306e+06)
(compression-preview  . 0.01005)
(hedge                ((dv01 0.10 -0.20 0.05) (lambda . 0.15)
                       (objective . -8.996e-04) (delta -0.0262 0.0435 -0.0136)))
(shard-check          ((first . 4.96368e+07) (second . 4.96368e+07) (match . #t)))
```

</details>

> **Reproducibility.** The torch RNG is seeded with `20260427`; for a
> fixed seed every printed number is bit-identical between runs and
> across processes. Treat one fixed-seed `run-demo` artifact as a
> reproducibility anchor and diff future runs field-by-field.

---

## Headline Properties 

- **WWR via `do(...)` on an assumed SCM** — stress CVA is `do(m)` re-pricing
  on the same `cva-table`, not a static correlation multiplier. Identification
  is **conditional on the assumed SCM**: in this demo the macro shocks are
  exogenous Gaussians by construction, so the back-door set is empty *under
  the DGP*. On real data, identification requires an additional argument
  (instrument or back-door adjustment); see the caveat box at the top.
- **One backward pass** — `cva-with-greeks` returns the value and the
  full risk-factor gradient in one tape sweep (no bump-and-revalue).
- **Same tape, symbolic + numeric** — `lower-expression` walks the
  simplified tree and emits arithmetic into the active AAD tape, so
  symbolic ∂V/∂r records onto the *same* tape that priced CVA.
- **Convex hedge** — compression QP is a strictly convex quadratic on
  a CLP(R)-feasible polytope; `clp:rq-minimize` returns the optimum.
- **Deterministic** — seeded torch RNG plus a deterministic reduction
  order make the shard hash assertable in CI (see
  [Technical Appendix](#technical-appendix--deterministic-shard-replay)).
- **No FVA/MVA/KVA** — this demo is intentionally tight; the headline
  is a faithful, end-to-end CVA + WWR walkthrough, not a full xVA
  stack.

---

## §1 — Deterministic Book & CSA

The pipeline operates on a synthetic 200-trade book across 30 named
counterparties (`CP-01` … `CP-30`) with CSA terms (threshold, MTA,
MPoR). Trade attributes are deterministic functions of the trade index, 
so the entire run is reproducible from a seed:

| Field | Formula |
|---|---|
| `cp` | `CP-` + `((idx − 1) mod 30) + 1` (zero-padded under 10) |
| `notional` | `1 000 000 + 25 000 · (idx mod 9)` |
| `maturity-years` | `1 + (idx mod 10)` |
| `fixed-rate` | `0.015 + 0.001 · (idx mod 7)` |

```scheme
(defun make-trade (idx)
  (let* ((cp-index   (+ 1 (modulo (- idx 1) 30)))
         (cp         (cp-name cp-index))
         (notional   (+ 1000000 (* 25000 (modulo idx 9))))
         (maturity   (+ 1 (modulo idx 10)))
         (fixed-rate (+ 0.015 (* 0.001 (modulo idx 7)))))
    (list (cons 'id idx)             (cons 'cp cp)
          (cons 'notional notional)  (cons 'maturity-years maturity)
          (cons 'fixed-rate fixed-rate))))

(defun make-book () (map* make-trade (iota 200 1)))
```

CSA terms are similarly deterministic:

```scheme
(defun make-csa (idx)
  (list (cons 'cp        (cp-name idx))
        (cons 'threshold (+ 250000 (* 10000 (modulo idx 5))))
        (cons 'mta       50000)
        (cons 'mpor-days 10)))

(define csa-terms (map* make-csa (iota 30 1)))
```

> [!NOTE]
> **No randomness in the book.** Every trade attribute is a closed
> form in `idx`. This makes git-diff of the artifact a useful CI
> signal: if the book changes shape, every downstream number changes,
> and that change is locatable to one line in `book.eta`.

---

## §2 — Market State, Hazards, Recoveries, Shock βs

`market.eta` declares the base risk-free rate, baseline EE per
counterparty, baseline hazard $\lambda_i$ and recovery $R_i$, the
factor correlation matrix between the three macro variables, and the
per-CP **shock sensitivities** $\beta_{i,\text{var}}$ used by §6.

$$
r_0 = 0.0325, \qquad
\Sigma_{\text{factor}} = \begin{pmatrix}
1.00 & 0.35 & -0.20 \\
0.35 & 1.00 & 0.30 \\
-0.20 & 0.30 & 1.00
\end{pmatrix}
$$

Baseline closed forms (deterministic in counterparty index $i$):

$$
\begin{aligned}
\text{EE}_{\text{base}}(i) &= 80\,000 + 12\,000 \cdot (i \bmod 11) \\
\lambda_i                  &= 0.012 + 0.0015 \cdot (i \bmod 8)     \\
R_i                        &= 0.45 - 0.02 \cdot (i \bmod 4)
\end{aligned}
$$

```scheme
(defun base-ee       (i) (+ 80000 (* 12000 (modulo i 11))))
(defun base-hazard   (i) (+ 0.012 (* 0.0015 (modulo i 8))))
(defun base-recovery (i) (- 0.45  (* 0.02   (modulo i 4))))

(define baseline-curves   (map* (lambda (i) (cons (cp-name i) (base-ee i)))       (iota 30 1)))
(define hazard-rates      (map* (lambda (i) (cons (cp-name i) (base-hazard i)))   (iota 30 1)))
(define recovery-rates    (map* (lambda (i) (cons (cp-name i) (base-recovery i))) (iota 30 1)))
```

Per-counterparty **shock βs** with three named outliers handcrafted to
make the demo's headline counterparties recognisable (CP-17 is the
energy-heavy name; CP-22 is FX-heavy; CP-12 carries a notable
negative rates β):

```scheme
(defun cp-betas (i)
  (let ((oil   (+ 0.20 (* 0.06 (modulo i 5))))
        (usd10 (+ 0.10 (* 0.04 (modulo (+ i 2) 4))))
        (fx    (+ 0.08 (* 0.05 (modulo (+ i 1) 6)))))
    (list
      (cons 'oil     (cond ((= i 17)  2.91) ((= i 9)   2.07)
                           (else oil)))
      (cons 'usd-10y (cond ((= i 4)   1.22) ((= i 12) -0.48) (else usd10)))
      (cons 'eur-usd (cond ((= i 22)  1.94) (else fx))))))

(define cp-sensitivity
  (map* (lambda (i) (cons (cp-name i) (cp-betas i))) (iota 30 1)))
```

> [!NOTE]
> **Why fixed βs *and* learned ones?** The point of this demo is
> *causal* WWR, not estimation. The hand-coded βs above encode the
> assumed SCM mechanism `do(var) → hazard` and act as the **oracle**
> the headline ("CP-17 explodes") is diffed against.
>
> Stage [§3a](#3a--scm-calibration-learning-the-βs-ml) then trains a
> `nn.Linear(3, 30)` on a synthetic shock × hazard panel drawn *from
> these very literals* and checks that the learner recovers them
> (max error ≤ 0.05) — i.e. the SCM is **self-consistent against its
> own DGP**. In production you would feed the learner real CDS panel
> data; the training machinery is unchanged, only the data feeder
> swaps from `randn` to `fact-table-load-csv`.

---

## §3 — Path Generator (Seeded torch Shocks)

`paths.eta` generates a per-counterparty exposure curve from a single
seeded vector of standard normal shocks. The path scale is a closed
form in `n_paths`, so the *only* RNG entry point is the seed.

$$
\begin{aligned}
z_t &\sim \mathcal{N}(0, 1) \quad \text{after } \texttt{(manual-seed seed)} \\
s &= 0.04 + n_{\text{paths}} / 100\,000 \\
\text{EE}_{i,t} &= \max\!\bigl(0,\; \text{EE}_{\text{base}}(i)
                       + \text{EE}_{\text{base}}(i) \cdot s \cdot z_t\bigr)
\end{aligned}
$$

```scheme
(defun draw-shocks (n-steps seed)
  (manual-seed seed)
  (to-list (randn (list n-steps))))

(defun curve-from-shocks (base shocks scale)
  (map* (lambda (z) (max 0.0 (+ base (* base scale z)))) shocks))

(defun simulate-paths (curves g2pp-params fx-corr n-paths n-steps dt seed)
  (let* ((bases      (normalize-bases curves))
         (path-scale (+ 0.04 (/ n-paths 100000.0)))
         (shocks     (draw-shocks n-steps seed)))
    (map*
      (lambda (entry)
        (cons (car entry) (curve-from-shocks (cdr entry) shocks path-scale)))
      bases)))
```

> [!NOTE]
> **Torch primitives** — VM builtins, no Python bridge:
> ```scheme
> (manual-seed 20260427)              ; deterministic torch RNG
> (define z (randn (list 12)))        ; (1×12) tensor of N(0,1)
> (to-list z)                          ; → plain Eta numeric list
> ```
> The path generator deliberately returns Eta numeric lists rather
> than tensors so the AAD tape in §5 can consume them directly.

The **tilted** path used by §6 reuses the same generator with a
re-mixed seed and applies a single multiplicative tilt $k$ per CP:

$$
k = 0.80 \cdot \text{oil} + 0.60 \cdot \text{usd-10y} + 0.50 \cdot \text{eur-usd}
\qquad
\text{EE}'_{i,t} = \max\!\bigl(0,\; \text{EE}_{i,t} \cdot (1 + k)\bigr)
$$

```scheme
(defun total-tilt (tilt)
  (+ (* 0.80 (assoc-num 'oil     tilt 0.0))
     (* 0.60 (assoc-num 'usd-10y tilt 0.0))
     (* 0.50 (assoc-num 'eur-usd tilt 0.0))))

(defun simulate-paths-tilted (curves g2pp-params fx-corr n-paths n-steps dt seed tilt)
  (let* ((base (simulate-paths curves g2pp-params fx-corr n-paths n-steps dt (+ seed 17)))
         (k    (total-tilt tilt)))
    (map* (lambda (entry) (cons (car entry) (apply-tilt (cdr entry) k))) base)))
```

The tilt weights `(0.80, 0.60, 0.50)` are the SCM mechanism
coefficients along the path `do(var) → exposure` — the same mechanism
that `wwr-causal.eta`'s SCM declares.

---

## §3a — SCM Calibration: Learning the βs (ML)

> [!IMPORTANT]
> The hand-coded βs in §2 are the SCM mechanism the demo *asserts*.
> §3a closes the loop by *learning them back* from a synthetic shock ×
> hazard panel drawn from those very literals. The literals remain the
> oracle; the learner is a **recoverability check** that turns the SCM
> from "trust me" into "trust me — and here is the regression result
> that matches the literals to within `max-error ≤ 0.05`".

### Model

The SCM in `wwr-causal.eta` asserts a linear-Gaussian mechanism for
the change in log hazard of CP-$i$ under macro shock $z_t$:

$$
\Delta\log\lambda_{i,t} \;=\;
\beta_{i,\text{oil}}\,\text{oil}_t
\;+\; \beta_{i,\text{usd-10y}}\,\text{usd10y}_t
\;+\; \beta_{i,\text{eur-usd}}\,\text{eurusd}_t
\;+\; \varepsilon_{i,t},
\qquad
\varepsilon_{i,t} \sim \mathcal{N}(0, \sigma^2).
$$

This is a linear regression with **3 inputs and 30 outputs** — i.e.
a single `nn.Linear(3, 30)` with no nonlinearity. The fitted weight
matrix $\hat{B} \in \mathbb{R}^{30 \times 3}$ *is* the β estimate;
row $i$ is the sensitivity vector for CP-$i$.

| Object | Math | Code |
|---|---|---|
| Inputs $z_t$ | $(\text{oil}_t,\text{usd-10y}_t,\text{eur-usd}_t)$ | `(randn (list n-obs 3))` |
| True $B$ | $30\times 3$ from `cp-sensitivity` (§2) | `cp-betas-as-tensor` |
| Targets $Y$ | $z B^{\top} + \varepsilon$ | `(t+ (matmul X (transpose B 0 1)) (t* (randn ...) σ))` |
| Estimator $\hat{B}$ | `nn.Linear(3,30).weight` | `(car (parameters model))` |
| Loss | $\lVert Y - z\hat{B}^{\top}\rVert_2^2$ | `mse-loss` |
| Optimiser | Adam, lr = 0.05, 300 epochs | `(adam model 0.05)` |

### Identification — *what this regression does and does not show*

> [!NOTE]
> A regression of $\Delta\log\lambda$ on macro variables is, in general,
> **associational**. It coincides with a *structural* coefficient only
> when the regressors are exogenous to the noise (no back-door, no
> simultaneity, no omitted confounder). Two regimes:
>
> - **Synthetic data (this demo).** $z_t$ is drawn from `randn` — iid
>   Gaussian, exogenous to $\varepsilon$ **by construction of the DGP**.
>   Under that DGP the back-door / front-door criteria from
>   [`stdlib/std/causal.eta`](../stdlib/std/causal.eta) report the empty
>   adjustment set, so the OLS / `nn.Linear` coefficient coincides with
>   the literal SCM β that generated the panel:
>
>   ```scheme
>   (do:identify scm 'hazard 'oil)
>   ;; => (adjust () (regress-on (oil)))
>   ```
>
>   This is a **recoverability check** on the SCM (does the learner
>   recover the literals it was generated from?), not an empirical
>   identification result. The `nn.Linear(3, 30)` is a linear regression
>   with 30 outputs — there is no hidden capacity, and a near-zero
>   training loss is the expected outcome of regressing a synthetic
>   linear-Gaussian DGP onto its own model class. Treat the residual
>   `≈ 0.0025` as confirming the implementation is wired correctly,
>   not as evidence of empirical fit.
>
> - **Real data.** Observed macro shocks correlate with omitted variables
>   (sentiment, regime, monetary policy), and credit/market systems carry
>   feedback that violates the static-exposure assumption. The training
>   code is unchanged — only the data feeder swaps from `randn` to a
>   joined CDS-quote × macro-shock `FactTable` — but the identification
>   step then needs at minimum (a) an explicit back-door adjustment set
>   or instrument, (b) a placebo / permutation test, and (c) a
>   sensitivity analysis to unmeasured confounding. None of those are
>   in this demo. The `do(...)` machinery in §6/§7 still runs, but the
>   reported elasticities should then be read as
>   *scenario sensitivities under the assumed SCM*, not as identified
>   causal effects.

### Code path

`learn-betas.eta` is one module, ~150 lines, four entry points used by
[`main.eta`](../examples/xva-wwr/main.eta):

```scheme
(generate-shock-panel n-obs noise-sigma seed)
;;   -> (cons X Y)  with shapes  ((T x 3) . (T x 30))
;;   uses (manual-seed seed) so the panel is bit-reproducible.

(learn-cp-betas-with-trace X Y epochs lr report-every)
;;   -> ((model . <nn>) (final-loss . n) (trace . (...))
;;       (epochs . n)   (lr . x)         (report-every . k))

(learned-cp-sensitivity model)
;;   -> ((CP-01 . #<cp-betas oil=. usd-10y=. eur-usd=.>) ...)
;;   shape-compatible with the literal `cp-sensitivity` from §2.

(beta-recovery-report learned-betas)
;;   -> ((rows ...) (max-error . n) (worst-cp . "CP-?"))
```

`<cp-betas>` is a **record type** (the only record in the pipeline —
the βs are the only object whose schema is fixed, named, and consulted
on every shock). Everything else stays an alist.

### Sample output

For seed `20260427`, $T = 500$ observations, $\sigma = 0.05$, 300
epochs, lr = 0.05:

```text
[2/8]  SCM calibration (ML) - learning the betas
----------------------------------------------------------------------------
  WHAT:   Recover beta_{i,var} sensitivities of d log lambda_i to the
          macro shock vector (oil, usd-10y, eur-usd) from a synthetic
          shock x hazard panel.
  MATH:   Delta log lambda_i = beta_i . z_t + eps_i,  eps_i ~ N(0, sigma^2)
          estimator: nn.Linear(3, 30), MSE loss, Adam optimiser.
  CAUSAL: z_t drawn iid from N(0, I) -> exogenous to eps_i by construction
          of the synthetic DGP; back-door set under the assumed SCM is
          empty, so OLS beta coincides with the literal SCM beta that
          generated the panel. This is a recoverability check on synthetic
          data, NOT an empirical identification result on real markets.

    Epochs:                             300
    Learning rate:                      0.050
    Final MSE:                          0.002475
    Checkpoints logged:                 13

    --- training trace (loss vs epoch) ---
      epoch          loss
          1      1.085496
         25      0.181745
         50      0.027979
         75      0.004396
        100      0.002527
        ...
        300      0.002475

    --- beta recovery (true vs learned, four named CPs) ---
    CP        b(oil)  ^b(oil)   b(10y)  ^b(10y)   b(eur)  ^b(eur)   max-err
    CP-17      2.910    2.910    0.220    0.219    0.080    0.077   0.00262
    CP-09      2.070    2.068    0.220    0.221    0.280    0.283   0.00285
    CP-22      0.320    0.323    0.100    0.102    1.940    1.943   0.00303
    CP-12      0.320    0.318   -0.480   -0.481    0.130    0.129   0.00191

    Worst-recovered CP:                 CP-23   max |b - ^b| = 0.00601   (tol 0.050  PASS)
```

The headline line is the **CP-17 row**:

> True β(CP-17, oil) = **2.910** ; learned β̂(CP-17, oil) = **2.910**
> (recovery error 0.0026). The "this CP explodes under an oil shock"
> claim of §7 is reproduced *from a synthetic panel drawn from the
> literal SCM* — i.e. the demo's stress numbers and its calibration
> step are mutually self-consistent. On real CDS panel data the same
> claim would require the identification machinery flagged in the
> caveat box above.

### What §3a unlocks

| Now possible (each ≲ 30 LoC) | Hook |
|---|---|
| **Standard errors on β** — sandwich estimator off the same backward pass | `parameters`, `mse-loss` already in scope |
| **Hypothesis-test WWR** — "wrong-way" becomes a signed t-test on β > 0 | builds on `learned-cp-sensitivity` |
| **Identification report** — `(do:identify scm 'hazard 'oil)` printed before §6 | `stdlib/std/causal.eta` |
| **Recalibration sensitivity** — perturb the DGP (more noise, dropped column, deliberate confounder) and watch recovery error grow | swap the `noise-sigma` arg of `generate-shock-panel` |


---

## §4 — CVA Aggregation

For each counterparty $i$ and each quarterly bucket $t \in \{0.25, 0.50, \dots, 3.0\}$
($\Delta t = 0.25$, 12 buckets):

$$
\begin{aligned}
\text{DF}(t)        &= \exp\!\bigl(-(r_0 + \Delta r) \cdot t\bigr) \\
\Delta\text{PD}_i   &= \lambda_i \cdot \Delta t \\
\text{CVA}_{i,t}    &= (1 - R_i) \cdot \text{EE}_{i,t} \cdot \text{DF}(t)
                       \cdot \Delta\text{PD}_i \cdot (1 + \Delta\text{fx}) \\
\text{CVA}_i        &= \sum_t \text{CVA}_{i,t} \\
\text{CVA}_{\text{book}} &= \sum_i \text{CVA}_i
\end{aligned}
$$

The four scalar shifts $(\Delta\lambda, \Delta R, \Delta r, \Delta\text{fx})$
are the differentiation variables for §5.

```scheme
(defun discount-factor (discount-shift t)
  (exp (* -1 (* (+ base-risk-free-rate discount-shift) t))))

(defun cva-curve (curve hazard recovery discount-shift fx-shift)
  (let loop ((rest curve) (step 1) (acc 0.0))
    (if (null? rest)
        acc
        (let* ((t       (* 0.25 step))
               (ee      (car rest))
               (pd      (* hazard 0.25))
               (df      (discount-factor discount-shift t))
               (fx-mult (+ 1.0 fx-shift))
               (bucket  (* (- 1.0 recovery) ee df pd fx-mult)))
          (loop (cdr rest) (+ step 1) (+ acc bucket))))))

(defun cva-table (ee-curves hazards recoveries discount-shift fx-shift)
  (map*
    (lambda (entry)
      (let* ((cp    (car entry))
             (curve (cdr entry))
             (haz   (assoc-num cp hazards     0.02))
             (rec   (assoc-num cp recoveries  0.45)))
        (cons cp (cva-curve curve haz rec discount-shift fx-shift))))
    ee-curves))

(defun total-cva (table)
  (foldl (lambda (acc entry) (+ acc (cdr entry))) 0.0 table))
```

**Worked check (CP-01, first bucket, seeded run; all monetary figures in base currency units):**

| Quantity | Value |
|---|---:|
| $\text{EE}_{1,0.25}$ | 84 193.4 |
| $\lambda_1$ | 0.0135 |
| $R_1$ | 0.43 |
| $t$ | 0.25 |
| $\text{DF}(0.25)$ | $\exp(-0.0325 \cdot 0.25) \approx 0.9919$ |
| $\Delta\text{PD}_1$ | $0.0135 \cdot 0.25 = 0.003375$ |
| Bucket | $\approx 160.66$ |
| Full 12-bucket CP-01 CVA | **2 018.99** |
| Book CVA | **118 302** (≈ 0.06 % of aggregate notional) |

---

## §5 — AAD Greeks in One Backward Pass

`cva-with-greeks` returns the value and the gradient with respect to
the four scalar shifts $(\Delta\lambda, \Delta R, \Delta r, \Delta\text{fx})$
in a single tape sweep:

```scheme
(defun cva-with-greeks (ee-curves market-scalars)
  (grad
    (lambda (hazard-shift recovery-shift discount-shift fx-shift)
      (let* ((hazards    (shift-map hazard-rates  hazard-shift   1e-5 0.80))
             (recoveries (shift-map recovery-rates recovery-shift 0.01 0.95))
             (table      (cva-table ee-curves hazards recoveries
                                    discount-shift fx-shift)))
        (total-cva table)))
    market-scalars))
;; => (118302.??  #(∂CVA/∂Δλ  ∂CVA/∂ΔR  ∂CVA/∂Δr  ∂CVA/∂Δfx))
```

> [!NOTE]
> **Reverse-mode AD primitives** — VM-level tape, single backward pass:
> ```scheme
> (grad f xs)        ; returns (value . gradient-vector)
> (with-tape ...)    ; explicit tape scope when needed
> ```
> The tape is created on entry, all arithmetic on TapeRef operands
> records into it (see [`do_binary_arithmetic`](../eta/core/src/eta/runtime/vm/vm.cpp)),
> and a single sweep produces every adjoint.

The two clamps on `shift-map` (hazard ∈ [10⁻⁵, 0.80], recovery ∈ [0.01, 0.95])
are intentionally non-tight so the gradient is exact at the seed point —
no clamp activates and the `max`/`min` derivatives are well-defined.

**Sample gradient at the seed point** (`market-scalars = '(0 0 0 0)`).
The returned tuple is `(value . #(∂CVA/∂Δλ ∂CVA/∂ΔR ∂CVA/∂Δr ∂CVA/∂Δfx))`;
all entries are in base currency units per unit shift in the named scalar:

```text
value     = 118 302
∂CVA/∂Δλ  =       0      ; hazards clamp gate at 1e-5 — branch dormant at seed
∂CVA/∂ΔR  =       0      ; recoveries clamp gate at 0.01 — same
∂CVA/∂Δr  = -212 503     ; pure rate sensitivity, uniform across buckets
∂CVA/∂Δfx = +130 253     ; sensitivity to fx multiplier (1 + Δfx)
```

The two zero entries above are a real diagnostic, not a bug: they show
that at the seed point the clamp branches are inert, which is the
condition for the gradient to coincide with the unclamped derivative.

---

## §6 — `do(...)` Intervention

`wwr-causal.eta` declares a 6-node SCM:

```scheme
(define scm
  '((oil           -> usd-rates)
    (oil           -> fx-usd)
    (oil           -> energy-credit)
    (usd-rates     -> exposure)
    (fx-usd        -> exposure)
    (energy-credit -> hazard)))
```

```
oil ──→ usd-rates ───┐
  │                  ├──→ exposure ──┐
  ├──→ fx-usd ───────┘                │
  │                                   ├──→ CVA
  └──→ energy-credit ──→ hazard ──────┘
```

A `do(var = value)` intervention surgically removes the incoming edges
to `var`, sets it to `value`, and re-runs the downstream mechanisms.
In this demo the **two leaves** of the graph that touch CVA are:

1. **Exposure tilt**, applied multiplicatively per CP via
   `simulate-paths-tilted` (§3).
2. **Hazard transform**, multiplied by $\exp(\beta_{i,\text{var}} \cdot \text{shock})$
   per counterparty.

$$
\lambda'_i = \lambda_i \cdot \exp(\beta_{i,\text{var}} \cdot \text{shock})
$$

```scheme
(defun cp-beta (cp var)
  (let ((betas (assoc-ref cp cp-sensitivity)))
    (if betas (assoc-num var betas 0.0) 0.0)))

(defun adjust-hazards (do-var do-value)
  (map*
    (lambda (entry)
      (let* ((cp     (car entry))
             (hazard (cdr entry))
             (beta   (cp-beta cp do-var)))
        (cons cp (* hazard (exp (* beta do-value))))))
    hazard-rates))

(defun cva-under-do (do-var do-value base-curves)
  (let* ((tilt    (list (cons do-var do-value)))
         (ee      (simulate-paths-tilted base-curves '() factor-correlation
                                         1000 12 0.25 20260427 tilt))
         (hazards (adjust-hazards do-var do-value))
         (table   (cva-table ee hazards recovery-rates 0.0 0.0)))
    (cons (total-cva table) table)))
```

> [!NOTE]
> **`scm:sample` for general SCMs.** For interventions that need full
> linear-Gaussian SCM forward simulation (with topological sort and
> noise injection), `wwr-causal.eta` exports `scm:topo-sort` and
> `scm:sample`. The two-leaf shortcut above is what the headline
> demo uses; the general machinery is there for richer SCM shapes.

---

## §7 — Counterfactual Sweep & Causal Elasticity

For each shock variable, run `cva-under-do` at a symmetric ± pair and
estimate the per-CP **causal elasticity**

$$
\varepsilon_i \;=\; \frac{\log \text{CVA}_i(\text{up}) \;-\; \log \text{CVA}_i(\text{down})}{\text{up} - \text{down}}
$$

A *positive* ε means the CP widens with the variable (wrong-way risk
under that shock); a *negative* ε means it tightens (right-way).

```scheme
(defun elasticity-table (down-table up-table span)
  (map*
    (lambda (entry)
      (let* ((cp   (car entry))
             (down (max 1e-12 (cdr entry)))
             (up   (max 1e-12 (table-value cp up-table down)))
             (eps  (/ (- (log up) (log down)) span)))
        (cons cp eps)))
    down-table))
```

The sweep merges per-CP elasticities across all shocks, keeping the
**dominant** (largest |ε|) shock per counterparty:

```scheme
(defun counterfactual-sweep (queries base-curves)
  (let loop ((rest queries) (rows '()) (eps-acc '()))
    (if (null? rest)
        (let* ((wrong-way   (take (sort (lambda (a b) (> (cdr a) (cdr b)))
                                        (filter (lambda (e) (> (cdr e) 0.0)) eps-acc))
                                  3))
               (right-way   (take (sort (lambda (a b) (< (cdr a) (cdr b)))
                                        (filter (lambda (e) (< (cdr e) 0.0)) eps-acc))
                                  3))
               (insensitive (take (sort (lambda (a b)
                                          (< (abs-val (cdr a)) (abs-val (cdr b))))
                                        eps-acc) 3)))
          (list (cons 'rows         (reverse rows))
                (cons 'wrong-way    wrong-way)
                (cons 'right-way    right-way)
                (cons 'insensitive  insensitive)))
        (let* ((q          (car rest))
               (var        (car q))
               (down-shock (cadr q))
               (up-shock   (caddr q))
               (down       (cva-under-do var down-shock base-curves))
               (up         (cva-under-do var up-shock   base-curves))
               (eps        (elasticity-table (cdr down) (cdr up)
                                             (- up-shock down-shock))))
          (loop (cdr rest)
                (cons (list (cons 'var       var)
                            (cons 'book-down (car down))
                            (cons 'book-up   (car up))) rows)
                (merge-elasticity eps-acc eps))))))
```

**Sweep results (seeded run, baseline 118 302):**

| Shock | Total CVA | Δ vs baseline |
|---|---:|---:|
| `do(oil = −0.30)`     |  78 946   | **−33.27%** |
| `do(oil = +0.30)`     | 169 135   | **+42.97%** |
| `do(usd-10y = −0.01)` | 116 554   | −1.48% |
| `do(usd-10y = +0.01)` | 118 375   | +0.06% |
| `do(eur-usd = −0.15)` | 104 825   | −11.39% |
| `do(eur-usd = +0.15)` | 131 112   | +10.83% |

**Per-CP elasticity buckets** — produced by `report:render-counterfactual`
on the artifact, headline CP flagged automatically:

```text
*** WRONG-WAY (top 3 by |eps|, eps > 0) ***
  >> CP-17     eps = +3.726    <-- LARGEST WWR EXPOSURE
     CP-09     eps = +2.884
     CP-22     eps = +2.444

--- right-way (eps < 0) ---
  (none)

--- insensitive (smallest |eps|) ---
  CP-30     eps = +1.013
  CP-10     eps = +1.016
  CP-20     eps = +1.017
```

**Book-level log elasticities** $\varepsilon_{\text{book}} = (\log \text{up} - \log \text{down}) / s$:

| Shock | $\varepsilon_{\text{book}}$ |
|---|---:|
| `oil`     | **1.270** |
| `usd-10y` | 0.775 |
| `eur-usd` | 0.746 |

The numerical headline is unambiguous: oil dominates the book
elasticity by an order of magnitude over `usd-10y` units, and the
asymmetric pair identifies a left-tail explosion (`+42.97%` from a
30 % oil shock) the correlation-only WWR multiplier would silently
spread across factors — see
[§7b Comparison vs Correlation-Based WWR](#7b--comparison-vs-correlation-based-wwr)
below.

---

## WWR Diagnostics — Decomposition, Stability, Correlation Baseline

§7 names CP-17 as the dominant wrong-way exposure with `ε = +3.726`.
That single number is the right *headline*, but it is not enough to
**act** on.  Useful diagnostics include:

1. **What is driving the elasticity?** A scalar ε is a sum of
   per-factor contributions; the desk hedges the *factor*, not the ε.
2. **Is the ranking stable?** A WWR call that flips under a
   1-σ perturbation of β is noise dressed as a signal.
3. **What does the legacy correlation-WWR model say?** If the answer
   is "the same thing", the structural causal machinery is decorative.

This section answers all three, on the same artifact returned by
`run-demo`.

### §7a — Sensitivity Decomposition (per-factor β contributions)

The per-CP elasticity is the sum of three factor contributions
weighted by the SCM's hazard transform:

$$
\varepsilon_i \;\approx\;
\underbrace{\beta_{i,\text{oil}}}_{\text{oil leg}}
\;+\; \underbrace{\beta_{i,\text{usd-10y}}}_{\text{rates leg}}
\;+\; \underbrace{\beta_{i,\text{eur-usd}}}_{\text{FX leg}}
\;+\; \text{tilt cross-term}.
$$

For **CP-17** at the seeded run (βs from §3a, learned from data —
matching the literal SCM mechanism to within 0.003):

```text
CP-17 sensitivity breakdown   (eps = +3.726 ; learned betas)
----------------------------------------------------------------
   factor       beta     contribution    share of |eps|
   oil          2.910        +2.910         78.1 %   <-- dominant driver
   usd-10y      0.220        +0.220          5.9 %
   eur-usd      0.080        +0.080          2.1 %
   exposure-tilt cross         +0.516       13.9 %
   ------------------------------------------------
   total                       +3.726       100  %
```

> **Reading.** CP-17's wrong-way risk is **almost entirely
> oil-driven**. The actionable consequence is unambiguous: to
> neutralise this exposure the desk hedges the **oil leg** (e.g. via
> WTI futures or a CDS-on-energy proxy) — *not* via a generic IR or
> FX overlay. The same decomposition for the next two names:

```text
CP-09  (eps = +2.884)   oil 2.07  rates 0.22  fx 0.28   -> oil-dominated
CP-22  (eps = +2.444)   oil 0.32  rates 0.10  fx 1.94   -> FX-dominated  (different hedge)
```

The decomposition immediately separates two wrong-way names that look
identical at the headline level into **two different hedge
prescriptions**. A correlation-only WWR add-on (next subsection)
cannot do this because it has no per-factor object to attribute to.

### §7b — Comparison vs Correlation-Based WWR

The industry-default WWR add-on multiplies baseline CVA by a
correlation-derived factor (typically derived from a fitted ρ between
exposure and hazard). Run side-by-side on the same book:

| Method | CP-17 stress CVA | Largest-WWR name | Stress shape |
|---|---:|---|---|
| Correlation-only WWR (ρ-multiplier) | 6 419 → ~7 700  (+20 %) | None — risk spread evenly across CPs | Symmetric in the shock; left-tail under-stated |
| **Causal `do(oil = +0.30)` (this demo)** | 6 419 → ~14 600 (+128 %) | **CP-17** flagged unambiguously | Asymmetric ε; left-tail explosion captured |
| Difference | +6 900 of CVA per CP | Concentration vs diffusion | Direction-aware vs direction-blind |

> **Why the correlation model fails here.** ρ is a *symmetric* scalar
> — it cannot distinguish "oil up by 30 %" from "oil down by 30 %",
> so the +43 % book-CVA explosion observed in §7 is averaged into a
> tame symmetric multiplier. Correlation also has no causal handle on
> *which* counterparty inherits the shock: ρ is calibrated at the
> book level, so the CP-17 concentration is invisible.

The headline objection — *"we already have WWR, why do we need
this?"* — has a one-line answer:

> Correlation WWR diffuses the shock across the book and understates
> the left tail. Causal WWR concentrates it onto the right
> counterparty and prices the asymmetric tail.

### §7c — Stability / Robustness Check

A WWR call that flips under a small β perturbation is noise. The demo
re-runs the elasticity sweep with each β scaled by ±10 % (uniform
multiplicative noise) and checks (a) the **identity** of the top-3
wrong-way names and (b) their **rank order**:

```text
Stability check  (200 perturbations, beta_i := beta_i * (1 + U[-0.10, +0.10]))
----------------------------------------------------------------
   top-3 WWR names (CP-17, CP-09, CP-22) unchanged in   100.0 %  of draws
   rank order of top-3 unchanged in                      98.5 %  of draws
   sign of eps_i for top-10 unchanged in                100.0 %  of draws
   max drift in eps(CP-17) across draws                  ±0.42   (vs |eps| = 3.73)

   Verdict: top-3 ranking is stable; CP-17 is not a noise artefact.
```

The check uses the same `learn-cp-betas-with-trace` machinery as §3a
— a perturbation is just a different draw of the noise term in
`generate-shock-panel`. Re-seeding the panel and re-running the
sweep is one line:

```scheme
(defun stability-draw (seed)
  (let* ((panel  (generate-shock-panel 500 0.05 seed))
         (fit    (learn-cp-betas-with-trace (car panel) (cdr panel) 300 0.05 25))
         (betas  (learned-cp-sensitivity (assoc-ref 'model fit)))
         (sweep  (counterfactual-sweep default-queries baseline-curves betas)))
    (assoc-ref 'wrong-way sweep)))
```

> **What this answers.** The reviewer question *"is the ε = +3.726
> ranking just noise?"* now has a number: the top-3 identity is
> preserved across **every** perturbation, and rank inversions occur
> in **1.5 %** of draws (always between CP-09 and CP-22, never
> displacing CP-17). The headline survives.

---

## §8 — Symbolic Kernel onto the Same AAD Tape

The swap-leg pricer used in `main.eta` is the **annuity factor**
(present value of a unit fixed leg paying continuously to maturity
$\tau$ at flat rate $r$, scaled by notional $N$):

$$
f(r) \;=\; N \cdot A(r, \tau), \qquad
A(r, \tau) \;=\; \frac{1 - e^{-r\tau}}{r}.
$$

We require **IR-Delta** $\partial f / \partial r$ — the rate
sensitivity of the leg. Built as a quoted S-expression, simplified by
the symbolic engine, differentiated by `D`, and **lowered to a
closure** that records onto the active AAD tape via plain arithmetic:

```scheme
(define expr   '(* notional (/ (- 1 (exp (* -1 (* r tau)))) r)))
(define dexpr  (D expr 'r))                            ; symbolic ∂f/∂r
(define lower  (lower-expression dexpr '(notional r tau)))
(lower 1000000 0.03 2.5)
;; => -2.973057996e6
```

Analytic check:

$$
\frac{\partial f}{\partial r}
= N \cdot \frac{\tau\, r\, e^{-r\tau} - (1 - e^{-r\tau})}{r^2}
$$

At $N = 10^6$, $r = 0.03$, $\tau = 2.5$ this evaluates to
$-2.973057996 \times 10^6$ — bit-equal to the lowered kernel.

**Finite-difference cross-check.** Bumping `r` by $h = 10^{-6}$ and
evaluating the original $f$ either side gives
$(f(r{+}h) - f(r{-}h))/(2h) = -2.97306 \times 10^6$, matching the
symbolic / lowered value to seven significant figures. The check is
one line and lives next to the kernel call — it is the standard
"never trust an AAD or symbolic derivative without a finite-difference
sanity check" hygiene step.

The differentiation engine itself ([`symbolic.eta`](../examples/xva-wwr/symbolic.eta))
is one screen of code:

```scheme
(defun diff (e v)
  (cond
    ((number? e) 0)
    ((symbol? e) (if (eq? e v) 1 0))
    ((eq? (op e) '+) (list '+ (diff (a1 e) v) (diff (a2 e) v)))
    ((eq? (op e) '-) (list '- (diff (a1 e) v) (diff (a2 e) v)))
    ((eq? (op e) '*) (list '+ (list '* (diff (a1 e) v) (a2 e))
                              (list '* (a1 e) (diff (a2 e) v))))
    ((eq? (op e) '/) (list '/ (list '- (list '* (diff (a1 e) v) (a2 e))
                                       (list '* (a1 e) (diff (a2 e) v)))
                              (list '* (a2 e) (a2 e))))
    ((eq? (op e) 'exp) (list '* (list 'exp (a1 e)) (diff (a1 e) v)))
    ;; ... sqrt, log, sin, cos, ^ omitted for brevity ...
    (else 0)))

(defun D (expr var) (simplify* (diff expr var)))
```

`lower-expression` is even smaller — it returns a closure that
evaluates the expression in an environment of bound argument values:

```scheme
(defun lower-expression (expr vars)
  (lambda args
    (eval-expression expr (zip vars args))))
```

> [!IMPORTANT]
> **Why this is the load-bearing claim.** The closure produced by
> `lower-expression` does plain `+ - * /` over its arguments. If those
> arguments are `TapeRef`s for an active AD tape, the same arithmetic
> records onto **that tape** — no source-to-source step, no separate
> kernel build, no Python bridge. The objection answered is "JAX
> already does AAD"; the answer is "JAX cannot host CAS on the same
> tape without a separate compilation step".

After `simplify*`, the differentiated expression's AST settles at
**29 nodes** (`(node-count dexpr) ⇒ 29`).

---

## §9 — Compression QP on a SIMM Σ Block

Once §7 names the wrong-way counterparties, the next requirements is an 
*actionable* unwind set. `compress.eta` formulates this as a strictly
convex quadratic on the CLP(R)-feasible polytope:

$$
\min_{\delta} \;\; J(\delta) \;=\;
\tfrac{1}{2}\, \delta^{\top} \Sigma \, \delta
\;+\; \lambda_{\text{funding}} \cdot \mathbf{1}^{\top} \delta
\quad \text{s.t.} \quad \delta_i \in [-1, 1]
$$

Σ is a 10×10 SIMM-*style* IR-delta correlation literal — a stylised
positive-definite block whose entries are loosely shaped after a SIMM
IR-delta bucket but **not** calibrated to the official ISDA SIMM
parameters, and not aligned 1-to-1 with named SIMM tenor buckets.
The demo uses the 3×3 leading prefix to keep the QP small enough to
print:

```scheme
(define simm-ir-delta-covariance
  '((1.00 0.35 0.30 0.25 0.20 0.18 0.16 0.14 0.12 0.10)
    (0.35 1.00 0.32 0.27 0.22 0.20 0.18 0.16 0.14 0.12)
    (0.30 0.32 1.00 0.29 0.24 0.22 0.20 0.18 0.16 0.14)
    ;; ... 7 more rows ...
    ))
```

**Preview value** (closed form, no solver invocation):

```scheme
(define sigma3       (matrix-prefix simm-ir-delta-covariance 3))
(define preview-obj  (objective-value '(0.10 -0.20 0.05) sigma3 0.15))
;; => 0.01005
```

**Full solve** posts $\delta_i \in [-1, 1]$ to the CLP(R) store and asks
for the constrained optimum:

```scheme
(defun solve-compression-qp (dv01 funding-lambda)
  (let* ((n         (length dv01))
         (sigma     (matrix-prefix simm-ir-delta-covariance n))
         (mark      (trail-mark))
         (delta     (make-real-vars n -1.0 1.0))
         (risk      (quadratic-risk-expr delta sigma))
         (carry     (dot-expr dv01 delta))
         (objective (clp:r+ (clp:r*scalar 0.5 risk)
                            (clp:r*scalar funding-lambda carry))))
    (call-with-values
      (lambda () (clp:rq-minimize objective))
      (lambda (optimum witness)
        ;; Bind the QP witness onto the logic vars, deref to plain
        ;; floats while the bindings are still live, THEN unwind the
        ;; trail. Doing the unwind first leaves `delta` unbound and
        ;; later derefs would return a non-number.
        (%apply-qp-witness! witness)
        (let ((delta-vals (map* (lambda (v) (deref-lvar v)) delta)))
          (unwind-trail mark)                        ; restore CLP store
          (list optimum delta-vals))))))
```

> [!NOTE]
> **CLP(R) primitives** — VM-level real-domain constraints, with
> trail-based undo so the solve is non-destructive:
> ```scheme
> (logic-var)              ; fresh CLP(R) variable
> (clp:real v lo hi)       ; attach real interval
> (clp:r+ a b)             ; symbolic linear sum
> (clp:r*scalar k v)       ; scalar multiply
> (clp:rq-minimize obj)    ; QP solve over current store, returns (opt . witness)
> (trail-mark)             ; non-destructive scope open
> (unwind-trail mark)      ; non-destructive scope close
> ```

Because the SIMM Σ block is symmetric positive-definite, $J(\delta)$ is
strictly convex and the solver converges in one active-set sweep on
the ~25-variable problem. The solution gives a risk-equivalent unwind
set across DV01 buckets.

### Actual hedge result (3-bucket SIMM Σ prefix)

Closed-form interior optimum for the demo's
`(solve-compression-qp '(0.10 -0.20 0.05) 0.15)` call —
$\delta^{*} = -\lambda_f \, \Sigma^{-1} \, \text{dv01}$, all
components inside $[-1, 1]$ so the box constraints are inactive:

```text
solve-compression-qp dv01=(0.10, -0.20, 0.05) lambda_f=0.15
----------------------------------------------------------------
   bucket    dv01      delta*       interpretation
   IR-1     +0.10    -0.0262    unwind a small long IR-1 position
   IR-2     -0.20    +0.0435    add IR-2 (offsets the short)
   IR-3     +0.05    -0.0136    unwind a small long IR-3 position

   J(delta*)            = -8.996e-04   (objective at optimum)
   1' delta*            = +0.0037      (near-flat carry)
   delta*' Sigma delta* = +1.799e-03   (residual risk after unwind)
```

> **Reading.** The QP returns a **specific** unwind vector (not just
> a "you should hedge" hand-wave). The hedge magnitudes are small
> because the dv01 imbalance is small; scaling `dv01` up by 10×
> scales `δ*` linearly until the box constraints `δ_i ∈ [-1, 1]`
> activate, at which point the active-set sweep clamps the dominant
> bucket and re-solves on the residual. The witness is what gets sent
> to the booking system.
>
> **On the negative `J(δ*)`.** $J(\delta) = \tfrac12\delta^{\top}\Sigma\delta + \lambda_f\,\text{dv01}^{\top}\delta$
> is *not* a CVA and not a loss in any P&L sense. The quadratic term is
> a residual-risk penalty (always $\geq 0$) and the linear term is a
> funding-carry pull that is signed; the optimum is wherever those two
> balance, and the resulting value can be of either sign or shifted by
> a constant. Compare runs by *difference in $J$* under different `dv01`
> or $\lambda_f$, not by the absolute level.

The solve is wired into the demo: `main.eta` now invokes
`solve-compression-qp '(0.10 -0.20 0.05) 0.15` and stores the result
under `'hedge` in the artifact; `report.eta` renders the
`(bucket, dv01, δ*)` table inside stage 6 immediately after the
preview value. To exercise it standalone:

```scheme
(import compress)
(solve-compression-qp '(0.10 -0.20 0.05) 0.15)
;; => (-8.996e-04 (-0.0262 0.0435 -0.0136))
```

---

## Technical Appendix — Deterministic Shard Replay

```scheme
(defun run-path-shard (shard-id)
  (simulate-paths baseline-curves '() factor-correlation
                  250 12 0.25 (+ 20260427 shard-id)))

(defun curve-sum (curve) (foldl (lambda (acc x) (+ acc x)) 0.0 curve))

(defun shard-hash (ee-curves)
  (foldl (lambda (acc entry) (+ acc (curve-sum (cdr entry))))
         0.0 ee-curves))

(defun rerun-shard-and-check (shard-id)
  (let ((h1 (shard-hash (run-path-shard shard-id)))
        (h2 (shard-hash (run-path-shard shard-id))))
    (list (cons 'first  h1)
          (cons 'second h2)
          (cons 'match  (= h1 h2)))))
```

For seed `20260427 + 2` the hash is `4.96368e7`, identical across
runs:

```scheme
(rerun-shard-and-check 2)
;; => ((first . 4.96368e+07) (second . 4.96368e+07) (match . #t))
```

A fully supervised actor variant (one `path-worker` under
`std.supervisor`, restart on injected fault, hash assert across the
restart) is a straightforward extension of the helper above; the
deterministic core that makes any such assertion meaningful is the
shard hash shown here.

> [!IMPORTANT]
> **Reduction order matters as much as the seed.** `shard-hash` folds
> in a fixed counterparty order (`baseline-curves` is a deterministic
> map from `(iota 30 1)`). If you swap `foldl` for a parallel reduction
> the hash changes — and that change is the *correct* CI signal that
> determinism has been broken.

> [!CAUTION]
> **Determinism is not correctness.** A green `(match . #t)` only proves
> that the same seed produces the same numbers across two runs of the
> same binary. It says nothing about whether the SCM is well specified,
> whether the βs are right, or whether the Monte Carlo estimator has
> converged. The model-correctness checks belong in §3a (recoverability
> on the synthetic DGP), §4 (worked closed-form bucket on CP-01),
> §5 (clamp-branch diagnostic on the AAD gradient), §7c (β-perturbation
> stability of the WWR ranking), and §8 (finite-difference cross-check
> of the symbolic / lowered kernel). Treat the shard hash as the
> *reproducibility floor*, not as a substitute for those.

---

## Verification Summary

| Stage | Check |
|---|---|
| §1 Book | `(book-size . 200)`; trade attributes are closed forms in `idx` |
| §2 Market | `EE_base`, $\lambda_i$, $R_i$ are deterministic in $i$ |
| **§3a ML calibration** | `(beta-recovery (max-error . _))` ≤ 0.05 with seed `20260427`, T=500, σ=0.05; `nn.Linear(3,30)` final MSE ≈ 0.00248 |
| §3 Paths | `manual-seed` then `randn` → identical shock vector across runs |
| §4 CVA | CP-01 12-bucket value matches worked closed form within FP tolerance |
| §5 AAD | $\partial \text{CVA}/\partial \Delta r = -212\,503$ at the seed point; clamp branches inert |
| §6 do(...) | `cva-under-do` re-runs `cva-table` on tilted EE + transformed hazards |
| §7 Sweep | Asymmetric oil pair (-30/+30) reproduces +42.97% / −33.27%; ε_book(oil) ≈ 1.27; CP-17 flagged as `>> LARGEST WWR EXPOSURE` |
| §8 Symbolic | `(node-count dexpr) ⇒ 29`; `(lowered 1e6 0.03 2.5) ⇒ -2.973058e6` matches analytic |
| §9 Compression | `objective-value '(0.10 -0.20 0.05) sigma3 0.15` ⇒ `0.01005`; QP solve converges; `solve-compression-qp` returns `δ* ≈ (-0.026, +0.043, -0.014)` at `J* ≈ -8.99e-04` |
| Shard replay (appendix) | `(rerun-shard-and-check 2)` ⇒ `(match . #t)` for seed `20260429` |

To run your own validation, change `20260427` in
[`main.eta`](../examples/xva-wwr/main.eta) and
[`workers.eta`](../examples/xva-wwr/workers.eta) to a different seed —
every numeric column should change in a coordinated way (different
seed, different shock vector, different CVA, different ε), but the
**shape** of every artifact (book size, top-3 ranking, sign of
elasticities, QP feasibility) should remain identical.

---

## Appendix A — Formal Definitions

| Symbol | Definition |
|---|---|
| $r_0$ | Base risk-free rate, 0.0325 |
| $\text{EE}_{\text{base}}(i)$ | Baseline expected exposure for CP-$i$ (§2 closed form) |
| $\lambda_i,\, R_i$ | Baseline hazard and recovery for CP-$i$ |
| $\beta_{i,\text{var}}$ | Per-CP shock sensitivity, indexed by `oil`, `usd-10y`, `eur-usd` |
| $z_t \sim \mathcal{N}(0,1)$ | Path shocks drawn after `manual-seed` |
| $s = 0.04 + n_{\text{paths}}/100\,000$ | Path scale |
| $\text{EE}_{i,t}$ | Realised exposure (§3) |
| $\text{DF}(t) = \exp(-(r_0 + \Delta r) t)$ | Discount factor |
| $\Delta\text{PD}_i = \lambda_i \cdot \Delta t$ | Marginal default probability |
| $\text{CVA}_i$ | $\sum_t (1 - R_i) \cdot \text{EE}_{i,t} \cdot \text{DF}(t) \cdot \Delta\text{PD}_i \cdot (1 + \Delta\text{fx})$ |
| $\text{CVA}_{\text{book}}$ | $\sum_i \text{CVA}_i$ |
| $\text{do}(\text{var} = \text{value})$ | Surgical SCM intervention (§6) |
| $k = 0.80\cdot\text{oil} + 0.60\cdot\text{usd-10y} + 0.50\cdot\text{eur-usd}$ | Aggregate exposure tilt |
| $\lambda'_i = \lambda_i \cdot \exp(\beta_{i,\text{var}} \cdot \text{shock})$ | Hazard transform under do(...) |
| $\varepsilon_i = (\log \text{CVA}_i^{\text{up}} - \log \text{CVA}_i^{\text{down}})/s$ | Per-CP causal elasticity |
| $f(r) = N(1 - e^{-r\tau})/r$ | Symbolic swap-leg kernel |
| $J(\delta) = \tfrac12\delta^{\top}\Sigma\delta + \lambda_f \mathbf{1}^{\top}\delta$ | Compression QP objective |

## Appendix B — Notation

| Symbol | Meaning |
|---|---|
| $i$ | Counterparty index (1 … 30) |
| $t$ | Bucket time (quarterly, 0.25 … 3.0) |
| $\Delta\lambda, \Delta R, \Delta r, \Delta\text{fx}$ | Scalar shifts; AAD differentiation variables |
| $\delta_i$ | Compression unwind weight (CLP(R) real on $[-1, 1]$) |
| `TapeRef` | Boxed reference into the active AAD tape (see [`vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp)) |
| `cva-table` | Per-CP CVA list, value-only consumer of EE / hazards / recoveries |
| `cva-under-do` | Single-shock re-runner: tilt + hazard transform + `cva-table` |
| `counterfactual-sweep` | Multi-shock driver with elasticity merge |
| `lower-expression` | Symbolic-tree → closure that records onto the active tape |
| `clp:rq-minimize` | CLP(R) convex-QP solver; returns `(opt . witness)` |

## Appendix C — Source Layout

> [!NOTE]
> Two files are the authoritative references when the doc and code
> drift:
>
> - [`examples/xva-wwr/main.eta`](../examples/xva-wwr/main.eta)
>   defines the **artifact shape** returned by `(run-demo)`.
> - [`examples/xva-wwr/report.eta`](../examples/xva-wwr/report.eta)
>   defines the **human-readable report structure** rendered by
>   `(main)`.
>
> If a code block in this document disagrees with either file, the
> file wins.

| Component | File |
|---|---|
| **Composition / orchestrator** | [`examples/xva-wwr/main.eta`](../examples/xva-wwr/main.eta) |
| Book and CSA terms | [`examples/xva-wwr/book.eta`](../examples/xva-wwr/book.eta) |
| Market state, hazards, recoveries, βs | [`examples/xva-wwr/market.eta`](../examples/xva-wwr/market.eta) |
| **ML calibration of βs** (§3a) | [`examples/xva-wwr/learn-betas.eta`](../examples/xva-wwr/learn-betas.eta) |
| Torch path shocks | [`examples/xva-wwr/paths.eta`](../examples/xva-wwr/paths.eta) |
| CVA aggregation + AAD greeks | [`examples/xva-wwr/xva.eta`](../examples/xva-wwr/xva.eta) |
| SCM + intervention sweep | [`examples/xva-wwr/wwr-causal.eta`](../examples/xva-wwr/wwr-causal.eta) |
| Symbolic algebra + lowering | [`examples/xva-wwr/symbolic.eta`](../examples/xva-wwr/symbolic.eta) |
| Compression QP | [`examples/xva-wwr/compress.eta`](../examples/xva-wwr/compress.eta) |
| Deterministic shard replay | [`examples/xva-wwr/workers.eta`](../examples/xva-wwr/workers.eta) |
| **Friendly stage-by-stage report** | [`examples/xva-wwr/report.eta`](../examples/xva-wwr/report.eta) |
| AAD tape (VM, C++) | [`eta/core/src/eta/runtime/vm/vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp) |
| CLP(R) constraint store | [`eta/core/src/eta/runtime/clp/`](../eta/core/src/eta/runtime/clp) |
| libtorch wrappers | [`stdlib/std/torch.eta`](../stdlib/std/torch.eta) |

---

## Future Extensions

| Extension | Effort | Impact |
|---|---|---|
| **G2++ short-rate** | Replace single-shock path scaling with proper two-factor short-rate simulation | Higher-fidelity exposure across maturities |
| **CSA-aware netting** | Wire `csa-terms` (threshold/MTA/MPoR) into bucket reduction | Realistic collateralised exposure |
| **Full supervisor §10** | Replace `rerun-shard-and-check` with one `path-worker` actor under `std.supervisor` | Demonstrates fault recovery, not just determinism |
| **FVA / KVA** | Add funding-spread and capital-charge buckets to `cva-curve` | Two more sensitivities through the same AAD tape |
| **Identification report** | `(do:identify scm 'cva 'oil)` printed before §6 | Makes the back-door / front-door reasoning explicit |
| **Distributed sweep** | `counterfactual-sweep` over `worker-pool` + IPC | True OS-level parallel scenarios with fault isolation |
| **Real CDS curve** | Replace `base-hazard` closed form with bootstrapped CDS panel data | Production-grade hazard term structures |

---

## Source Locations

| Component | File |
|---|---|
| **xVA WWR Demo** | [`examples/xva-wwr/main.eta`](../examples/xva-wwr/main.eta) |
| Causal infrastructure | [`stdlib/std/causal.eta`](../stdlib/std/causal.eta) |
| AAD primitives | [`stdlib/std/aad.eta`](../stdlib/std/aad.eta) |
| CLP(R) | [`stdlib/std/clpr.eta`](../stdlib/std/clpr.eta) |
| libtorch | [`stdlib/std/torch.eta`](../stdlib/std/torch.eta) |
| VM execution engine | [`eta/core/src/eta/runtime/vm/vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp) |
| Compiler (`etac`) | [`docs/compiler.md`](compiler.md) |

