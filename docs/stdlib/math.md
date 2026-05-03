# std.math

Mathematical constants and common numeric helpers.

```scheme
(import std.math)
```

## Constants

| Symbol | Description |
| --- | --- |
| `pi` | The constant pi. |
| `e` | Euler's number. |

## Predicates

| Symbol | Description |
| --- | --- |
| `(even? n)` | True when `n` is even. |
| `(odd? n)` | True when `n` is odd. |

## Numeric helpers

| Symbol | Description |
| --- | --- |
| `(square x)` | `(* x x)`. |
| `(cube x)` | `(* x x x)`. |
| `(sign x)` | -1, 0, or 1. |
| `(clamp x lo hi)` | Clamp `x` into `[lo, hi]`. |
| `(quotient a b)` | Truncated integer division. |
| `(gcd a b ...)` | Greatest common divisor. |
| `(lcm a b ...)` | Least common multiple. |
| `(floor x)` | Round toward negative infinity. |
| `(ceiling x)` | Round toward positive infinity. |
| `(truncate x)` | Round toward zero. |
| `(round x)` | Round to nearest, ties to even. |
| `(expt base exp)` | Exponentiation. |
| `(sum xs)` | Sum of a list of numbers. |
| `(product xs)` | Product of a list of numbers. |

