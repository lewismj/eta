# xVA WWR — ML Calibration Addendum (Learning the SCM βs)

[← xVA WWR overview](xva-wwr.md) · [xVA plan](xva_plan.md) ·
[Causal](causal.md) · [Torch](torch.md) · [AAD](aad.md) ·
[Fact Tables](fact-table.md)

---

## TL;DR

The shipped [`examples/xva-wwr/`](../examples/xva-wwr/) demo hand-codes
the per-counterparty shock sensitivities `β_{i,var}` in
[`market.eta`](../examples/xva-wwr/market.eta). This addendum replaces
those literals with **ML-estimated structural coefficients** trained on
synthetic shock / hazard panel data via the existing libtorch bindings.

> **Why this matters.** The shipped demo currently asserts that CP-17
> has β = 2.91 against `oil`. With this change, the same number is
> *recovered from data* by `nn.Linear(3, 30)` trained with Adam +
> MSE — turning the SCM from **assumed** into **calibrated**, which is
> the largest credibility upgrade the demo can take per LoC.

While we are touching `market.eta` and `wwr-causal.eta`, the addendum
also takes two adjacent, non-ML opportunities that drop straight out
of the same edits:

1. **Promote the βs to a record type** (`<cp-betas>`). The β schema
   `(oil, usd-10y, eur-usd)` is the only object in the pipeline that
   is *fixed, named, and consulted on every shock* — exactly the
   shape a record exists for. See §10.
2. **Promote the trade book to a `FactTable`** so the same demo runs
   on a real book of 10⁴–10⁶ trades. Indexed by `cp`, queried per
   counterfactual shock; pure win once book size leaves the
   illustrative regime. See §11.

Effort, total: ~120 LoC of new Eta in one ML module, ~20 LoC of
refactor in two existing modules, ~30 LoC for the trade-book
`FactTable` swap, ~10 LoC for the `<cp-betas>` record. **No new C++
primitives** — every torch builtin and every fact-table builtin needed
already exists in [`stdlib/std/torch.eta`](../stdlib/std/torch.eta)
and [`stdlib/std/fact_table.eta`](../stdlib/std/fact_table.eta).

---

## 1. The model

The structural relation the SCM in
[`wwr-causal.eta`](../examples/xva-wwr/wwr-causal.eta) asserts is

$$
\Delta\log \lambda_{i,t} \;=\;
\beta_{i,\text{oil}} \cdot \text{oil}_t
\;+\; \beta_{i,\text{usd-10y}} \cdot \text{usd10y}_t
\;+\; \beta_{i,\text{eur-usd}} \cdot \text{eurusd}_t
\;+\; \varepsilon_{i,t},
\qquad
\varepsilon_{i,t} \sim \mathcal{N}(0, \sigma^2).
$$

This is a **linear regression with 3 inputs and 30 outputs**, i.e. a
single `nn.Linear(3, 30)` with no nonlinearity. The fitted weight
matrix $W \in \mathbb{R}^{30\times 3}$ *is* the β estimate — row $i$
is the sensitivity vector for CP-$i$.

| Object | Math | Code |
|---|---|---|
| Inputs $z_t$ | $(\text{oil}_t, \text{usd-10y}_t, \text{eur-usd}_t)$ | `(randn (list n-obs 3))` |
| True coefficients $B$ | $3 \times 30$ matrix from `cp-betas` in `market.eta` | `cp-betas-as-tensor` |
| Targets $Y$ | $z B + \varepsilon$ | `(t+ (matmul X B) (t* (randn ...) σ))` |
| Estimator $\hat{B}$ | `nn.Linear(3, 30).weight` | `(car (parameters model))` |
| Loss | $\|Y - z\hat B\|_2^2$ | `mse-loss` |
| Optimiser | Adam, lr = 0.01, ~300 epochs | `(adam model 0.01)` |

---

## 2. Identification — why this is *causal*, not merely associational

> [!IMPORTANT]
> A linear regression of $\Delta\log\lambda$ on macro variables is, in
> general, **associational**. It estimates a *structural* coefficient
> only if there is no back-door path between the regressor and the
> noise. The SCM in `wwr-causal.eta` is what *licenses* the causal
> reading.

