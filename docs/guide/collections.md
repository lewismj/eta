# Collections

[← Back to Language Guide](../language_guide.md)

This page covers the four primary in-memory collection types — lists,
vectors, hash maps, and hash sets — and the `std.collections` suite of
higher-order combinators that work over them.

---

## Choosing a container

| Need                           | Use                  | Module                                |
| :----------------------------- | :------------------- | :------------------------------------ |
| Cheap prepend / structural rec | List                 | builtin                               |
| O(1) random access, in-place   | Vector               | builtin                               |
| Keyed lookup                   | Hash map             | [`std.hashmap`](./reference/hashmap.md)        |
| Distinct membership            | Hash set             | `std.hashset`                         |
| Columnar relational data       | Fact table           | [`std.fact_table`](./reference/fact-table.md)  |

---

## Lists

A list is `'()` or a pair whose `cdr` is a list. Lists are immutable
when constructed with `cons` / `list` / quoted literals.

```scheme
(cons 1 '(2 3))         ; => (1 2 3)
(append '(a b) '(c d))  ; => (a b c d)
(length '(a b c))       ; => 3
(reverse '(1 2 3))      ; => (3 2 1)
(list-ref '(a b c) 1)   ; => b
```

> [!NOTE]
> `length` and `list-ref` are O(n). For repeated indexing convert to a
> vector with `list->vector`.

---

## Vectors

```scheme
(define v (make-vector 5 0))     ; => #(0 0 0 0 0)
(vector-set! v 2 'x)             ; mutates v
(vector-ref v 2)                 ; => x
(vector-length v)                ; => 5
(vector->list v)                 ; => (0 0 x 0 0)
```

Vector literals: `#(1 2 3)`. Vectors are mutable; `equal?` compares
element-wise.

---

## Hash maps

```scheme
(define m (hash-map))
(hash-map-set! m 'a 1)
(hash-map-set! m 'b 2)
(hash-map-ref  m 'a)             ; => 1
(hash-map-ref  m 'z 'missing)    ; => missing
(hash-map-keys m)                ; => (a b)
```

Full reference: [`hashmap.md`](./reference/hashmap.md).

---

## Hash sets

`std.hashset` adds a small set of helpers around the runtime
`hash-set` primitives:

| Helper                    | Purpose                                  |
| :------------------------ | :--------------------------------------- |
| `hash-set-empty?`         | True if the set has no elements          |
| `hash-set-size`           | Element count                            |
| `hash-set-subset? a b`    | Every element of `a` is in `b`           |
| `hash-set-equal? a b`     | `a` and `b` have the same elements       |

Underlying primitives: `hash-set`, `hash-set-add!`,
`hash-set-contains?`, `hash-set->list`.

---

## `std.collections`

Re-exported by `std.prelude`. The headline combinators:

```scheme
(import std.prelude)

(map*    (lambda (x) (* x x)) '(1 2 3))     ; => (1 4 9)
(filter  even? (range 1 11))                ; => (2 4 6 8 10)
(foldl   + 0 '(1 2 3 4 5))                  ; => 15
(foldr   cons '() '(a b c))                 ; => (a b c)
(reduce  + '(1 2 3 4 5))                    ; => 15

(zip       '(a b c) '(1 2 3))                ; => ((a . 1) (b . 2) (c . 3))
(map2      + '(1 2 3) '(10 20 30))           ; => (11 22 33)
(zip-with  * '(1 2 3) '(4 5 6))              ; => (4 10 18)

(any?      negative? '(1 -2 3))              ; => #t
(every?    positive? '(1 2 3))               ; => #t

(take 3 '(a b c d e))                        ; => (a b c)
(drop 3 '(a b c d e))                        ; => (d e)
(range 1 6)                                  ; => (1 2 3 4 5)
(flatten '((1 2) (3 4) (5)))                 ; => (1 2 3 4 5)
(sort < '(5 3 8 1 4 2))                      ; => (1 2 3 4 5 8)

(map-indexed (lambda (i x) (cons i x)) '(a b c))
;; => ((0 . a) (1 . b) (2 . c))
```

---

## Performance characteristics

| Operation             | List | Vector | Hash map |
| :-------------------- | :--: | :----: | :------: |
| `cons` / prepend      | O(1) |   —    |    —     |
| Index                 | O(n) | O(1)   | O(1)*    |
| Update at index       |  —   | O(1)   | O(1)*    |
| Membership            | O(n) | O(n)   | O(1)*    |
| Length / size         | O(n) | O(1)   | O(1)     |

*Average case; worst case depends on hash distribution.

---

## Related

- [Strings](./strings.md), [I/O](./io.md)
- [`hashmap.md`](./reference/hashmap.md), [`fact-table.md`](./reference/fact-table.md)
- [`csv.md`](./reference/csv.md), [`db.md`](./reference/db.md)


