# Causal Inference — `std.causal` Reference

[← Back to README](../../../README.md) ·
[Portfolio Engine](../../featured_examples/portfolio.md) · [Logic Programming](logic.md) ·
[CLP](clp.md) · [AAD](aad.md) · [xVA](xva.md)

> [!TIP]
> **See also**
>
> - [`examples/causal_demo.eta`](../../../examples/causal_demo.eta) — gentle
>   end-to-end primer (3-node DAG, single confounder) combining symbolic
>   differentiation, do-calculus identification, `findall` + CLP
>   validation, and a libtorch neural ATE.
> - [`causal-counterfactual.md`](./causal-counterfactual.md) - focused
>   counterfactual reference for twin networks, ID*, IDC*, and ETT.
> - [`portfolio.md`](../../featured_examples/portfolio.md) — full institutional pipeline:
>   6-node macro DAG, AAD risk sensitivities, CLP(R) + QP allocation,
>   scenario stress, dynamic control.

---

## Overview

This page is the **API reference** for Eta's causal-inference stack.
The implementation is research-grade and covers Pearl's identification
and estimation surface end-to-end:

| Layer | Module | Status |
| :---- | :----- | :----- |
| DAG utilities, do-calculus rules, back-door identification, plug-in adjustment | `std.causal`               | ✅ |
| ADMGs, bidirected edges, latent projection, c-components                       | `std.causal.admg`          | ✅ |
| ID / IDC algorithms, hedge detection, estimand simplifier                      | `std.causal.identify`      | ✅ |
| Generalised adjustment, front-door, instrumental variables                     | `std.causal.adjustment`    | ✅ |
| Mediation (NDE / NIE / CDE)                                                    | `std.causal.mediation`     | ✅ |
| Selection diagrams, sBD criterion, transport queries                           | `std.causal.transport`     | ✅ |
| Twin networks, ID* / IDC*, effect-of-treatment-on-the-treated                  | `std.causal.counterfactual`| ✅ |
| g-formula, IPW, AIPW, TMLE, bootstrap CIs, E-value, Rosenbaum bounds           | `std.causal.estimate`      | ✅ |
| PC / FCI / GES / NOTEARS structure learning + CI tests                         | `std.causal.learn`         | ✅ |
| DOT, Mermaid and LaTeX rendering, `define-dag` macro                           | `std.causal.render`        | ✅ |
| CATE meta-learners (S/T/X/R/DR)                                                | `std.causal.cate`          | ✅ |
| Regression CART + random forest                                                 | `std.ml.tree`, `std.ml.forest` | ✅ |
| Causal forest + local AIPW query                                                | `std.causal.forest`        | ✅ |
| Cross-fitting and DML (PLR/IRM)                                                | `std.causal.crossfit`      | ✅ |
| Uplift, Qini, and policy-value scoring                                          | `std.causal.policy`        | ✅ |

The layer also composes with:

- **`std.logic`** — structural unification and backtracking
- **`std.clp`** / **`std.clpr`** — bounding probability estimands and
  feasibility checks
- **`std.stats`** — Fisher-z residualisation for CI tests, OLS / GLS for
  outcome and propensity models
- **`std.torch`** — neural propensity / outcome models for `std.causal.estimate`
- **`std.ml.tree`** / **`std.ml.forest`** — tree and random-forest learners for CATE backbones

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
quantities into observational ones.  Eta exposes these directly in
`std.causal` via:

- `dag:mutilate-do`
- `dag:mutilate-see`
- `do-rule1-applies?`
- `do-rule2-applies?`
- `do-rule3-applies?`

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

