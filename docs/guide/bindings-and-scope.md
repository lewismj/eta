# Bindings & Scope

[← Back to Language Guide](../language_guide.md)

Eta is lexically scoped. This page lists every binding form, the
shadowing rules, and the differences between top-level, module-level,
and REPL behaviour.

---

## `define` and `defun`

`define` introduces a binding in the enclosing scope.

```scheme
(define greeting "hello")
(define (square x) (* x x))     ; sugar for (define square (lambda (x) (* x x)))
```

`defun` is an Eta convenience that mirrors Common Lisp:

```scheme
(defun square (x) (* x x))      ; equivalent to the (define (square x) …) form above
```

Internal `define`s inside the body of a `lambda`, `let`, `let*`,
`letrec`, `when`, `unless`, or `begin` form a mutually recursive group —
they are lifted to a synthetic `letrec` before the first non-define
expression.

---

## `let`, `let*`, `letrec`

| Form     | Scope of init expressions             | When to use                       |
| :------- | :------------------------------------ | :-------------------------------- |
| `let`    | Outer scope only                      | Independent bindings              |
| `let*`   | Each init sees the previous bindings  | Sequential / dependent bindings   |
| `letrec` | All inits see all bindings            | Mutually recursive functions      |

```scheme
(let  ((a 1) (b 2))         (+ a b))   ; => 3
(let* ((a 1) (b (+ a 1)))   (+ a b))   ; => 3
(letrec
  ((even? (lambda (n) (if (= n 0) #t (odd?  (- n 1)))))
   (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
  (even? 42))                          ; => #t
```

---

## Named `let`

A named `let` binds the body as a recursive function — the canonical
loop idiom in Eta:

```scheme
(let loop ((n 10) (acc 1))
  (if (= n 0) acc (loop (- n 1) (* acc n))))   ; => 3628800
```

The recursive call is in tail position — see
[Tail Calls](./tail-calls.md).

---

## `set!`

`set!` mutates an existing binding. It does **not** create one; using
it on an unbound name is a compile-time error in modules and a
`runtime.unbound` error in the REPL.

```scheme
(define n 0)
(set! n (+ n 1))
n                    ; => 1
```

> [!TIP]
> Closures capture the *binding*, not the value, so `set!` inside a
> closure mutates the enclosing variable as expected:
>
> ```scheme
> (defun make-counter ()
>   (let ((n 0))
>     (lambda () (set! n (+ n 1)) n)))
> ```

---

## Top-level vs module-level

A `(module …)` form opens a fresh namespace. Top-level `define`s inside
are private unless re-exported via `(export …)`. Re-defining the same
name twice in a module is a hard error.

```scheme
(module my-mod
  (export public-fn)
  (begin
    (defun helper (x) (* x 2))      ; private
    (defun public-fn (x) (helper (+ x 1)))))
```

---

## REPL shadowing

The REPL allows re-`define` at the prompt, replacing the previous
binding. Imported names can be shadowed by a local `define`:

```scheme
eta> (define + (lambda args 'nope))
eta> (+ 1 2)
=> nope
```

> [!WARNING]
> Shadowing built-ins from the REPL is a deliberate convenience for
> experiments; **do not** redefine standard names in modules.

See [`repl.md`](./reference/repl.md) for `:reset`, `:reload`, and other REPL
commands.

---

## Common pitfalls

- **Forgetting the `letrec` for mutual recursion** — `let` doesn't
  bring siblings into scope, so each `lambda` would close over an
  unbound name.
- **Internal define after a regular expression** — internal defines
  must precede the first non-define form in their containing body.
- **Using `define` inside `cond` branches** — only legal inside `begin`
  or a body context; wrap the branch in `(begin …)` if needed.

---

## Related

- [Functions, Closures & Tail Calls](./functions-and-closures.md)
- [Control Flow](./control-flow.md)
- [Modules](./reference/modules.md), [REPL](./reference/repl.md)


