# Functions and Closures

[← Back to Language Guide](./language_guide.md)

Functions are first-class values — they can be stored, passed, and
returned. Eta is a Lisp-1: functions and other values share a single
namespace.

---

## Defining functions

```scheme
(define square (lambda (x) (* x x)))   ; lambda form
(defun  square (x) (* x x))            ; defun shorthand
(define (square x) (* x x))            ; sugar form
```

All three forms produce equivalent closures.

---

## Parameter lists

| Form                         | Meaning                                       |
| :--------------------------- | :-------------------------------------------- |
| `(a b c)`                    | Fixed arity 3                                 |
| `(a . rest)`                 | One required arg, rest collected as a list    |
| `args`                       | Variadic; all args collected as `args`        |

```scheme
(defun greet (name . titles)
  (println (string-append
             "Hello, "
             (if (null? titles) "" (car titles))
             " " name)))

(greet "Ada")              ; Hello,  Ada
(greet "Ada" "Dr.")        ; Hello, Dr. Ada
```

---

## `apply`

Calls a function with a list of arguments — the inverse of dotted-rest
collection:

```scheme
(apply + '(1 2 3 4))                    ; => 10
(apply max 0 '(3 1 4 1 5 9 2 6))        ; => 9
```

The last argument to `apply` must be a (possibly empty) list; preceding
arguments are passed positionally.

---

## Closures and upvalues

A `lambda` captures every free variable from its lexical environment by
reference. The capture is the *binding*, so `set!` from inside the
closure is visible outside:

```scheme
(defun make-adder (n)
  (lambda (x) (+ n x)))

(define add5 (make-adder 5))
(add5 7)                                ; => 12
```

> [!NOTE]
> Closures are heap-allocated objects with an explicit upvalue array;
> the layout is described in [`bytecode-vm.md`](./reference/bytecode-vm.md). For
> common cases (single small upvalue, no captures) the compiler emits a
> compact representation.

---

## Higher-order patterns

`std.collections` (re-exported by `std.prelude`) provides the standard
combinators:

```scheme
(import std.prelude)
(map*    (lambda (x) (* x x)) '(1 2 3))     ; => (1 4 9)
(filter  even? (range 1 11))                ; => (2 4 6 8 10)
(foldl   + 0 '(1 2 3 4 5))                  ; => 15
(reduce  + '(1 2 3 4 5))                    ; => 15
```

Combinators on functions themselves:

```scheme
(define inc-then-double
  (compose (lambda (x) (* 2 x))
           (lambda (x) (+ x 1))))
(inc-then-double 5)                          ; => 12

(filter (negate even?) (range 1 11))         ; => (1 3 5 7 9)
((flip cons) '(1 2 3) 0)                     ; => (0 1 2 3)
((constantly 42) 'whatever)                  ; => 42
```

---

## Currying

Eta does not auto-curry; emulate it manually:

```scheme
(defun add (a) (lambda (b) (+ a b)))
(map* (add 10) '(1 2 3))                    ; => (11 12 13)
```

---

## Performance notes

- Each `lambda` may allocate a closure object. Inside hot loops,
  prefer `letrec` for the loop function so the recursion does not
  re-allocate per iteration.
- The peephole optimiser folds `(apply f args)` to a direct call when
  `args` is a literal list of known length.
- See [`optimisations.md`](./reference/optimisations.md) for the full pass list.

---

## Related

- [Tail Calls](./tail-calls.md)
- [Bindings & Scope](./bindings-and-scope.md)
- [Macros](./macros.md)

