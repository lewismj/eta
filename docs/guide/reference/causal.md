# Causal Inference — `std.causal` Reference

[← Back to README](../../../README.md) ·
[Portfolio Engine](../../featured_examples/portfolio.md) · [Logic Programming](logic.md) ·
[CLP](clp.md) · [AAD](aad.md) · [xVA](xva.md) · [Project Status](../../next-steps.md)

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

This page is the **API reference** for Eta's causal inference layer:
the `std.causal` module plus the supporting `examples/do-calculus/`
programs and the [`examples/causal_demo.eta`](../../../examples/causal_demo.eta)
primer.

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

For counterfactual graph transforms and pragmatic ID*/IDC* wrappers:

```scheme
(import std.causal.counterfactual)
```

| Function | Description |
| -------- | ----------- |
| `(twin-network g intervened-vars)` | Build a twin network with primed counterfactual copies and shared latent links for non-intervened variables |
| `(id* g gamma)` | Counterfactual identification entry point for conjunctions of `Y_X` events |
| `(idc* g gamma delta)` | Conditional counterfactual identification entry point |
| `(do:ett g y x x*)` | Effect of treatment on the treated query `P(Y_x | X=x*)` |

Current scope:
- `id*`/`idc*` normalize counterfactual event syntax and reuse `std.causal.identify` (`id`/`idc`) for estimand construction.
- `gamma` is represented as a list of events `(Y X)` where `X` is a symbol or symbol list.
- `delta` is represented as assignment-like entries where the first element names the conditioned variable.

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
(do:estimate-effect y-var x-var x-val z-set data)
;; legacy arity also accepted:
;; (do:estimate-effect y-var x-var x-val z-set z-val data)
```

```scheme
(do:conditional-mean
  (filter (lambda (obs) (= (assq 'sector obs) 'tech)) data)
  'stock-return)
;; => sample mean of stock-return in the tech stratum
```

### Estimation Backends 

For practical ATE estimation from binary-treatment observational data:

```scheme
(import std.causal.estimate)
```

Available APIs:

| Function | Description |
| -------- | ----------- |
| `(do:ate data y x z)` | Default ATE estimator (AIPW) |
| `(do:ate-gformula data y x z)` | Stratified g-formula |
| `(do:ate-ipw data y x z)` | Inverse-probability weighting |
| `(do:ate-aipw data y x z)` | Augmented IPW (doubly robust form) |
| `(do:propensity-score data x z)` | Stratified empirical propensity scores |
| `(do:bootstrap-ci estimator data n-boot alpha [seed])` | Percentile bootstrap CI |

Current scope:
- Binary treatment `x` in `{0,1}` or `{#f,#t}`
- Strata-based nuisance models (no TMLE yet)
- Bootstrap confidence intervals available via `do:bootstrap-ci`

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
> Eta now includes two estimation layers:
> 
> - `std.causal` plug-in adjustment (`do:estimate-effect`)
> - `std.causal.estimate` M9a estimators (`do:ate-gformula`, `do:ate-ipw`,
>   `do:ate-aipw`, `do:bootstrap-ci`)
> 
> Remaining gaps for:
> 
> - TMLE is not yet implemented.
> - No influence-function/analytic standard errors yet.
> - Continuous-treatment and richer model-based nuisance estimation are
>   still pending.
> - Mediation estimands currently rely on observed treatment/mediator
>   support and do not yet include sensitivity analysis modules.

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
| Estimation backends (M9a)            | [`stdlib/std/causal/estimate.eta`](../../../stdlib/std/causal/estimate.eta)               |
| DAG demo                             | [`examples/do-calculus/dag.eta`](../../../examples/do-calculus/dag.eta)                   |
| Do-calculus rules demo               | [`examples/do-calculus/do-rules.eta`](../../../examples/do-calculus/do-rules.eta)         |
| Full identification demo             | [`examples/do-calculus/demo.eta`](../../../examples/do-calculus/demo.eta)                 |
| CSV module                           | [`stdlib/std/csv.eta`](../../../stdlib/std/csv.eta)                                         |
| End-to-end primer (causal + NN ATE)  | [`examples/causal_demo.eta`](../../../examples/causal_demo.eta)                           |
| CLP binding for `clp(Z)` / `clp(FD)` | [`stdlib/std/clp.eta`](../../../stdlib/std/clp.eta)                                       |
| C++ constraint store                 | [`clp/constraint_store.h`](../../../eta/core/src/eta/runtime/clp/constraint_store.h)      |
