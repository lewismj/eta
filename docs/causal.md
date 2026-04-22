# Causal Inference — Do-Calculus & Factor Analysis

[← Back to README](../README.md) · [Logic Programming](logic.md) ·
[CLP](clp.md) · [AAD](aad.md) · [xVA](xva.md) · [Project Status](next-steps.md)

---

## Overview

This page describes Eta's causal inference layer: the `std.causal` module
and the `examples/do-calculus/` and `examples/causal-factor/` example
programs.

The layer combines:

- **`std.logic`** — structural unification and backtracking (symbolic
  reasoning over causal graphs)
- **`std.clp`** — constraint logic programming (bounding probability
  estimands, feasibility checks)
- **`std.causal`** — DAG utilities + Pearl's do-calculus engine

The finance-oriented running example estimates the **causal effect of
market-beta exposure on excess stock returns**, adjusting for sector
membership as an observed confounder.

---

## Finance Motivation

Quantitative finance asks causal questions that cannot be answered by
correlation alone.  For example:

> *"Does increasing a portfolio's market-beta exposure actually cause
>  higher excess returns — or does the apparent relationship simply reflect
>  the fact that high-beta stocks are concentrated in high-return sectors?"*

A naive OLS regression of `stock-return` on `market-beta` conflates two
distinct paths:

```
sector ──→ market-beta ──→ stock-return     (causal path)
sector ──────────────────→ stock-return     (direct sector effect)
```

`sector` is a **confounder**: it influences both the exposure
(`market-beta`) and the outcome (`stock-return`), creating a spurious
association.  Pearl's **do-calculus** provides a formal procedure to
identify the *interventional* distribution `P(return | do(beta))` — the
return we would observe if we *set* beta externally — from purely
*observational* data.

---

## The Causal DAG

```scheme
(define finance-dag
  '((sector      -> market-beta)
    (sector      -> stock-return)
    (market-beta -> stock-return)))
```

```
sector ──→ market-beta ──→ stock-return
  │                              ↑
  └──────────────────────────────┘
```

Nodes and their roles:

| Variable       | Role                                                      |
| -------------- | --------------------------------------------------------- |
| `sector`       | Observed confounder (e.g. Technology, Energy, Financials) |
| `market-beta`  | Exposure — systematic risk sensitivity (the "X" in do(X)) |
| `stock-return` | Outcome — monthly excess return over risk-free rate       |

---

## Symbolic Identification with Do-Calculus

### The Three Rules

Pearl's do-calculus provides three rules that transform interventional
quantities into observational ones.  `std.causal` implements each rule as
a relational query over a DAG.

**Rule 1 — Insertion/deletion of observations:**

```
P(y | do(x), z, w) = P(y | do(x), w)
if  Y ⊥ Z | X, W  in  G_{X̄}
```

`G_{X̄}` is the graph with all incoming edges to `X` removed.
If `Y` is independent of `Z` given `X` and `W` in that graph, we can
drop `Z` from the conditioning set.

**Rule 2 — Action/observation exchange:**

```
P(y | do(x), do(z), w) = P(y | do(x), z, w)
if  Y ⊥ Z | X, W  in  G_{X̄, Z_}
```

`G_{X̄, Z_}` additionally removes all outgoing edges from `Z`.
If `Y` is then independent of `Z`, a `do(z)` intervention can be
replaced by passive observation of `z`.

**Rule 3 — Insertion/deletion of actions:**

```
P(y | do(x), do(z), w) = P(y | do(x), w)
if  Y ⊥ Z | X, W  in  G_{X̄, Z̄(W)}
```

where `Z̄(W)` contains the Z-nodes that are not ancestors of any W-node
in `G_{X̄}`.  If the condition holds, the `do(z)` can be dropped entirely.

### Back-Door Identification

For the finance DAG, the **back-door criterion** identifies the
adjustment set directly without applying the three rules sequentially.
`(do:identify dag y x)` checks it automatically:

```scheme
(do:identify finance-dag 'stock-return 'market-beta)
;; => (adjust (sector) (P stock-return market-beta (sector)) (P (sector)))
```

To inspect all valid adjustment sets and status metadata:

```scheme
(do:identify-details finance-dag 'stock-return 'market-beta)
;; => ((status . adjust)
;;     (result . (adjust (sector) ...))
;;     (chosen-z . (sector))
;;     (all-z-sets . ((sector)))
;;     (assumptions . (...)))
```

Formatted as a human-readable formula:

```scheme
(do:adjustment-formula->string formula 'stock-return 'market-beta)
;; => "P(stock-return | do(market-beta)) =
;;     Σ_{sector} P(stock-return | market-beta, sector) · P(sector)"
```