`dag:d-connected?` and `dag:d-separated?` are implemented with
Bayes-ball traversal, so reachability queries scale linearly with graph size.

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
| `(dag:valid? dag)`                                            | True when edges are well-formed and acyclic                         |
| `(dag:cyclic? dag)`                                           | True when a directed cycle exists                                   |
| `(dag:topo-sort dag)`                                         | Topological order (raises `cyclic-graph` on cyclic input)           |
| `(dag:add-edge dag from to)`                                 | Return DAG with `from -> to` inserted                               |
| `(dag:remove-edge dag from to)`                              | Return DAG with `from -> to` removed                                |
| `(dag:flip-edge dag from to)`                                | Return DAG with `from -> to` flipped to `to -> from`                |
| `(dag:d-connected? dag x-or-set y-or-set z-set)`             | True if any active path exists between X and Y given `z-set`        |
| `(dag:d-separated? dag x-or-set y-or-set z-set)`             | True if all paths between X and Y are blocked by `z-set`            |
| `(dag:mutilate-do dag x-set)`                                | Remove all incoming edges to nodes in `x-set`                       |
| `(dag:mutilate-see dag x-set)`                               | Remove all outgoing edges from nodes in `x-set`                     |
| `(do-rule1-applies? dag y x z w)`                            | Rule 1 applicability check                                           |
| `(do-rule2-applies? dag y x z w)`                            | Rule 2 applicability check                                           |
| `(do-rule3-applies? dag y x z w)`                            | Rule 3 applicability check                                           |
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

## ADMG Utilities

For mixed graphs with latent confounding, import:

```scheme
(import std.causal.admg)
```

`std.causal.admg` provides:

| Function | Description |
| -------- | ----------- |
| `(admg:directed g)` | Directed edges in an ADMG |
| `(admg:bidirected g)` | Bidirected edges (`<->`) |
| `(admg:district g v)` / `(admg:districts g)` | Bidirected-connected components |
| `(admg:project dag latents)` | Latent projection from DAG to ADMG over observed nodes |
| `(admg:ancestors g v-or-set)` | Directed ancestors (ignores bidirected edges) |
| `(admg:moralize g s)` | Ancestral moralization to an undirected graph |

---

## ID and IDC on ADMGs

To identify effects in mixed graphs with latent confounding:

```scheme
(import std.causal.identify)
```

| Function | Description |
| -------- | ----------- |
| `(id g y x)` | Returns an estimand AST for `P(Y | do(X))` or `(fail (hedge ...))` |
| `(idc g y x z)` | Returns an estimand AST for `P(Y | do(X), Z)` or `(fail (hedge ...))` |
| `(do:simplify estimand)` | Simplifies estimand ASTs by collapsing trivial sums, products, and conditionals |

The returned AST uses:

- `(P vars)` for factors
- `(sum vars expr)` for marginalization
- `(prod e1 e2 ...)` for products
- `(cond-on expr cond)` for conditional forms
- `(fail (hedge ...))` for non-identifiable queries

### Worked examples

```scheme
(import std.causal.identify)

;; Direct effect — trivial conditional.
(id '((x -> y)) '(y) '(x))
;; => (P (y x))

;; Front-door (X -> M -> Y, X <-> Y latent confounder).
(id '((x -> m) (m -> y) (x <-> y)) '(y) '(x))
;; => (sum (m)
;;      (prod (P (m x))
;;            (sum (x*) (prod (P (y x* m)) (P (x*))))))

;; Bow graph — non-identifiable, returns a hedge witness.
(id '((x -> y) (x <-> y)) '(y) '(x))
;; => (fail (hedge ...))

;; Napkin graph — identifiable through W1, W2.
(id '((w1 -> w2) (w2 -> x) (x -> y) (w1 <-> x) (w1 <-> y)) '(y) '(x))
;; => (sum ...)   ; not a hedge

;; IDC: condition on Z.
(idc '((x -> y) (z -> y)) '(y) '(x) '(z))
;; => (P (y x z))
```

---

## Adjustment, Front-Door, and IV

For generalized adjustment enumeration and structural criteria checks:

```scheme
(import std.causal.adjustment)
```

| Function | Description |
| -------- | ----------- |
| `(dag:gac? g x y z)` | Generalized adjustment criterion check on mixed graphs |
| `(dag:minimal-adjustments g x y observed)` | Enumerate minimal valid adjustment sets from `observed` |
| `(front-door? g x y m)` | Front-door criterion check for mediator set `m` |
| `(do:front-door-formula y x m)` | Return canonical front-door estimand AST |
| `(iv? g z x y)` | Instrumental-variable criterion check |

---

## Mediation Analysis

For natural and controlled direct/indirect effect decomposition:

```scheme
(import std.causal.mediation)
```