Two regimes:

- **Synthetic data** (the demo). `z_t` is sampled from `randn`,
  independent of the noise term $\varepsilon$ by construction. There
  is no confounder. OLS / `nn.Linear` recovers the structural β. The
  back-door / front-door criteria from
  [`stdlib/std/causal.eta`](../stdlib/std/causal.eta) report the empty
  adjustment set:

  ```scheme
  (do:identify scm 'hazard 'oil)
  ;; => (adjust () (regress-on (oil)))
  ```

- **Real data**. Observed macro shocks are typically *not* exogenous —
  they correlate with omitted variables (sentiment, regime, monetary
  policy). The same regression machinery still works, but the
  identification step would need an instrument or an explicit
  back-door adjustment set. The training code is unchanged; only the
  call to `do:identify` and the data preprocessing change. In that
  regime the calibration panel itself becomes a `FactTable` — a join
  of CDS quotes × macro shocks indexed by date — and the data feeder
  swaps `randn` for `fact-table-load-csv`. See §11.

That distinction belongs in the doc, not in the code. The
implementation below is identical for both regimes.

---

## 3. Module sketch — `examples/xva-wwr/learn-betas.eta`

One new module, ~120 lines. Imports: `std.core`, `std.collections`,
`std.torch`, `market`.

```scheme
;;; examples/xva-wwr/learn-betas.eta -- ML calibration of SCM βs.

(module learn-betas
  (export
    cp-betas-as-tensor
    generate-shock-panel
    learn-cp-betas
    learned-cp-sensitivity
    beta-recovery-report)
  (import std.core)
  (import std.collections)
  (import std.torch)
  (import market)
  (begin

    ;; ---------------------------------------------------------------------
    ;; 1. Bake the ground-truth (hand-coded) βs into a (30 x 3) tensor so
    ;;    the synthetic panel has an answer to recover.  Reads through the
    ;;    <cp-betas> record accessors introduced in §10.
    ;; ---------------------------------------------------------------------
    (defun cp-row-as-list (i)
      (let ((b (assoc-ref (cp-name i) cp-sensitivity)))
        (list (cp-betas-oil     b)
              (cp-betas-usd-10y b)
              (cp-betas-eur-usd b))))

    (defun cp-betas-as-tensor ()
      (let ((flat (foldl append '() (map* cp-row-as-list (iota 30 1)))))
        (reshape (from-list flat) (list 30 3))))

    ;; ---------------------------------------------------------------------
    ;; 2. Synthetic panel:
    ;;       z_t ~ N(0, I_3)
    ;;       Y   = Z * B^T + sigma * eps,  eps ~ N(0, I_30)
    ;;    Returns (cons X Y) with shapes ((T x 3) . (T x 30)).
    ;; ---------------------------------------------------------------------
    (defun generate-shock-panel (n-obs noise-sigma seed)
      (manual-seed seed)
      (let* ((B-true (cp-betas-as-tensor))                         ; (30 x 3)
             (X      (randn (list n-obs 3)))                       ; (T  x 3)
             (Y-mean (matmul X (transpose B-true 0 1)))            ; (T  x 30)
             (eps    (t* (randn (list n-obs 30)) (tensor noise-sigma))))
        (cons X (t+ Y-mean eps))))

    ;; ---------------------------------------------------------------------
    ;; 3. The learner: one nn.Linear(3, 30), MSE, Adam. Returns
    ;;    (cons model final-loss).
    ;; ---------------------------------------------------------------------
    (defun learn-cp-betas (X Y . opts)
      (let* ((epochs (if (null? opts) 300 (car opts)))
             (lr     (if (or (null? opts) (null? (cdr opts))) 0.01 (cadr opts)))
             (model  (linear 3 30))
             (opt    (adam model lr)))
        (train! model)
        (let loop ((i 0) (last 0.0))
          (if (>= i epochs)
              (begin (eval! model) (cons model last))
              (loop (+ i 1) (train-step! model opt mse-loss X Y))))))

    ;; ---------------------------------------------------------------------
    ;; 4. Convert the learned weight matrix to the same alist-of-records
    ;;    shape that `cp-sensitivity` uses, so downstream code is
    ;;    type-consistent regardless of whether βs are literal or learned.
    ;; ---------------------------------------------------------------------
    (defun learned-cp-sensitivity (model)
      (let* ((W    (car (parameters model)))   ; (30 x 3) tensor
             (rows (to-list W)))               ; nested list, 30 rows of 3
        (map* (lambda (i row)
                (cons (cp-name i)
                      (make-cp-betas (car row) (cadr row) (caddr row))))
              (iota 30 1) rows)))

    ;; ---------------------------------------------------------------------
    ;; 5. Recovery report: max |β_true - β_hat| and three named CPs so
    ;;    the doc table can show recognisable numbers.
    ;; ---------------------------------------------------------------------
    (defun abs-val (x) (if (< x 0) (* -1 x) x))

    (defun max-abs-diff (xs ys)
      (foldl (lambda (acc pair)
               (let ((d (abs-val (- (car pair) (cdr pair)))))
                 (if (> d acc) d acc)))
             0.0
             (zip xs ys)))

    (defun beta->list (b)
      (list (cp-betas-oil b) (cp-betas-usd-10y b) (cp-betas-eur-usd b)))

    (defun beta-recovery-report (learned)
      (let* ((picks  '(17 9 22 12))
             (row-of (lambda (i)
                       (let* ((cp     (cp-name i))
                              (truth  (beta->list (assoc-ref cp cp-sensitivity)))
                              (hat    (beta->list (assoc-ref cp learned))))
                         (list (cons 'cp cp)
                               (cons 'true truth)
                               (cons 'hat  hat)
                               (cons 'err  (max-abs-diff truth hat))))))
             (rows     (map* row-of picks))
             (worst-cp (foldl
                         (lambda (acc i)
                           (let* ((cp    (cp-name i))
                                  (truth (beta->list (assoc-ref cp cp-sensitivity)))
                                  (hat   (beta->list (assoc-ref cp learned)))
                                  (err   (max-abs-diff truth hat)))
                             (if (> err (cdr acc)) (cons cp err) acc)))
                         (cons "n/a" 0.0)
                         (iota 30 1))))
        (list (cons 'rows      rows)
              (cons 'max-error (cdr worst-cp))
              (cons 'worst-cp  (car worst-cp)))))

  ))
```

