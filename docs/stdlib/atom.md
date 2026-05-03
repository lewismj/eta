# std.atom

Atomic mutable references with compare-and-set semantics.

```scheme
(import std.atom)
```

## Construction and predicates

| Symbol | Description |
| --- | --- |
| `(atom:new value)` | Create a new atom holding `value`. |
| `(atom:atom? x)` | True when `x` is an atom. |

## Access and update

| Symbol | Description |
| --- | --- |
| `(atom:deref a)` | Read the current value. |
| `(atom:reset! a value)` | Set the value unconditionally. |
| `(atom:swap! a f . args)` | Atomically replace value with `(apply f current args)`. |
| `(atom:compare-and-set! a expected new)` | Set to `new` only if current value equals `expected`. Returns true on success. |

## Aliases

The following Clojure-style names are also exported and refer to the same
procedures: `atom`, `atom?`, `deref`, `reset!`, `swap!`, `compare-and-set!`.

