# std.clpr

CLP over real intervals with linear and quadratic optimisation.

```scheme
(import std.clpr)
```

## Domains

| Symbol | Description |
| --- | --- |
| `(clp:real var lo hi)` | Closed interval `[lo, hi]`. |
| `(clp:real-open var lo hi)` | Open interval `(lo, hi)`. |
| `(clp:real-half-open var lo hi lo-open? hi-open?)` | Mixed open/closed bounds. |
| `(clp:domain-r? d)` | True when `d` is a real-interval domain. |
| `(clp:domain-r-lo d)` | Lower bound. |
| `(clp:domain-r-hi d)` | Upper bound. |
| `(clp:domain-r-lo-open? d)` | Lower bound open? |
| `(clp:domain-r-hi-open? d)` | Upper bound open? |
| `(clp:domain-r var)` | Current real domain of `var`. |
| `(clp:r-bounds var)` | `(lo . hi)` of current bounds. |
| `(clp:r-feasible? store)` | True when constraints remain consistent. |

## Linear constraints

| Symbol | Description |
| --- | --- |
| `(clp:r+ x y z)` | `z = x + y`. |
| `(clp:r- x y z)` | `z = x - y`. |
| `(clp:r*scalar k x z)` | `z = k * x`. |
| `(clp:r= a b)` | Equality. |
| `(clp:r<= a b)` | `a <= b`. |
| `(clp:r< a b)` | `a < b`. |
| `(clp:r>= a b)` | `a >= b`. |
| `(clp:r> a b)` | `a > b`. |

## Optimisation

| Symbol | Description |
| --- | --- |
| `(clp:r-minimize objective vars)` | Linear minimisation. |
| `(clp:r-maximize objective vars)` | Linear maximisation. |
| `(clp:rq-minimize quadratic linear vars)` | Quadratic minimisation. |
| `(clp:rq-maximize quadratic linear vars)` | Quadratic maximisation. |