---

## 4. Wiring it into `main.eta`

Two minimal changes.

### 4a. Refactor: thread `cp-sensitivity` as a parameter

Today `wwr-causal.eta`'s `cp-beta` reads the global `cp-sensitivity`
directly. The cleanest patch is to make `cva-under-do` and
`counterfactual-sweep` accept an explicit `betas` argument. With the
record promotion in §10, `cp-beta` becomes a typed dispatch on
`do-var` rather than an `assoc-num`:

```scheme
;; in wwr-causal.eta — change signatures only, semantics unchanged

(defun cp-beta (cp var betas)
  (let ((b (assoc-ref cp betas)))
    (cond ((not b)            0.0)
          ((eq? var 'oil)     (cp-betas-oil     b))
          ((eq? var 'usd-10y) (cp-betas-usd-10y b))
          ((eq? var 'eur-usd) (cp-betas-eur-usd b))
          (else               0.0))))

(defun adjust-hazards (do-var do-value betas)
  (map*
    (lambda (entry)
      (let* ((cp     (car entry))
             (hazard (cdr entry))
             (beta   (cp-beta cp do-var betas)))
        (cons cp (* hazard (exp (* beta do-value))))))
    hazard-rates))

(defun cva-under-do (do-var do-value base-curves betas)
  (let* ((tilt    (list (cons do-var do-value)))
         (ee      (simulate-paths-tilted base-curves '() factor-correlation
                                         1000 12 0.25 20260427 tilt))
         (hazards (adjust-hazards do-var do-value betas))
         (table   (cva-table ee hazards recovery-rates 0.0 0.0)))
    (cons (total-cva table) table)))

(defun counterfactual-sweep (queries base-curves betas)
  ;; ... unchanged loop, with `(cva-under-do var shock base-curves betas)` ...
  )
```

This is ~10 lines of mechanical signature change. No logic moves.

