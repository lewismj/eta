# std.clp

Constraint logic programming over integers (Z) and finite domains (FD), with
labelling and branch-and-bound search.

```scheme
(import std.clp)
```

## Domains

| Symbol | Description |
| --- | --- |
| `(clp:domain var lo hi)` | Restrict `var` to integer interval `[lo, hi]`. |
| `(clp:in-fd var values)` | Restrict `var` to a finite set of values. |
| `(clp:domain-z? d)` | True when `d` is an integer-interval domain. |
| `(clp:domain-fd? d)` | True when `d` is a finite-domain. |
| `(clp:domain-lo d)` | Lower bound. |
| `(clp:domain-hi d)` | Upper bound. |
| `(clp:domain-values d)` | Domain values as a list (FD only). |
| `(clp:domain-empty? d)` | True when domain is empty. |

## Arithmetic constraints

| Symbol | Description |
| --- | --- |
| `(clp:= a b)` | Equality constraint. |
| `(clp:+ x y z)` | `z = x + y`. |
| `(clp:plus-offset x k z)` | `z = x + k`. |
| `(clp:abs x z)` | `z = |x|`. |
| `(clp:* x y z)` | `z = x * y`. |
| `(clp:sum xs z)` | `z = sum(xs)`. |
| `(clp:scalar-product cs xs z)` | `z = sum(c_i * x_i)`. |
| `(clp:element idx xs z)` | `z = xs[idx]`. |

## Relational constraints

| Symbol | Description |
| --- | --- |
| `(clp:<= a b)` | `a <= b`. |
| `(clp:>= a b)` | `a >= b`. |
| `(clp:<> a b)` | Disequality `a /= b`. |
| `(clp:all-different xs)` | All variables in `xs` take distinct values. |

## Search

| Symbol | Description |
| --- | --- |
| `(clp:solve vars)` | Solve the constraint store for the given variables. |
| `(clp:labeling vars [strategy])` | Enumerate solutions; strategies include `'first-fail`. |
| `(clp:minimize expr vars)` | Branch-and-bound minimization. |
| `(clp:maximize expr vars)` | Branch-and-bound maximization. |

## Deprecated

| Symbol | Replacement |
| --- | --- |
| `clp:!=` | `clp:<>`. |