| Function | Description |
| -------- | ----------- |
| `(mediation-formula data y x m treatment-for-y treatment-for-m)` | Cross-world mediation mean `Sum_m E[Y|X=treatment-for-y, M=m] * P(M=m|X=treatment-for-m)` |
| `(do:nde g y x m data x* x)` | Natural direct effect `E[Y(x, M(x*))] - E[Y(x*, M(x*))]` |
| `(do:nie g y x m data x* x)` | Natural indirect effect `E[Y(x, M(x))] - E[Y(x, M(x*))]` |
| `(do:cde g y x m data m-val x* x)` | Controlled direct effect at fixed mediator value(s) |

The mediation helpers enforce data support checks for requested treatment
values and mediator-treatment strata.

---

## Transportability and Selection Bias

For selection-diagram checks and transport queries:

```scheme
(import std.causal.transport)
```

| Function | Description |
| -------- | ----------- |
| `(s-node? n)` | True when `n` is an S-node marker (symbol name starts with `S`) |
| `(do:sBD? g x y z s-nodes)` | Selection back-door criterion check |
| `(do:transport g* g y x)` | Transport query: returns target ID estimand, transport-adjustment estimand, or `(fail (transport ...))` |

`do:transport` uses a conservative workflow:
- return target-domain ID when no S-nodes are present;
- return target-domain ID when `Y` is selection-invariant given `X`;
- otherwise try an s-backdoor adjustment set `Z`;
- otherwise return a transport failure witness.

When s-backdoor transport applies, the returned estimand uses `P*` for
source-domain interventional information:

```scheme
(sum (z) (prod (P* (y x z)) (P (z))))
```

---

## Counterfactual Queries

For counterfactual graph transforms and ID*/IDC* wrappers:

```scheme
(import std.causal.counterfactual)
```

| Function | Description |
| -------- | ----------- |
| `(twin-network g intervened-vars)` | Build a twin network with primed counterfactual copies and shared latent links for non-intervened variables |
| `(id* g gamma)` | Counterfactual identification entry point for conjunctions of `Y_X` events |
| `(idc* g gamma delta)` | Conditional counterfactual identification entry point |
| `(do:ett g y x x*)` | Effect of treatment on the treated query `P(Y_x | X=x*)` |

Event syntax and a fuller worked example live on the
[counterfactual reference page](./causal-counterfactual.md).

---

## Structure Learning

For data-driven structure learning helpers:

```scheme
(import std.causal.learn)
```

| Function | Description |
| -------- | ----------- |
| `(learn:ci-test:fisher-z data x y z alpha)` | Partial-correlation conditional-independence test returning `(independent? . p-value)` |
| `(learn:ci-test:chi2 data x y z alpha)` | Discrete chi-square conditional-independence test returning `(independent? . p-value)` |
| `(learn:pc data alpha ci-test)` | PC skeleton + collider orientation + Meek propagation, returned as a CPDAG |
| `(learn:fci data alpha ci-test)` | Latent-confounding hook; currently delegates to `learn:pc` |
| `(learn:ges data [alpha [ci-test]])` | Score-search hook; currently delegates to `learn:pc` |
| `(learn:notears data lambda1 max-iter)` | Continuous-learning hook with correlation thresholding and transitive reduction |

CPDAG edge format:
- directed edge: `(u -> v)`
- undirected edge: `(u -- v)`

### Worked example

```scheme
(import std.causal.learn)

;; Linear-Gaussian chain  a -> b -> c.
(define chain-data
  (let loop ((i 0) (acc '()))
    (if (= i 240)
        (reverse acc)
        (let* ((a (- (* 1.0 i) 120.0))
               (b (+ (* 0.9 a) (if (even? i) 0.15 -0.15)))
               (c (+ (* 0.9 b) (if (zero? (modulo i 3)) 0.10 -0.10))))
          (loop (+ i 1)
                (cons (list (cons 'a a) (cons 'b b) (cons 'c c))
                      acc))))))

(learn:pc chain-data 0.01 learn:ci-test:fisher-z)
;; => CPDAG containing (a -- b) (b -- c) ; long edge a-c removed.

(learn:notears chain-data 0.2 100)
;; => ((a -> b) (b -> c))   ; transitive a->c reduced away.

(learn:ci-test:fisher-z chain-data 'a 'c '(b) 0.05)
;; => (#t . p-value)   ; conditionally independent given the mediator.
```

---

## Rendering and the `define-dag` macro