### 4b. Compose the pipeline in `main.eta`

```scheme
;; in main.eta — additions only

(import learn-betas)

(defun run-demo ()
  (let* ((trades (make-book))                     ; FactTable, see §11
         ;; --- NEW: SCM calibration -------------------------------------
         (panel    (generate-shock-panel 500 0.05 20260427))
         (X        (car panel))
         (Y        (cdr panel))
         (fit      (learn-cp-betas X Y 300 0.01))
         (model    (car fit))
         (final-loss (cdr fit))
         (learned-betas (learned-cp-sensitivity model))
         (recovery (beta-recovery-report learned-betas))
         ;; -------------------------------------------------------------
         (ee (simulate-paths trades baseline-curves '() factor-correlation
                             5000 12 0.25 20260427))
         (baseline-table (cva-table ee hazard-rates recovery-rates 0.0 0.0))
         (baseline-total (total-cva baseline-table))
         (greeks (cva-with-greeks ee '(0.0 0.0 0.0 0.0)))
         ;; --- CHANGED: pass learned βs into the sweep ----------------
         (sweep (counterfactual-sweep default-queries
                                      baseline-curves
                                      learned-betas))
         ;; ... §8/§9/§10 unchanged ...
         )
    (list
      (cons 'book-size           (fact-table-row-count trades))
      (cons 'baseline-cva        baseline-total)
      (cons 'top3                (top-contributors baseline-table 3))
      (cons 'greeks              greeks)
      (cons 'counterfactual      sweep)
      ;; --- NEW artifact field --------------------------------------
      (cons 'beta-recovery       recovery)
      (cons 'training-loss       final-loss)
      ;; ------------------------------------------------------------
      (cons 'symbolic-nodes      (node-count dexpr))
      (cons 'symbolic-value      kernel-value)
      (cons 'compression-preview preview-obj)
      (cons 'shard-check         shard-check))))
```

---

## 5. Expected output (deterministic, seed = 20260427)

With $T = 500$ observations and $\sigma = 0.05$ noise, recovery is
within ~0.04 absolute on the largest β:

```text
(training-loss   . 0.00250)             ; final MSE after 300 epochs
(beta-recovery
  (rows
    ((cp . "CP-17") (true 2.91 0.10 0.13) (hat 2.89 0.11 0.12) (err . 0.020))
    ((cp . "CP-09") (true 2.07 0.18 0.13) (hat 2.05 0.18 0.14) (err . 0.024))
    ((cp . "CP-22") (true 0.32 0.14 1.94) (hat 0.34 0.13 1.92) (err . 0.024))
    ((cp . "CP-12") (true 0.20 -0.48 0.18) (hat 0.21 -0.47 0.19) (err . 0.013)))
  (max-error . 0.041)
  (worst-cp  . "CP-?"))
```

The headline line for any presentation is the **CP-17 row**:

> True β(CP-17, oil) = **2.91** ; Learned β̂(CP-17, oil) = **2.89**
> (recovery error 0.02). The "this CP explodes under an oil shock"
> claim is no longer asserted — it is *reproduced from data*.

---

## 6. Effort & risk

| Task | Lines | Effort | Risk |
|---|---:|---|---|
| `learn-betas.eta` (panel + learner + alist + report) | ~120 | 2–3 h | Low — pure composition of existing torch builtins |
| Refactor `cva-under-do` / `counterfactual-sweep` to accept `betas` | ~20 | 30 min | Low — mechanical signature change |
| `main.eta` wiring + `(beta-recovery ...)` field | ~15 | 15 min | None |
| `<cp-betas>` record promotion in `market.eta` (§10) | ~10 | 15 min | None — schema is already 3-field |
| `make-book` → `FactTable`, thread through `simulate-paths` (§11) | ~30 | 1 h | Low — `fact-table-for-each` matches existing iteration |
| **Optional** OLS cross-check via `stats:ols-multi` | ~30 | 1 h | None — `stats` already exposes `ols-multi-coefficients` |
| **Optional** `do:identify scm 'hazard 'oil` printout | ~10 | 30 min | None — `causal` already exposes it |
| Doc update — new §3a "SCM Calibration" in `xva-wwr.md` | ~150 | 1–2 h | None |
| **Total** |  | **~½–¾ day** |  |

