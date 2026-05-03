# std.causal

DAG operations, do-calculus rules, back-door identification, and numeric
effect estimation.

```scheme
(import std.causal)
```

A graph is a list of edges. Top-level `std.causal` operates on directed
edges `(u -> v)`; the ADMG modules add bidirected `(u <-> v)` edges.

## DAG queries

| Symbol | Description |
| --- | --- |
| `(dag:nodes g)` | List of node symbols. |
| `(dag:parents g v)` | Parents of `v`. |
| `(dag:children g v)` | Children of `v`. |
| `(dag:ancestors g v)` | Ancestors of `v` (excluding `v`). |
| `(dag:descendants g v)` | Descendants of `v` (excluding `v`). |
| `(dag:non-descendants g v)` | Nodes that are not descendants of `v`. |
| `(dag:has-path? g u v)` | True when there is a directed path. |
| `(dag:valid? g)` | Structural validity check. |
| `(dag:cyclic? g)` | True when the graph has a cycle. |
| `(dag:topo-sort g)` | Topological order. |

## DAG editing

| Symbol | Description |
| --- | --- |
| `(dag:add-edge g e)` | Add an edge. |
| `(dag:remove-edge g e)` | Remove an edge. |
| `(dag:flip-edge g e)` | Reverse an edge. |
| `(dag:mutilate-do g vars)` | Remove all incoming edges to `vars` (do-mutilation). |
| `(dag:mutilate-see g vars)` | Remove all outgoing edges from `vars` (see-mutilation). |

## d-separation

| Symbol | Description |
| --- | --- |
| `(dag:d-connected? g x y z)` | True when X and Y are d-connected given Z. |
| `(dag:d-separated? g x y z)` | True when X and Y are d-separated given Z. |

## Do-calculus rules

| Symbol | Description |
| --- | --- |
| `(do-rule1-applies? g y x z w)` | Insertion/deletion of observations. |
| `(do-rule2-applies? g y x z w)` | Action/observation exchange. |
| `(do-rule3-applies? g y x z w)` | Insertion/deletion of actions. |

## Back-door and adjustment

| Symbol | Description |
| --- | --- |
| `(dag:satisfies-backdoor? g x y z)` | Test the back-door criterion. |
| `(dag:adjustment-sets g x y)` | Enumerate valid adjustment sets. |
| `(dag:adjustment-sets-observed g x y observed)` | Restrict to observed variables. |

## Identification

| Symbol | Description |
| --- | --- |
| `(do:identify g y x)` | Returns an estimand AST or a failure witness. |
| `(do:identify-details g y x)` | Same with diagnostic details. |
| `(do:identify-observed g y x observed)` | Identify using only observed variables. |
| `(do:identify-details-observed g y x observed)` | Same with diagnostic details. |
| `(do:adjustment-formula->string formula)` | Pretty-print an adjustment formula. ASCII output (`Sum`) on Windows. |

## Numeric estimation (uses observational data)

| Symbol | Description |
| --- | --- |
| `(do:estimate-effect data y x z)` | Adjustment-based ATE estimate. |
| `(do:conditional-mean data y x z)` | Conditional mean E[Y | X=x, Z=z]. |
| `(do:marginal-prob data vars values)` | Empirical joint probability. |

## See also

- [std.causal.adjustment](causal/adjustment.md) - GAC, front-door, IV.
- [std.causal.identify](causal/identify.md) - ID/IDC algorithms.
- [std.causal.estimate](causal/estimate.md) - Modern ATE estimators.

