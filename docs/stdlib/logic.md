# std.logic

Prolog/miniKanren-style goal combinators built over the unification
primitives.

```scheme
(import std.logic)
```

## Unification

| Symbol | Description |
| --- | --- |
| `(== a b)` | Unification goal: succeed when `a` and `b` unify. |
| `(==o a b)` | Goal-form alias of `==`. |
| `(copy-term* t)` | Copy a term, replacing logic variables with fresh ones. |

## Goal combinators

| Symbol | Description |
| --- | --- |
| `(succeedo)` | Goal that always succeeds. |
| `(failo)` | Goal that always fails. |
| `(disj g1 g2 ...)` | Logical disjunction. |
| `(conj g1 g2 ...)` | Logical conjunction. |
| `(fresh (vars ...) body ...)` | Introduce fresh logic variables. |
| `(fresh-vars n)` | Build a list of `n` fresh variables. |
| `(conde clause ...)` | Disjunction of conjunction clauses. |
| `(conda clause ...)` | Soft-cut conjunction. |
| `(condu clause ...)` | Committed-choice conjunction. |
| `(onceo g)` | Restrict `g` to its first solution. |
| `(naf g)` | Negation as failure. |
| `(goal thunk)` | Wrap a thunk into a goal. |

## Membership

| Symbol | Description |
| --- | --- |
| `(membero x xs)` | Goal: `x` is a member of list `xs`. |

## Solvers

| Symbol | Description |
| --- | --- |
| `(succeeds? g)` | True when goal `g` has at least one solution. |
| `(findall var g)` | List of all solutions of `var` under `g`. |
| `(run1 vars g)` | First solution. |
| `(run* vars g)` | All solutions. |
| `(run-n n vars g)` | First `n` solutions. |

## Errors

| Symbol | Description |
| --- | --- |
| `(logic-throw tag value)` | Raise a logic-tagged condition. |
| `(logic-catch tag thunk handler)` | Catch a logic-tagged condition. |