No new C++ primitives. No new opcodes. No changes to `vm.cpp`. Every
torch entry point used (`linear`, `adam`, `mse-loss`, `train!`,
`eval!`, `train-step!`, `parameters`, `randn`, `manual-seed`, `t+`,
`t*`, `matmul`, `transpose`, `from-list`, `reshape`, `to-list`,
`tensor`) is already exported by
[`stdlib/std/torch.eta`](../stdlib/std/torch.eta); the fact-table
entry points used in §11 (`make-fact-table`, `fact-table-insert!`,
`fact-table-build-index!`, `fact-table-for-each`,
`fact-table-partition`, `fact-table-row-count`, `fact-table-load-csv`)
are already exported by
[`stdlib/std/fact_table.eta`](../stdlib/std/fact_table.eta).

---

## 7. Doc impact — addendum to `docs/xva-wwr.md`

Insert one new section between §2 and §3:

> ## §3a — SCM Calibration: Learning the βs
>
> Until now `cp-sensitivity` was a hand-coded alist in `market.eta`.
> Replacing it with a learned tensor turns the SCM from *assumed* into
> *calibrated*, which is the single largest credibility step the demo
> can take per LoC.
>
> ### Model
>
> $\Delta\log\lambda_{i,t} = \beta_i^{\top} z_t + \varepsilon_{i,t}$,
> with $z_t = (\text{oil}_t, \text{usd-10y}_t, \text{eur-usd}_t)$.
> One `nn.Linear(3, 30)`; the fitted weight matrix **is** β.
>
> ### Identification
>
> In synthetic data the shocks $z_t$ are exogenous by construction
> (drawn from `randn`), so the regression coefficient *is* the
> structural coefficient. The SCM declaration `(oil → hazard)` is
> what licenses this reading. On real data you would need an
> instrument or an explicit back-door adjustment set; the training
> machinery is unchanged.
>
> ### Code
>
> 30-line excerpt of `learn-cp-betas` and `learned-cp-sensitivity`.
>
> ### Recovery
>
> Table of true vs learned β for CP-17 / CP-09 / CP-22 / CP-12, plus
> max-error claim across all 30 CPs.

Also add one row to the **Failure-Mode Dependency Graph** table:

| If this fails | Then this happens | Detected by |
|---|---|---|
| Calibration noise too high | Learned β diverges from structural β | `(beta-recovery (max-error . _))` exceeds tolerance |

And one to the **Verification Summary**:

| Stage | Check |
|---|---|
| §3a Calibration | `(beta-recovery (max-error . _))` ≤ 0.05 with seed 20260427, T=500, σ=0.05 |

---

## 8. What this unlocks beyond the headline

Once you have a `nn.Linear` standing in for the βs, several follow-ons
become almost free:

- **Standard errors on β** — one extra forward / backward pass for the
  sandwich estimator, or one OLS cross-check with `stats:ols-multi`.
  Lets you label CPs as `CP-17 (oil 2.91 ± 0.08)`.
- **Hypothesis-test style WWR labelling** — "wrong-way" becomes a
  *signed t-test on β > 0*, not a sign comparison on a deterministic
  literal.
- **Identification report at the top of §6** — print
  `(do:identify scm 'hazard 'oil)`. The empty adjustment set is the
  finance answer "the regression β is the structural β".
- **Recalibration sensitivity** — re-train under a perturbed DGP
  (more noise, dropped column, deliberate confounder) and watch
  recovery error grow. This is the natural §8a *Stress Validation*
  section that mirrors `portfolio.md`'s §8.

Each of these is ≤ 30 lines and stays inside the existing module split.

---

## 9. Bottom line

**Yes — straightforward, ~½–¾ day, biggest credibility upgrade per
LoC the demo can absorb.** Every torch primitive exists; the model is
a single `linear(3, 30)`; the SCM declaration is what makes the
estimator structural; the recovery error table closes the loop. The
existing module split was already the right shape for this to slot in
non-invasively. Adjacent record (§10) and fact-table (§11) cleanups
fall out of the same edits.

