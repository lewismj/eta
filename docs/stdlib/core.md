# std.core

Core combinators, list accessors, and a platform predicate.

```scheme
(import std.core)
```

## Predicates

| Symbol | Description |
| --- | --- |
| `(atom? x)` | True when `x` is an atomic value (not a pair). |
| `(list? x)` | True when `x` is a proper list. |
| `(windows?)` | True when running on Windows. |

## Combinators

| Symbol | Description |
| --- | --- |
| `(identity x)` | Returns `x`. |
| `(compose f g ...)` | Right-to-left function composition. |
| `(flip f)` | Returns `(lambda (a b) (f b a))`. |
| `(constantly x)` | Returns a function that ignores its arguments and returns `x`. |
| `(negate p)` | Returns `(lambda args (not (apply p args)))`. |
| `(void)` | Returns the unspecified value. |

## List accessors

| Symbol | Description |
| --- | --- |
| `(cadr xs)` | Second element. |
| `(caddr xs)` | Third element. |
| `(caar xs)` | `car` of `car`. |
| `(cdar xs)` | `cdr` of `car`. |
| `(caddar xs)` | `car` of `cddar`. |
| `(last xs)` | Last element of a non-empty list. |
| `(iota n)` | List of integers `0..n-1`. |
| `(assoc-ref alist key)` | Value paired with `key`, or `#f` if absent. |