For visualisation and validated graph literals:

```scheme
(import std.causal.render)
```

| Function / form | Description |
| --------------- | ----------- |
| `(dag:->dot g [opts])` | Graphviz DOT (directed `->`, dashed bidirected `<->`, undirected `--`). Options alist supports `(title . "...")` and `(rankdir . "LR")` |
| `(dag:->mermaid g)`    | Mermaid `flowchart LR` with deterministic node ids |
| `(dag:->latex g)`      | Compact LaTeX math expression with `\to` and `\leftrightarrow` |
| `(define-dag name edge ...)` | Macro that validates edges (`->` and `<->`) and acyclicity at expansion time |

```scheme
(define-dag finance-dag
  (sector      -> market-beta)
  (sector      -> stock-return)
  (market-beta -> stock-return))

(dag:->mermaid '((a -> b) (a <-> c) (b -- c)))
;; flowchart LR
;;   n0["a"]
;;   n1["b"]
;;   n2["c"]
;;   n0 --> n1
;;   n0 -.-> n2
;;   n2 -.-> n0
;;   n1 --- n2

(dag:->dot finance-dag '((title . "Finance DAG") (rankdir . "TB")))
;; digraph "Finance DAG" {
;;   rankdir=TB;
;;   "market-beta" -> "stock-return";
;;   "sector" -> "market-beta";
;;   "sector" -> "stock-return";
;; }
```

---

## Graph Rendering

For graph rendering and graph-literal validation helpers:

```scheme
(import std.causal.render)
```

| Function | Description |
| -------- | ----------- |
| `(dag:->dot g [opts])` | Render a mixed graph as Graphviz DOT (`->`, `<->`, `--`) |
| `(dag:->mermaid g)` | Render a mixed graph as Mermaid `flowchart LR` text |
| `(dag:->latex g)` | Render a mixed graph as a compact LaTeX expression |
| `(define-dag g)` | Validate and normalize a directed/bidirected graph literal |

DOT options:
- `(title . "...")` sets the graph title (`"Causal DAG"` by default)
- `(rankdir . "...")` sets Graphviz rank direction (`"LR"` by default)

Rendering conventions:
- bidirected edge `(u <-> v)` is rendered as a dashed bidirectional edge
- undirected edge `(u -- v)` is rendered without arrowheads

---

## Numeric Estimation

Once an estimand is identified, two estimation surfaces are available:

1. **Plug-in stratified adjustment** — bundled in `std.causal`,
   evaluates the back-door formula directly from the data.
2. **Modern estimators** — in `std.causal.estimate`:
   g-formula, IPW, AIPW, TMLE, plus bootstrap CIs and sensitivity tools.

### 1. Plug-in adjustment (`std.causal`)

For the finance DAG the back-door formula is

```
E[Y | do(X=x)] = Σ_s  E[Y | X=x, sector=s]  ·  P(sector=s)
```

`std.causal` evaluates it with plain Eta arithmetic:

```scheme
(do:estimate-effect y-var x-var x-val z-set data)
;; legacy arity also accepted:
;; (do:estimate-effect y-var x-var x-val z-set z-val data)

(do:conditional-mean
  (filter (lambda (obs) (eq? (cdr (assq 'sector obs)) 'tech)) data)
  'stock-return)
;; => sample mean of stock-return in the tech stratum
```

### 2. Modern estimators (`std.causal.estimate`)

For practical ATE estimation on binary-treatment observational data:

```scheme
(import std.causal.estimate)
```

| Function | Description |
| -------- | ----------- |
| `(do:ate data y x z)` | Default ATE estimator (AIPW) |
| `(do:ate-gformula data y x z)` | Stratified g-formula |
| `(do:ate-ipw data y x z)` | Inverse-probability weighting |
| `(do:ate-aipw data y x z)` | Augmented IPW (doubly robust) |
| `(do:ate-tmle data y x z)` | One-step targeted minimum loss estimator |
| `(do:propensity-score data x z)` | Stratified empirical propensity scores |
| `(do:bootstrap-ci estimator data n-boot alpha [seed])` | Percentile bootstrap CI |
| `(do:e-value rr)` | E-value for risk-ratio sensitivity analysis |
| `(do:rosenbaum-bound ratio gamma)` | Multiplicative sensitivity envelope `(lower . upper)` |