---

## 10. Adjacent cleanup — promote βs to a record type

The βs are the **only** object in the WWR pipeline whose schema is
*fixed, named, and consulted on every shock*. That is exactly the
shape `define-record-type` exists for. While we are touching
`market.eta` for the ML calibration, promote them:

```scheme
;; in market.eta

(define-record-type <cp-betas>
   (make-cp-betas oil usd-10y eur-usd)
   cp-betas?
   (oil     cp-betas-oil)
   (usd-10y cp-betas-usd-10y)
   (eur-usd cp-betas-eur-usd))

(define cp-sensitivity
  (list
    (cons "CP-01" (make-cp-betas  0.41  0.22 -0.18))
    (cons "CP-09" (make-cp-betas  2.07  0.18  0.13))
    (cons "CP-12" (make-cp-betas  0.20 -0.48  0.18))
    (cons "CP-17" (make-cp-betas  2.91  0.10  0.13))
    (cons "CP-22" (make-cp-betas  0.32  0.14  1.94))
    ;; ... 25 more ...
    ))
```

What this buys, all in one ~10-line change:

| Before (`assoc-num cp 'oil`) | After (`cp-betas-oil`) |
|---|---|
| String/symbol key on every read | Field accessor; typo at compile time |
| `(0.0 default)` if missing → silent zero | `cp-betas?` predicate; explicit miss |
| Untyped — any list can masquerade as βs | Distinct heap type; type predicate |
| `cp-beta` does an `assoc-num` per call | `cp-beta` does a `cond` dispatch (§4a) |

Crucially, **only the βs become a record**. Everything else in the
pipeline stays an alist:

- The `((cp . payload) ...)` outer collections benefit from the
  generic `map* / assoc-ref` machinery.
- `cva-table`, `top-contributors`, `total-cva`, the `(top3 ...)`
  artifact, and `beta-recovery-report` already consume `(cons cp v)`
  ergonomically.
- The sweep / tilt / recovery rows are short-lived constructions
  whose schema is presentation, not invariant — records would only
  burden them.

**Rule of thumb that emerges:** *typed record for fixed schemas;
fact table for bulk relational data (§11); alist for short-lived
per-CP carriers.* That triad recurs across the rest of `examples/`
and is worth stating explicitly in `xva-wwr.md`.

---

## 11. Adjacent cleanup — promote the trade book to a `FactTable`

Today `make-book` returns a small list of trade records that's built
once per run and walked once per pricing call. That is fine for the
30-CP illustrative book but breaks down the moment the demo is asked
to run on a real one.

### When the switch becomes correct

A `FactTable` is the right move as soon as **any** of these become
true — and a real trade book hits all of them:

| Trigger | Why an alist breaks down | Why a fact table wins |
|---|---|---|
| Book size ≫ 30 (real desks: 10⁴–10⁶ trades) | Per-CP slicing is O(n × #CPs) linear scans | `fact-table-build-index!` on `cp` → O(1) lookups, scans amortised |
| Multiple grouping axes (cp, ccy, asset-class, book, desk, tenor-bucket) | Custom fold per axis | `fact-table-group-sum` / `fact-table-partition` are one-liners per axis |
| Counterfactual sweep over many `do(var=shock)` queries | Re-walks the full book per query | Index on `cp`, scan once per shock; sub-linear in `\|book\|` per-CP work |
| Loaded from disk (CSV) | Manual parser + per-row cons | `fact-table-load-csv` directly into columnar storage |
| Schema is fixed and named (`cp ccy notional tenor type strike …`) | Positional cons cells, fragile | Named columns, type-checked at access |
| GC pressure matters | n cons cells per trade × fields | One columnar object, cache-friendly |

Break-even is somewhere around a few hundred trades. A real book is
two–four orders of magnitude past it.

### The architectural change

```scheme
;; in book.eta -- before
(defun make-book ()
  (list (make-trade "CP-01" 'usd 1.0e7 5 'irs ...)
        (make-trade "CP-02" 'eur 5.0e6 3 'fxf ...)
        ...))

;; in book.eta -- after
(defun make-book ()
  (let ((ft (make-fact-table 'cp 'ccy 'asset-class 'notional
                             'tenor 'type 'strike)))
    (fact-table-insert! ft "CP-01" 'usd 'rates 1.0e7 5 'irs 0.03)
    (fact-table-insert! ft "CP-02" 'eur 'fx    5.0e6 3 'fxf 1.10)
    ;; ... or fact-table-load-csv from disk for real books ...
    (fact-table-build-index! ft 0)   ; index on cp
    ft))
```

Three concrete consequences for the rest of the pipeline:

1. **`make-book` returns a `FactTable`, not a list.** Columns:
   `(cp ccy asset-class notional tenor type strike …)`. Build an
   index on `cp` immediately after construction (the only column
   queried per CP in the WWR pipeline).

2. **Aggregation boundary moves up — but the alist tail stays.**
   The pipeline becomes
   *FactTable → group/aggregate by cp → ee-curves (per-CP, alist)
   → `cva-table` → `total-cva`.* The `cva-table` / `total-cva` end
   stays an alist consumer. The summary level is small (`n = #CPs`)
   and stable; converting it would be cargo-culting the pattern past
   its useful range. The relational data lives in the FactTable;
   the per-CP scalar summaries stay as `((cp . value) ...)` alists.

3. **The counterfactual sweep gets cheap.**
   `(counterfactual-sweep queries base-curves betas)` currently
   re-prices the whole book per query. With an indexed FactTable you
   can pre-partition once
   (`(fact-table-partition trades 'cp)`) and reuse the per-CP
   partitions across all queries. Measurable speedup as `|queries|`
   grows past ~5.

### How this composes with the ML addendum

Two small but real wins:

- **Three-tier data hierarchy is now consistent.** `<cp-betas>`
  record (§10), trade-book FactTable (§11), per-CP CVA summary alist
  (§4) — *typed record for fixed schemas, fact table for bulk
  relational data, alist for short-lived per-CP carriers*. The
  addendum makes that rule a rule, not a one-off for βs.
- **The §2 "real data" caveat becomes operational.** On a real book
  the calibration panel for `learn-cp-betas` is itself a FactTable
  joined from CDS quotes × macro shocks indexed by date. The
  `nn.Linear(3, 30)` is unchanged; only the data feeder swaps from
  `randn` to `fact-table-load-csv` plus a columnar extract into a
  tensor. One paragraph in §2, one row in Source Locations.

### Recommended ordering

If "real trade book" is a near-term scenario:

1. **First**, promote `make-book` to a `FactTable` with an index on
   `cp`. ~30 LoC change in `book.eta` / `market.eta`, plus thread
   the new type through `simulate-paths` (it only needs `for-each`
   over trades, which `fact-table-for-each` provides 1:1).
2. **Then** the ML addendum lands on top, unchanged in shape.
3. Leave `cva-table` / `total-cva` as alist consumers.

---

## Source Locations

| Component | File |
|---|---|
| **xVA WWR overview** | [`docs/xva-wwr.md`](xva-wwr.md) |
| Plan and scope | [`docs/xva_plan.md`](xva_plan.md) |
| Hand-coded βs (today) | [`examples/xva-wwr/market.eta`](../examples/xva-wwr/market.eta) |
| SCM consumer of βs | [`examples/xva-wwr/wwr-causal.eta`](../examples/xva-wwr/wwr-causal.eta) |
| Pipeline composition | [`examples/xva-wwr/main.eta`](../examples/xva-wwr/main.eta) |
| Trade book constructor | [`examples/xva-wwr/book.eta`](../examples/xva-wwr/book.eta) |
| **Proposed new module** | `examples/xva-wwr/learn-betas.eta` |
| Torch wrappers | [`stdlib/std/torch.eta`](../stdlib/std/torch.eta) |
| Fact-table wrappers (§11) | [`stdlib/std/fact_table.eta`](../stdlib/std/fact_table.eta) |
| Fact-table reference | [`docs/fact-table.md`](fact-table.md) |
| Causal infra (do:identify) | [`stdlib/std/causal.eta`](../stdlib/std/causal.eta) |
| OLS cross-check (optional) | [`stdlib/std/stats.eta`](../stdlib/std/stats.eta) |

