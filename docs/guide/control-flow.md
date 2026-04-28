# Control Flow

[← Back to Language Guide](./language_guide.md)

Eta inherits the standard Scheme conditional and sequencing forms.
There is no built-in iteration construct — loops are written as tail
recursion (see [Tail Calls](./tail-calls.md)).

---

## `if`

```scheme
(if test then-expr else-expr)
(if test then-expr)              ; one-armed; returns unspecified on #f
```

The else branch is mandatory in expression contexts where the value is
used; otherwise it returns the unspecified value.

---

## `cond`

```scheme
(cond
  ((null? xs)        'empty)
  ((= (length xs) 1) 'singleton)
  ((pair? xs)        'many)
  (else              'other))
```

Each clause's body is in tail position when `cond` itself is. The arrow
form `(test => proc)` is supported and applies `proc` to the test
value:

```scheme
(cond ((assv key alist) => cdr)
      (else 'not-found))
```

---

## `when` / `unless`

Single-armed conditionals with an implicit `(begin …)` body:

```scheme
(when (> n 0)
  (println "positive")
  (do-something n))

(unless (null? xs)
  (process (car xs)))
```

---

## `case`

Dispatch on `eqv?` of a key against a list of literal data:

```scheme
(case opcode
  ((push pop)        'stack-op)
  ((add sub mul div) 'arith)
  (else              'other))
```

---

## `and` / `or`

Short-circuit; return the deciding value (not necessarily a boolean).

```scheme
(and 1 2 3)    ; => 3
(or  #f 7 9)   ; => 7
(or  #f #f)    ; => #f
```

---

## `begin`

Sequences expressions; evaluates to the last. Implicit inside
`when`, `unless`, `lambda`, `let*`, `letrec`, `cond`-clause bodies, and
the body of `module`.

```scheme
(begin
  (println "step 1")
  (println "step 2")
  42)
```

---

## Loops

There is no `while` or `for`. The idiomatic loop is a tail-recursive
named `let`:

```scheme
(let loop ((i 0) (acc 0))
  (if (= i 10)
      acc
      (loop (+ i 1) (+ acc i))))     ; => 45
```

For side-effecting iteration over a list, use `for-each` from
`std.collections`:

```scheme
(for-each println '(a b c))
```

---

## Early exit

Combine `catch` and `raise` to break out of deep recursion:

```scheme
(defun first-negative (lst)
  (catch 'found
    (let loop ((xs lst))
      (cond
        ((null? xs)     #f)
        ((< (car xs) 0) (raise 'found (car xs)))
        (else           (loop (cdr xs)))))))
```

See [Error Handling](./error-handling.md) for the full model.

---

## Related

- [Functions, Closures & Tail Calls](./functions-and-closures.md)
- [Tail Calls](./tail-calls.md)
- [Error Handling](./error-handling.md)