**Proof that `{sector}` satisfies the back-door criterion:**

1. **No descendant condition**: `sector` is not a descendant of
   `market-beta` ✓ (it is a *parent* of `market-beta`)
2. **Blocking condition**: `sector` blocks the only back-door path:
   
   ```
   market-beta ← sector → stock-return
   ```
   
   Conditioning on `sector` closes this path ✓

```scheme
(dag:satisfies-backdoor? finance-dag 'market-beta 'stock-return '(sector))
;; => #t
```

Observed-set-restricted identification (useful for latent-confounder stress DAGs):

```scheme
(define latent-dag
  '((latent-u    -> market-beta)
    (latent-u    -> stock-return)
    (sector      -> market-beta)
    (sector      -> stock-return)
    (market-beta -> stock-return)))

(do:identify-details-observed
  latent-dag 'stock-return 'market-beta '(sector market-beta stock-return))
;; => ((status . unidentified) ...)
```

---

## DAG Graph Utilities

`std.causal` provides a complete graph library over the `(from -> to)`
edge-list format:

| Function                                                     | Description                                                         |
| ------------------------------------------------------------ | ------------------------------------------------------------------- |
| `(dag:nodes dag)`                                            | All nodes in the graph                                              |
| `(dag:parents dag n)`                                        | Direct causes of `n`                                                |
| `(dag:children dag n)`                                       | Direct effects of `n`                                               |
| `(dag:ancestors dag n)`                                      | Transitive causes (BFS)                                             |
| `(dag:descendants dag n)`                                    | Transitive effects (BFS)                                            |
| `(dag:non-descendants dag n)`                                | All nodes except descendants of `n`                                 |
| `(dag:has-path? dag a b forbidden)`                          | Path from `a` to `b` not through `forbidden`                        |
| `(dag:add-edge dag from to)`                                 | Return DAG with `from -> to` inserted                               |
| `(dag:remove-edge dag from to)`                              | Return DAG with `from -> to` removed                                |
| `(dag:flip-edge dag from to)`                                | Return DAG with `from -> to` flipped to `to -> from`                |
| `(dag:d-connected? dag x y z-set)`                           | True if any active path exists between `x` and `y` given `z-set`    |
| `(dag:d-separated? dag x y z-set)`                           | True if all paths between `x` and `y` are blocked by `z-set`        |
| `(dag:satisfies-backdoor? dag x y z-set)`                    | Back-door criterion check                                           |
| `(dag:adjustment-sets dag x y [max-size])`                   | Enumerate valid back-door adjustment sets, minimal first            |
| `(dag:adjustment-sets-observed dag x y observed [max-size])` | Enumerate valid sets restricted to observed variables               |
| `(do:identify-details dag y x [max-size])`                   | Identification metadata with chosen and alternative adjustment sets |
| `(do:identify-details-observed dag y x observed [max-size])` | Identification metadata under observed-only adjustment              |
| `(do:identify-observed dag y x observed [max-size])`         | Convenience API returning only the observed-set-constrained formula |

```scheme
(dag:ancestors finance-dag 'stock-return)
;; => (market-beta sector)

(dag:non-descendants finance-dag 'market-beta)
;; => (sector)    — stock-return is a descendant, so excluded

(dag:has-path? finance-dag 'sector 'stock-return '(market-beta))
;; => #t  — path: sector → stock-return (direct edge)

(dag:has-path? finance-dag 'sector 'stock-return '(market-beta stock-return))
;; => #f  — both paths blocked
```

---

## Numeric Estimation

### The Back-Door Adjustment Formula

Once `do:identify` returns the adjustment formula, it can be evaluated
numerically against observational data:

```
E[Y | do(X=x)] = Σ_s  E[Y | X=x, sector=s]  ·  P(sector=s)
```

`std.causal` provides `do:estimate-effect` and `do:conditional-mean` for
this purpose, using plain Eta arithmetic:

```scheme
(do:conditional-mean
  (filter (lambda (obs) (= (assq 'sector obs) 'tech)) data)
  'stock-return)
;; => sample mean of stock-return in the tech stratum
```

### End-to-End Example