Notes:
- Binary treatment `x` in `{0,1}` or `{#f,#t}`.
- Strata-based nuisance models; for richer nuisance fits plug `std.torch`
  or `std.stats` regressors into your own wrapper.
- `do:bootstrap-ci` is generic over any of the estimators above.

### End-to-end example

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
    (define data '(...))   ; see examples/causal_demo.eta for the DGP

    ;; Symbolic identification
    (define formula (do:identify dag 'stock-return 'market-beta))
    (println (do:adjustment-formula->string formula 'stock-return 'market-beta))

    ;; Numeric estimation: CATE for beta=1.4 vs beta=0.9
    (define cate
      (- (do:estimate-effect 'stock-return 'market-beta 1.4
                             '(sector) data)
         (do:estimate-effect 'stock-return 'market-beta 0.9
                             '(sector) data)))
    (print "Adjusted CATE: ")
    (println cate)))
```

Run the full primer:

```bash
etai examples/causal_demo.eta
```

---

## CATE Meta-Learners

For conditional treatment-effect modelling:

```scheme
(import std.causal.cate)
```

| Function | Description |
| -------- | ----------- |
| `(cate:fit-s-learner data y x z reg-spec)` | Fit an S-learner (single response model over `[x z]`) |
| `(cate:fit-t-learner data y x z reg-spec)` | Fit a T-learner (separate response models per treatment arm) |
| `(cate:fit-x-learner data y x z reg-spec cls-spec)` | Fit an X-learner with propensity-weighted pseudo-outcome blending |
| `(cate:fit-r-learner data y x z reg-spec [opts...])` | Fit an R-learner via orthogonalized pseudo-residual regression |
| `(cate:fit-dr-learner data y x z reg-spec cls-spec [opts...])` | Fit a DR-learner via doubly-robust pseudo-outcomes |
| `(cate:predict model row)` | Predict CATE at one observation alist |
| `(cate:ate model data)` | Average predicted CATE over a dataset |
| `(cate:rank model data)` | Return `(sorted-data . tau-hats)` in descending CATE order |
| `(cate:residual-r2 actual predicted)` | `R^2` diagnostic for two equal-length numeric vectors |
| `(cate:propensity-overlap scores [threshold])` | Overlap summary alist for propensity-score vectors |

Learner-spec protocol expected by the fitters:

```scheme
((fit . (lambda (X y) -> model))
 (predict . (lambda (model X) -> yhat))
 (kind . regressor|classifier)
 (fit-weighted . (lambda (X y w) -> model))) ; optional
```

`std.stats` already ships compatible constructors:

```scheme
(define reg (stats:make-ols-regressor))
(define cls (stats:make-logistic))
(define m (cate:fit-dr-learner data 'y 'x '(z1 z2 z3) reg cls))
(cate:ate m data)
```

---

## Cross-fitting and DML

For cross-fitted nuisance estimation and Double Machine Learning:

```scheme
(import std.causal.crossfit)
```

| Function | Description |
| -------- | ----------- |
| `(crossfit:k-folds n k seed)` | Deterministic K-fold split as `((train . test) ...)` |
| `(crossfit:nuisance learner data cols y k seed)` | Out-of-fold predictions for one target variable |
| `(crossfit:nuisance-arm learner data y x z arm k seed)` | Arm-specific out-of-fold response model `mu_arm(z)` |
| `(crossfit:dml-plr data y x z reg-mu reg-m k seed)` | Partially linear DML estimator (PLR) |
| `(crossfit:dml-irm data y x z reg-mu cls-e k seed)` | Interactive regression DML estimator (IRM, binary treatment) |
| `(crossfit:influence-se psi den n)` | Influence-function standard error helper |
| `(crossfit:dml-ci theta se alpha)` | Normal-approximation confidence interval `(lower upper)` |

Minimal usage:

```scheme
(import std.causal.crossfit)
(import std.stats)

(define reg (stats:make-ols-regressor))
(define cls (stats:make-logistic))

(crossfit:dml-plr data 'y 'x '(z1 z2 z3) reg reg 5 42)
;; => ((theta . ...) (se . ...) (ci . (... ...)) (n . ...))

(crossfit:dml-irm data 'y 'x '(z1 z2 z3) reg cls 5 42)
;; => ((theta . ...) (se . ...) (ci . (... ...)) (n . ...))
```

Notes:
- `crossfit:dml-irm` expects binary treatment (`0/1` or `#f/#t`).
- All nuisance vectors are aligned with input row order.

---

## Uplift, Qini, and Policy Value

For treatment-ranking metrics, policy evaluation, and synthetic-effect diagnostics:

```scheme
(import std.causal.policy)
```

| Function | Description |
| -------- | ----------- |
| `(policy:qini-curve cate-preds y x)` | Cumulative uplift/Qini curve as `((k . value) ...)` |
| `(policy:qini-coefficient curve)` | Area between model Qini curve and random-targeting baseline |
| `(policy:auuc curve)` | Normalized area under cumulative gain curve |
| `(policy:cumulative-gain-curve cate-preds y x)` | Alias for cumulative gain curve construction |
| `(policy:value-ipw data y x z policy-fn e-hat)` | Horvitz-Thompson off-policy value estimator |
| `(policy:value-aipw data y x z policy-fn mu1 mu0 e-hat)` | Doubly robust off-policy value estimator |
| `(policy:rank-by-cate data cate-preds)` | Sort rows by descending CATE, returns `(rows . taus)` |
| `(policy:pehe true-cate pred-cate)` | Precision in Estimation of Heterogeneous Effect |
| `(policy:ate-rmse true-ate pred-ate)` | ATE RMSE diagnostic (scalar or vector form) |
| `(policy:ate-bias true-ate pred-ate)` | ATE signed-bias diagnostic (scalar or vector form) |
| `(policy:greedy-treat-positive tau-hat)` | One-step policy `1{tau_hat > 0}` |
| `(policy:greedy-budget cate-preds budget)` | Top-k/top-fraction treatment assignment vector |

Minimal usage:

```scheme
(import std.causal.policy)

(define curve (policy:qini-curve tau-hat y x))
(policy:qini-coefficient curve)
(policy:auuc curve)

(policy:value-ipw data 'y 'x '(z1 z2) policy-fn e-hat)
(policy:value-aipw data 'y 'x '(z1 z2) policy-fn mu1 mu0 e-hat)
```

---

## Trees, Random Forests, and Causal Forest

M13 introduces native tree/forest learners plus a causal-forest wrapper.

### Regression tree (`std.ml.tree`)

```scheme
(import std.ml.tree)
```

| Function | Description |
| -------- | ----------- |
| `(tree:fit X y [opts...])` | Fit regression CART (`'min-leaf`, `'max-depth`, `'mtry`, `'honest?`, `'seed`, optional `'row-ids`) |
| `(tree:predict model X)` | Predict a numeric response vector |
| `(tree:leaf-membership model row)` | Return stable leaf id for one feature row |
| `(tree:leaves model)` | Return leaf records with id, prediction, members, and size |

### Random forest (`std.ml.forest`)

```scheme
(import std.ml.forest)
```

| Function | Description |
| -------- | ----------- |
| `(forest:fit X y [opts...])` | Fit a bagged regression forest (`'n-trees`, `'min-leaf`, `'max-depth`, `'mtry`, `'subsample`, `'honest?`, `'seed`) |
| `(forest:predict model X)` | Predict by averaging tree predictions |
| `(forest:fit-parallel X y [opts...])` | API-compatible entry point (currently serial implementation) |
| `(forest:make-rf-regressor n-trees min-leaf [opts...])` | Return learner-spec adapter compatible with `std.causal.cate` |

### Causal forest (`std.causal.forest`)

```scheme
(import std.causal.forest)
```

| Function | Description |
| -------- | ----------- |
| `(forest:fit-causal-forest data y x z [opts...])` | Fit DR-pseudo-outcome forest for CATE (`'n-trees`, `'nuisance-trees`, `'min-leaf`, `'max-depth`, `'subsample`, `'honest?`, `'mtry`, `'seed`) |
| `(forest:predict-cate cf row)` | Predict CATE for one observation alist |
| `(forest:variable-importance cf)` | Return normalized `(feature . weight)` importances aligned with `z` |
| `(forest:local-aipw cf row)` | Return alpha-weighted local AIPW estimate at a query row |

Minimal usage:

```scheme
(define cf
  (forest:fit-causal-forest data 'y 'x '(z1 z2 z3)
                            'n-trees 500
                            'min-leaf 5
                            'subsample 0.8
                            'seed 42))

(forest:predict-cate cf (car data))
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


## Source Locations

| Component                            | File                                                                                |
| ------------------------------------ | ----------------------------------------------------------------------------------- |
| DAG utilities, do-calculus engine    | [`stdlib/std/causal.eta`](../../../stdlib/std/causal.eta)                                 |
| Adjustment/front-door/IV helpers     | [`stdlib/std/causal/adjustment.eta`](../../../stdlib/std/causal/adjustment.eta)           |
| ADMG ID/IDC algorithms               | [`stdlib/std/causal/identify.eta`](../../../stdlib/std/causal/identify.eta)               |
| Mediation effect estimators          | [`stdlib/std/causal/mediation.eta`](../../../stdlib/std/causal/mediation.eta)             |
| Transportability helpers             | [`stdlib/std/causal/transport.eta`](../../../stdlib/std/causal/transport.eta)             |
| Counterfactual helpers               | [`stdlib/std/causal/counterfactual.eta`](../../../stdlib/std/causal/counterfactual.eta)   |
| CATE meta-learner API                | [`stdlib/std/causal/cate.eta`](../../../stdlib/std/causal/cate.eta)                       |
| Cross-fitting / DML API              | [`stdlib/std/causal/crossfit.eta`](../../../stdlib/std/causal/crossfit.eta)               |
| Uplift / policy scoring API          | [`stdlib/std/causal/policy.eta`](../../../stdlib/std/causal/policy.eta)                   |
| Causal forest helpers                | [`stdlib/std/causal/forest.eta`](../../../stdlib/std/causal/forest.eta)                   |
| Regression tree                      | [`stdlib/std/ml/tree.eta`](../../../stdlib/std/ml/tree.eta)                               |
| Random forest                        | [`stdlib/std/ml/forest.eta`](../../../stdlib/std/ml/forest.eta)                           |
| Structure learning helpers           | [`stdlib/std/causal/learn.eta`](../../../stdlib/std/causal/learn.eta)                     |
| Estimation backends                  | [`stdlib/std/causal/estimate.eta`](../../../stdlib/std/causal/estimate.eta)               |
| Rendering (DOT / Mermaid / LaTeX) and `define-dag` macro | [`stdlib/std/causal/render.eta`](../../../stdlib/std/causal/render.eta) |
| Counterfactual reference page        | [`causal-counterfactual.md`](./causal-counterfactual.md)                                  |
| DAG demo                             | [`examples/do-calculus/dag.eta`](../../../examples/do-calculus/dag.eta)                   |
| Do-calculus rules demo               | [`examples/do-calculus/do-rules.eta`](../../../examples/do-calculus/do-rules.eta)         |
| Full identification demo             | [`examples/do-calculus/demo.eta`](../../../examples/do-calculus/demo.eta)                 |
| CSV module                           | [`stdlib/std/csv.eta`](../../../stdlib/std/csv.eta)                                         |
| End-to-end primer (causal + NN ATE)  | [`examples/causal_demo.eta`](../../../examples/causal_demo.eta)                           |
| CATE test suite                      | [`stdlib/tests/causal-cate.test.eta`](../../../stdlib/tests/causal-cate.test.eta)         |
| Cross-fitting test suite             | [`stdlib/tests/causal-crossfit.test.eta`](../../../stdlib/tests/causal-crossfit.test.eta) |
| Policy scoring test suite            | [`stdlib/tests/causal-policy.test.eta`](../../../stdlib/tests/causal-policy.test.eta)     |
| Tree/forest test suite               | [`stdlib/tests/ml-tree.test.eta`](../../../stdlib/tests/ml-tree.test.eta)                 |
| Causal-forest test suite             | [`stdlib/tests/causal-forest.test.eta`](../../../stdlib/tests/causal-forest.test.eta)     |
| CLP binding for `clp(Z)` / `clp(FD)` | [`stdlib/std/clp.eta`](../../../stdlib/std/clp.eta)                                       |
| C++ constraint store                 | [`clp/constraint_store.h`](../../../eta/core/src/eta/runtime/clp/constraint_store.h)      |
