# Hash Maps and Hash Sets

[Back to README](../../../README.md) · [Modules and Stdlib](modules.md) · [Next Steps](../../next-steps.md)

---

## Overview

Eta now ships native hash-map and hash-set runtime types with immutable update
operations (`assoc`/`dissoc` style). The core builtins are available in every
module, and `std.hashmap` / `std.hashset` add convenience helpers.

Key points:

- `hash-map-assoc` and `hash-map-dissoc` return new values.
- `hash-map-ref` accepts an optional default value.
- Keys are compared structurally (`equal?`) with one important numeric rule:
  `1` and `1.0` are distinct keys.
- Values can be converted to and from list form via `hash-map->list` /
  `list->hash-map` and `hash-set->list` / `list->hash-set`.

---

## Quick Start

```scheme
(module demo
  (import std.hashmap std.hashset std.io)
  (begin
    (define m (hash-map 'a 1 'b 2))
    (define m2 (hash-map-assoc m 'c 3))
    (println (hash-map-ref m2 'b))
    (println (hash-map-size m2))

    (define s (hash-set 'x 'y))
    (define s2 (hash-set-add s 'z))
    (println (hash-set-contains? s2 'z))
    (println (hash-set-size s2))))
```

---

## Core Hash-Map Builtins

| Function | Signature |
|----------|-----------|
| `hash-map` | `(k1 v1 k2 v2 ...) -> map` |
| `make-hash-map` | `() -> map` |
| `hash-map?` | `(x) -> bool` |
| `hash-map-ref` | `(m k [default]) -> value` |
| `hash-map-assoc` | `(m k v) -> map` |
| `hash-map-dissoc` | `(m k) -> map` |
| `hash-map-keys` | `(m) -> list` |
| `hash-map-values` | `(m) -> list` |
| `hash-map-size` | `(m) -> int` |
| `hash-map->list` | `(m) -> list` |
| `list->hash-map` | `(xs) -> map` |
| `hash-map-fold` | `(f init m) -> value` |
| `hash` | `(x) -> int` |

---

## Core Hash-Set Builtins

| Function | Signature |
|----------|-----------|
| `make-hash-set` | `() -> set` |
| `hash-set` | `(x1 x2 ...) -> set` |
| `hash-set?` | `(x) -> bool` |
| `hash-set-add` | `(s x) -> set` |
| `hash-set-remove` | `(s x) -> set` |
| `hash-set-contains?` | `(s x) -> bool` |
| `hash-set-union` | `(a b) -> set` |
| `hash-set-intersect` | `(a b) -> set` |
| `hash-set-diff` | `(a b) -> set` |
| `hash-set->list` | `(s) -> list` |
| `list->hash-set` | `(xs) -> set` |

---

## stdlib Wrappers

`std.hashmap` adds:

- `hash-map-empty?`
- `hash-map-contains?`
- `hash-map-update`
- `hash-map-update-with-default`
- `hash-map-merge`
- `hash-map-merge-with`
- `hash-map-map`
- `hash-map-filter`
- `hash-map->alist`
- `alist->hash-map`

`std.hashset` adds:

- `hash-set-empty?`
- `hash-set-size`
- `hash-set-subset?`
- `hash-set-equal?`

---

## Notes

- v1 does not add reader literal syntax (`{...}` / `#{...}`).
- Printer output includes `#hashmap{...}` and `#hashset{...}` forms for display.
- Use constructors (`hash-map`, `hash-set`) as canonical source syntax.
