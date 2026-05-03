# Atom

[Back to README](../../../README.md) · [Modules and Stdlib](modules.md) · [Next Steps](../../next-steps.md)

---

## Overview

`std.atom` provides a single-cell mutable reference with compare-and-set (CAS)
semantics.

Key points:

- A value is wrapped with `atom` / `atom:new`.
- Read with `deref` / `atom:deref`.
- Write with `reset!` / `atom:reset!`.
- CAS with `compare-and-set!` / `atom:compare-and-set!`.
- Update loops with `swap!` / `atom:swap!`.

`std.atom` is not re-exported by `std.prelude`. Import it explicitly.

---

## Quick Start

```scheme
(module demo
  (import std.atom std.io)
  (begin
    (define counter (atom 0))
    (println (deref counter))                          ;; 0
    (println (swap! counter (lambda (x) (+ x 1))))    ;; 1
    (println (compare-and-set! counter 1 10))         ;; #t
    (println (deref counter))))                        ;; 10
```

---

## API

Collision-safe exports:

| Function | Signature | Description |
|----------|-----------|-------------|
| `atom:new` | `(value) -> atom` | Create a new atom with initial `value`. |
| `atom:atom?` | `(x) -> bool` | True iff `x` is an atom. |
| `atom:deref` | `(a) -> value` | Read the current atom value. |
| `atom:reset!` | `(a value) -> value` | Store `value` and return it. |
| `atom:swap!` | `(a fn arg ...) -> value` | Compute and install a new value by applying `fn` to old value plus extra args. |
| `atom:compare-and-set!` | `(a old new) -> bool` | CAS from `old` to `new`; returns `#t` on success, `#f` otherwise. |

Clojure-style aliases (same behavior):

`atom`, `atom?`, `deref`, `reset!`, `swap!`, `compare-and-set!`

---

## Semantics

- `compare-and-set!` uses raw `LispVal` equality (bitwise identity/value), not
  deep structural `equal?`.
- `swap!` is a CAS retry loop. Under contention, the callback may be invoked
  more than once before one CAS succeeds.
- The atom cell is GC-traced as a strong reference.

---

## Import Notes

`std.core` already exports `atom?` with Scheme semantics ("not a pair").
When both modules are needed, prefer the collision-safe names from `std.atom`
or use filtered imports:

```scheme
(import std.core)
(import (only std.atom atom:new atom:deref atom:swap! atom:compare-and-set!))
```

