# Syntax & Values

[← Back to Language Guide](../language_guide.md)

This chapter is the lexical and value-level reference for Eta. It
describes the surface syntax read by the front-end, the kinds of values
the runtime understands, and the equality predicates that compare them.

---

## Lexical structure

| Element        | Form                                    |
| :------------- | :-------------------------------------- |
| Line comment   | `; …`                                   |
| Block comment  | `#\| … \|#` (nestable)                  |
| Datum comment  | `#;<datum>` skips one datum             |
| Shebang        | `#!/usr/bin/env etai` allowed on line 1 |
| Whitespace     | Any Unicode whitespace separates tokens |

Identifiers are case-sensitive. They may contain letters, digits, and
the punctuation characters ``! $ % & * + - . / : < = > ? @ ^ _ ~``,
provided they do not parse as a number. Idiomatic style uses
`kebab-case` for definitions and a trailing `?` for predicates,
`!` for mutators.

---

## Booleans

`#t` and `#f`. **Every value other than `#f` is truthy** (including
`'()`, `0`, and `""`). Use `not` to negate; `boolean?` to test.

---

## Numbers

The numeric tower has two concrete representations:

| Type     | Range / precision                      | Predicate              |
| :------- | :------------------------------------- | :--------------------- |
| Fixnum   | 48-bit signed int (NaN-box payload)    | `integer?`, `fixnum?`  |
| Float    | IEEE-754 double                        | `real?`, `float?`      |

Mixed-arity arithmetic promotes fixnums to floats. Integer overflow
promotes silently to float. There is no native bignum / rational type.

Useful operations: `+ - * / quotient remainder modulo expt abs min max
floor ceiling round truncate sqrt exp log sin cos tan asin acos atan`.

> [!NOTE]
> The NaN-box layout is documented in [`nanboxing.md`](./reference/nanboxing.md);
> in user code the distinction is invisible.

---

## Characters and strings

Characters: `#\a`, `#\space`, `#\newline`, `#\tab`, `#\u03BB` (hex
escape).

Strings: `"…"` with the standard escapes `\" \\ \n \t \r \0 \uXXXX`.
Strings are immutable and indexed in characters, not bytes:

```scheme
(string-length "héllo")       ; => 5
(string-ref "héllo" 1)        ; => #\é
(substring "héllo" 1 4)       ; => "éll"
```

See [`strings.md`](./strings.md) for the full operation set.

---

## Symbols

Written as bare identifiers; created at run time with `string->symbol`.
Symbols are interned — `eq?` is the right equality. A symbol is its own
quote: `'foo` ≡ `(quote foo)`.

---

## Pairs and lists

`(cons a d)` builds a pair; `'()` is the empty list. A *proper list* is
`'()` or a pair whose cdr is a proper list. Lists are the canonical
sequence type; vectors are preferred for random access.

```scheme
(cons 1 2)             ; => (1 . 2)        improper pair
(list 1 2 3)           ; => (1 2 3)
(append '(a b) '(c))   ; => (a b c)
(car '(1 2 3))         ; => 1
(cdr '(1 2 3))         ; => (2 3)
```

---

## Vectors

`#(1 2 3)` reads as a 3-element vector. Vectors are mutable and O(1)
indexed via `vector-ref` / `vector-set!`. Use `make-vector`,
`vector-length`, `vector->list`, `list->vector`.

---

## Quoting

| Reader         | Expansion                  |
| :------------- | :------------------------- |
| `'x`           | `(quote x)`                |
| `` `x ``       | `(quasiquote x)`           |
| `,x`           | `(unquote x)`              |
| `,@x`          | `(unquote-splicing x)`     |

```scheme
(let ((x 10) (xs '(2 3)))
  `(a ,x ,@xs))           ; => (a 10 2 3)
```

---

## Equality

| Predicate | Semantics                                                  |
| :-------- | :--------------------------------------------------------- |
| `eq?`     | Object identity. Always correct for symbols, fixnums, `#t`, `#f`, `'()`. |
| `eqv?`    | Identity + numeric / character equivalence by value.       |
| `equal?`  | Structural recursion through pairs, vectors, strings, records. |

> [!TIP]
> For symbols, `eq?` is sufficient and the fastest choice — that's why
> `cond` clauses such as `(eq? (car form) 'lambda)` are idiomatic.

---

## Truthiness cheat-sheet

```scheme
(if 0   'a 'b)   ; => a    — 0 is truthy
(if ''  'a 'b)   ; => a    — empty list is truthy
(if "" 'a 'b)    ; => a    — empty string is truthy
(if #f 'a 'b)    ; => b    — only #f is falsy
```

---

## Related

- [Bindings & Scope](./bindings-and-scope.md)
- [Strings](./strings.md), [Collections](./collections.md)
- [`nanboxing.md`](./reference/nanboxing.md), [`runtime.md`](./reference/runtime.md)