```scheme
(module causal-demo
  (import std.causal)
  (import std.io)
  (begin
    (define dag
      '((sector      -> market-beta)
        (sector      -> stock-return)
        (market-beta -> stock-return)))

    ;; Embedded toy data: 30 observations
    ;; (sector market-beta stock-return)
    (define data '(...))   ; see examples/causal-factor/analysis.eta

    ;; Symbolic identification
    (define formula (do:identify dag 'stock-return 'market-beta))
    (println (do:adjustment-formula->string formula 'stock-return 'market-beta))

    ;; Numeric estimation: CATE for beta=1.4 vs beta=0.9
    (define cate
      (- (do:estimate-effect 'stock-return 'market-beta 1.4
                             '(sector) '((sector . tech))    data)
         (do:estimate-effect 'stock-return 'market-beta 0.9
                             '(sector) '((sector . energy))  data)))
    (print "Adjusted CATE: ")
    (println cate)))
```

Run the full example:

```bash
etai examples/causal-factor/analysis.eta
```

---

## CLP Integration

`std.clp` connects to causal reasoning through **structural constraints**
on the estimands.  For example, since `P(sector)` is a probability it
must lie in `(0, 1)`.  Representing as a percentage:

```scheme
(import std.clp)

(let ((w-tech (logic-var)))
  (clp:domain w-tech 1 99)   ; P(tech) ∈ (0,1) — non-degenerate
  (unify w-tech 33)           ; observed: 33% of data is tech
  (println (deref-lvar w-tech)))  ; => 33
```

CLP is also used to assert that estimated causal effects satisfy
prior knowledge bounds — a lightweight form of causal sensitivity
analysis:

```scheme
;; The CAPM predicts that the excess-return slope wrt beta equals
;; the equity risk premium.  Historically this is in [3%, 8%] per unit beta.
(let ((slope (logic-var)))
  (clp:domain slope 3 8)      ; prior knowledge: slope ∈ [3,8]% per unit beta
  (if (and (unify slope estimated-slope-pct)
           (ground? slope))
      (println "Estimate is consistent with CAPM prior.")
      (println "Warning: estimate outside historical ERP range.")))
```

---

## Limitations

> [!IMPORTANT]
> **Numerical estimation** currently uses a *plug-in estimator*:
> sample means and proportions, giving an unbiased point estimate
> but **no confidence intervals or hypothesis tests**.
> 
> Production-quality causal inference requires:
> 
> - Bootstrap or influence-function standard errors
> - Doubly-robust estimation (combines outcome model + propensity score)
> - Regression-based adjustment for continuous confounders
> 
> These methods require BLAS/LAPACK matrix operations or a library such
> as **libtorch** or **Eigen**.  In Eta, they are implemented through the
> native runtime integration layer so `do:estimate-effect` can use
> vectorised regression instead of loop-based accumulators.

---

## Extended Finance Examples

### Fama-French Three-Factor Model

```
market-return  ──→ stock-return
smb            ──→ stock-return     (size premium: Small Minus Big)
hml            ──→ stock-return     (value premium: High Minus Low)
economic-cycle ──→ market-return
economic-cycle ──→ smb
economic-cycle ──→ hml
```

Query: `P(stock-return | do(hml))` — the causal effect of increasing
value-factor exposure, holding market and size exposures fixed.
`{economic-cycle}` satisfies the back-door criterion.

### CVA Causal Decomposition

In the context of Credit Valuation Adjustment (CVA, see [`docs/xva.md`](xva.md)):

```
credit-quality ──→ default-intensity ──→ CVA
interest-rate  ──→ exposure
interest-rate  ──→ CVA
credit-quality ──→ CVA
```

Query: `P(CVA | do(interest-rate))` — the causal contribution of a
rate shock to CVA, isolating it from the confounding through credit
quality.

---

## Source Locations

| Component                            | File                                                                                |
| ------------------------------------ | ----------------------------------------------------------------------------------- |
| DAG utilities, do-calculus engine    | [`stdlib/std/causal.eta`](../stdlib/std/causal.eta)                                 |
| DAG demo                             | [`examples/do-calculus/dag.eta`](../examples/do-calculus/dag.eta)                   |
| Three rules implementation           | [`examples/do-calculus/do-rules.eta`](../examples/do-calculus/do-rules.eta)         |
| Full identification demo             | [`examples/do-calculus/demo.eta`](../examples/do-calculus/demo.eta)                 |
| CSV loader                           | [`examples/causal-factor/csv-loader.eta`](../examples/causal-factor/csv-loader.eta) |
| Back-door estimator                  | [`examples/causal-factor/adjustment.eta`](../examples/causal-factor/adjustment.eta) |
| Finance analysis                     | [`examples/causal-factor/analysis.eta`](../examples/causal-factor/analysis.eta)     |
| CLP binding for `clp(Z)` / `clp(FD)` | [`stdlib/std/clp.eta`](../stdlib/std/clp.eta)                                       |
| C++ constraint store                 | [`clp/constraint_store.h`](../eta/core/src/eta/runtime/clp/constraint_store.h)      |
