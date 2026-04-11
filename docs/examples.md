# Eta — Language Examples 

A tour of the example programs in [`examples/`](../examples/).  Each file
is a self-contained module that can be run directly:

```bash
etai examples/hello.eta
```

---

## Quick Reference

| Example                                            | Key Concepts |
|----------------------------------------------------|-------------|
| [`causal_demo.eta`](#causal-demo--causal-neural-factor-analysis) | **Flagship** — symbolic diff, do-calculus, findall+CLP, libtorch NN, causal ATE |
| [`hello.eta`](#helloeta)                           | Minimal program, `println`, `defun`, recursion |
| [`basics.eta`](#basicseta)                         | Arithmetic, booleans, `if`/`cond`, `let`/`let*`, strings, pairs, lists, records, quoting |
| [`functions.eta`](#functionseta)                   | `defun`, `lambda`, closures, tail recursion, variadic args, `letrec` |
| [`higher-order.eta`](#higher-ordereta)             | `map*`, `filter`, `foldl`/`foldr`, `reduce`, `sort`, `zip`, `take`/`drop`, `range` |
| [`composition.eta`](#compositioneta)               | `compose`, `flip`, `constantly`, `negate`, manual currying, pipelines |
| [`recursion.eta`](#recursioneta)                   | Fibonacci, list reversal, deep flatten, Ackermann, Towers of Hanoi |
| [`exceptions.eta`](#exceptionseta)                 | `catch`/`raise`, tag specificity, structured payloads, `dynamic-wind` cleanup, re-raising |
| [`boolean-simplifier.eta`](#boolean-simplifiereta) | Symbolic tree rewriting, De Morgan's laws, fixed-point simplification |
| [`symbolic-diff.eta`](#symbolic-differentiation)   | Computer algebra: differentiation rules, algebraic simplification |
| [`aad.eta`](#aadeta)                               | Reverse-mode AD, closures as backpropagators, `define-syntax`, `grad` |
| [`xva.eta`](#xvaeta)                               | Quantitative finance: CVA, FVA, xVA sensitivities via AAD |
| [`european.eta`](#europeaneta)                     | BS option Greeks: first & second order via AAD, custom VJP, Schwarz check |
| [`sabr.eta`](#sabreta)                             | SABR vol surface with tape-based AD, Hagan approximation, Greeks |
| [`logic.eta`](#logiceta)                           | Relational logic programming: `parento`, `grandparento`, `membero`, bidirectional queries |
| [`modules and imports`](#imports)                  | `import`, `export`, `only`, `except`, `rename`, `prefix` |

---

## [causal demo — Causal Neural Factor Analysis](../examples/causal_demo.eta)

The flagship example that threads **all four pillars** of Eta through a
single quantitative-finance pipeline:

1. **Symbolic Processing** — factor model as S-expressions, symbolic
   differentiation, algebraic simplification
2. **Causal Reasoning** — DAG definition, `do:identify` derives back-door
   adjustment formula
3. **Logic & CLP** — `findall` + backtracking enumerates valid adjustment
   sets; `clp:domain` validates probability weights
4. **libtorch Integration** — trains a neural network to learn
   E[return | beta, sector]
5. **Integration** — plugs NN predictions into the causal formula to
   compute the Average Treatment Effect (ATE ≈ 0.45)

Each section's output feeds the next, forming a dependency chain.

```console
etac -O examples/causal_demo.eta -o causal_demo.etac
etai causal_demo.etac
```

> [!TIP]
> See the full [Causal Neural Factor Analysis walkthrough](causal-factor.md)
> for detailed commentary on each step, including how the VM handles
> unification, trail management, CLP forward checking, and libtorch
> tensor objects.

---

## [hello world](../examples/hello.eta)

The canonical first program.

```scheme
(module hello
  import std.io)
  (begin
    (println "Hello, world!")

    (defun factorial (n)
      (if (= n 0) 1
          (* n (factorial (- n 1)))))

    (println (factorial 20))))
```

```
Hello, world!
2432902008176640000
```

---

## [basics](../examples/basics.eta)

Core language features: values, expressions, bindings, and data structures.

```scheme
;; Arithmetic — variadic
(+ 1 2 3)            ; => 6
(* 2 3 4)            ; => 24

;; Conditionals
(if (> 3 2) "yes" "no")   ; => "yes"

(cond
  ((= 1 2) "nope")
  ((= 1 1) "match!")
  (#t      "default"))    ; => "match!"

;; Bindings
(let ((a 10) (b 20))
  (+ a b))                ; => 30

(let* ((a 1) (b (+ a 1)) (c (+ a b)))
  c)                      ; => 4

;; Pairs & lists
(cons 1 2)                ; => (1 . 2)
(list 1 2 3 4 5)          ; => (1 2 3 4 5)
(append '(a b) '(c d))    ; => (a b c d)

;; Records — define-record-type
;; Syntax: name, constructor call, predicate, (field accessor) …
(define-record-type <point>
  (make-point x y)
  point?
  (x point-x)
  (y point-y))

(define p (make-point 3 4))
(point? p)                ; => #t
(point-x p)               ; => 3
(point-y p)               ; => 4
(pair? p)                 ; => #f   (records are not pairs)

;; Mutable fields — add a setter name after the accessor
(define-record-type <counter>
  (make-counter value)
  counter?
  (value counter-value set-counter-value!))

(define c (make-counter 0))
(set-counter-value! c (+ (counter-value c) 1))
(counter-value c)         ; => 1
```

---

## [functions](../examples/functions.eta)

Functions, closures, and recursion — the heart of any Lisp.

```scheme
;; Named function
(defun square (x) (* x x))
(square 7)                    ; => 49

;; Tail-recursive factorial
(defun factorial-tr (n)
  (letrec ((go (lambda (n acc)
                 (if (= n 0) acc
                     (go (- n 1) (* acc n))))))
    (go n 1)))
(factorial-tr 20)             ; => 2432902008176640000

;; Closures — make-adder returns a new function
(defun make-adder (n)
  (lambda (x) (+ n x)))

(define add5 (make-adder 5))
(add5 3)                      ; => 8

;; Variadic (rest) args
(defun sum-all (first . rest)
  ...)
(sum-all 1 2 3 4 5)          ; => 15

;; Mutual recursion via letrec
(letrec
  ((is-even? (lambda (n)
     (if (= n 0) #t (is-odd? (- n 1)))))
   (is-odd? (lambda (n)
     (if (= n 0) #f (is-even? (- n 1))))))
  (is-even? 42))             ; => #t
```

---

## [higher order functions](../examples/higher-order.eta)

Map, filter, fold, and friends from `std.collections`.

```scheme
(import std.prelude)

(define nums (list 1 2 3 4 5 6 7 8 9 10))

;; Transform
(map* square nums)               ; => (1 4 9 16 25 36 49 64 81 100)
(map* (lambda (x) (* x 10)) '(1 2 3))  ; => (10 20 30)

;; Select
(filter even? nums)              ; => (2 4 6 8 10)
(filter (lambda (x) (> x 5)) nums)     ; => (6 7 8 9 10)

;; Accumulate
(foldl + 0 nums)                 ; => 55
(foldl * 1 '(1 2 3 4 5))        ; => 120
(foldr cons '() '(a b c))       ; => (a b c)
(reduce + '(1 2 3 4))           ; => 10

;; Combine: sum of squares of even numbers
(foldl + 0
  (map* square
    (filter even? nums)))        ; => 220

;; Predicates
(any? negative? '(1 -2 3))      ; => #t
(every? positive? '(1 2 3))     ; => #t

;; Utilities
(zip '(a b c) '(1 2 3))         ; => ((a . 1) (b . 2) (c . 3))
(take 3 nums)                   ; => (1 2 3)
(drop 7 nums)                   ; => (8 9 10)
(range 1 6)                     ; => (1 2 3 4 5)
(flatten '((1 2) (3 4) (5)))    ; => (1 2 3 4 5)

;; Sorting (merge sort)
(sort < '(5 3 8 1 4 2 7 6))     ; => (1 2 3 4 5 6 7 8)
```

---

## [currying and composition](../examples/composition.eta)

Currying, composition, and functional pipelines.

```scheme
(import std.prelude)

;; compose — (f ∘ g)(x) = f(g(x))
(define inc-then-double
  (compose (lambda (x) (* 2 x))
           (lambda (x) (+ x 1))))
(inc-then-double 5)             ; => 12

;; flip — swap argument order
(define rcons (flip cons))
(rcons '(1 2 3) 0)             ; => (0 1 2 3)

;; constantly — ignore all args, return a fixed value
(define always-42 (constantly 42))
(always-42 'anything)           ; => 42

;; Manual currying — return closures
(defun add (a) (lambda (b) (+ a b)))
(defun mul (a) (lambda (b) (* a b)))

(define double (mul 2))
(define triple (mul 3))
(map* double '(1 2 3 4 5))     ; => (2 4 6 8 10)
(map* triple '(1 2 3 4 5))     ; => (3 6 9 12 15)

;; Compose curried functions into a pipeline
(define double-then-inc (compose (add 1) (mul 2)))
(map* double-then-inc '(1 2 3 4 5))  ; => (3 5 7 9 11)

;; negate — flip a predicate
(filter (negate even?) (range 1 11))  ; => (1 3 5 7 9)
```

---

## [recursion](../examples/recursion.eta)

Classic recursive algorithms.

```scheme
;; Fibonacci — naive
(defun fib (n)
  (cond
    ((= n 0) 0)
    ((= n 1) 1)
    (#t (+ (fib (- n 1)) (fib (- n 2))))))
(fib 10)                        ; => 55

;; Fibonacci — tail-recursive (linear time)
(defun fib-fast (n)
  (letrec ((go (lambda (i a b)
                 (if (= i 0) a
                     (go (- i 1) b (+ a b))))))
    (go n 0 1)))
(fib-fast 30)                   ; => 832040

;; Deep flatten
(defun deep-flatten (xs)
  (cond
    ((null? xs) '())
    ((pair? (car xs))
     (append (deep-flatten (car xs))
             (deep-flatten (cdr xs))))
    (#t (cons (car xs) (deep-flatten (cdr xs))))))
(deep-flatten '((1 (2 3)) (4) 5))  ; => (1 2 3 4 5)

;; Ackermann function
(defun ackermann (m n)
  (cond
    ((= m 0) (+ n 1))
    ((= n 0) (ackermann (- m 1) 1))
    (#t (ackermann (- m 1) (ackermann m (- n 1))))))
(ackermann 3 4)                 ; => 125

;; Tower of Hanoi (prints moves)
(defun hanoi (n from to via) ...)
(hanoi 3 'A 'C 'B)
;; Move disk 1 from A to C
;; Move disk 2 from A to B
;; Move disk 3 from A to C
;; ...
```

---

## [exceptions](../examples/exceptions.eta)

Eta's exception system is built on two special forms that compile
directly to `SetupCatch` / `Throw` VM opcodes:

| Form | Meaning |
|------|---------|
| `(raise 'tag value)` | Signal an exception; unwind the stack to the nearest matching `catch` |
| `(catch 'tag body)` | Evaluate `body`; if a `raise` with `'tag` escapes, return its payload |
| `(catch body)` | Catch-all: intercepts any `raise` regardless of tag |

Tags are ordinary symbols and serve as typed exception channels.  The
raised value can be any Eta value — a number, string, pair, or record.

### Basic catch / raise

```scheme
;; Normal completion — catch is transparent
(catch 'err 42)                          ; => 42

;; Exception signalled — catch returns the raised payload
(catch 'err (raise 'err "oops!"))        ; => "oops!"

;; Numeric payload
(catch 'math-error (raise 'math-error 404))  ; => 404

;; Catch-all — no tag, intercepts everything
(catch (raise 'anything "caught!"))      ; => "caught!"
```

### Tag specificity and nested handlers

Handlers are matched from the **inside out**.  An inner `catch` with the
correct tag fires first; a mismatched inner handler lets the raise
propagate to the next outer one:

```scheme
;; Inner handler fires; outer never sees the exception
(catch 'outer
  (+ 10 (catch 'inner
           (raise 'inner 5))))           ; => 15

;; Inner handler is for the wrong tag — raise propagates to catch-all
(catch
  (catch 'other-tag
    (raise 'real-tag "bypassed inner"))) ; => "bypassed inner"
```

### Propagation through function calls

`raise` performs a **non-local exit**, unwinding any number of call
frames back to the matching `catch`.  This makes it straightforward to
implement safe wrappers:

```scheme
(defun safe-divide (a b)
  (if (= b 0)
      (raise 'division-by-zero 'undefined)
      (/ a b)))

(catch 'division-by-zero (safe-divide 10 2))   ; => 5
(catch 'division-by-zero (safe-divide 10 0))   ; => undefined
```

### Structured payloads

The raised value can be any data structure.  Using a pair `(code . detail)`
gives callers a typed, inspectable error object:

```scheme
(defun validate-age (age)
  (cond
    ((< age 0)   (raise 'validation-error (cons 'negative age)))
    ((> age 150) (raise 'validation-error (cons 'too-large age)))
    (#t          age)))

(catch 'validation-error (validate-age 25))    ; => 25

(let ((err (catch 'validation-error (validate-age -5))))
  (car err)   ; => negative
  (cdr err))  ; => -5

(let ((err (catch 'validation-error (validate-age 200))))
  (car err)   ; => too-large
  (cdr err))  ; => 200
```

### Early-exit pattern

Raising from inside a loop or recursion is an efficient way to
**short-circuit** traversal once a result is found:

```scheme
(defun first-negative (lst)
  (catch 'found
    (letrec ((loop (lambda (xs)
                     (cond
                       ((null? xs)     #f)
                       ((< (car xs) 0) (raise 'found (car xs)))
                       (#t             (loop (cdr xs)))))))
      (loop lst))))

(first-negative '(3 1 4 1 5))    ; => #f
(first-negative '(3 1 -4 2 -1)) ; => -4
```

### Resource cleanup with `dynamic-wind`

`dynamic-wind` guarantees its *after* thunk runs even when an exception
escapes the body — enabling reliable resource cleanup (file handles,
locks, database connections):

```scheme
(define cleanup-called #f)

(catch 'resource-error
  (dynamic-wind
    (lambda () (set! cleanup-called #f))       ; before: initialise
    (lambda () (raise 'resource-error "boom")) ; body: raises
    (lambda () (set! cleanup-called #t))))     ; after: ALWAYS runs

cleanup-called  ; => #t
```

### Re-raising (exception chaining)

An inner handler can inspect, wrap, and re-raise a new exception,
translating low-level errors into higher-level domain errors:

```scheme
(defun process (x)
  (catch 'low-level
    (if (< x 0)
        (raise 'low-level (list 'bad-input x))
        (* x 2))))

(let ((result
       (catch 'high-level
         (let ((v (process -3)))
           ;; v holds the intercepted low-level payload; wrap and re-raise
           (raise 'high-level (cons 'wrapped v))))))
  (car result)          ; => wrapped
  (car (cdr result)))   ; => bad-input
```

### Implementation notes

`catch`/`raise` compile to the `SetupCatch` and `Throw` VM opcodes.
Each `catch` frame records the tag symbol, the handler program counter,
and a snapshot of the frame stack, so `raise` can restore execution
state in O(depth) time.  The catch stack is separate from the logic
trail, so `unwind-trail` and exception handling compose cleanly —
backtracking does not accidentally discard live exception handlers.

---

## [boolean simplifier](../examples/boolean-simplifier.eta)

A symbolic boolean simplifier that rewrites expression trees using
algebraic identities, De Morgan's laws, and double-negation elimination.
Runs in a fixed-point loop until the expression is fully simplified.

```scheme
;; One-step simplifier handles: not, and, or
(defun simplify-bool (e)
  (cond
    ((atom? e) e)
    ;; ¬(¬x) = x
    ;; ¬(a ∧ b) = (¬a) ∨ (¬b)   — De Morgan
    ;; x ∧ ⊤ = x,  x ∧ ⊥ = ⊥
    ;; x ∨ ⊥ = x,  x ∨ ⊤ = ⊤
    ...))

;; Fixed-point wrapper
(defun simplify-bool* (e)
  (let ((s (simplify-bool e)))
    (if (equal? s e) s (simplify-bool* s))))
```

```
x ∧ ⊤         = x
x ∧ ⊥         = #f
(⊤ ∧ x) ∨ ⊥   = x
¬(¬y)         = y
¬(a ∧ b)      = (or (not a) (not b))
¬(a ∨ b)      = (and (not a) (not b))
(x ∨ x)       = x
¬(¬(p ∧ q))   = (and p q)
```

---

## [symbolic differentiation](../examples/symbolic-diff.eta)

A small computer algebra system: symbolic differentiation with sum,
product, power, and chain rules, plus an algebraic simplifier with
constant folding, identity elimination, and like-term combining.

```scheme
;; Differentiate symbolically
(defun diff (e v) ...)          ; sum, product, power, chain rules
;; Simplify algebraically
(defun simplify* (e) ...)       ; fixed-point: 0+x→x, 1*x→x, fold constants

;; Convenience: differentiate + simplify
(defun D (expr var)
  (simplify* (diff expr var)))
```

```
d/dx (x² + 3)      = (* 2 x)
d/dx (x·(x + 3))   = (+ (+ x 3) x)
d/dx sin(x²)       = (* (cos (* x x)) (* 2 x))
d/dx (x + 1)³      = (* 3 (^ (+ x 1) 2))
d/dx exp(2x)       = (* (exp (* 2 x)) 2)
d/dx log(x)        = (/ 1 x)
d/dx 42            = 0
d/dx y (wrt x)     = 0
```

---

## [aad — adjoint algorithmic differentiation](../examples/aad.eta)

A tape-based reverse-mode AD library that computes exact gradients.
The VM transparently records arithmetic onto a flat tape when TapeRef
operands are detected — zero closure allocations.
See the full [AAD walkthrough](aad.md) for detailed commentary and
worked examples.

```scheme
;; grad: compute gradient of f at a given point
;; The tape records +, -, *, /, sin, cos, exp, log, sqrt transparently
(grad (lambda (x y) (+ (* x y) (sin x))) '(2 3))
;; => (8.909.. #(2.583.. 2))
;;    primal = 2*3 + sin(2)
;;    ∂f/∂x = y + cos(x) = 3 + cos(2) ≈ 2.584
;;    ∂f/∂y = x = 2

;; Rosenbrock function: gradient at (1,1) = (0,0) — global minimum
(grad (lambda (x y)
        (+ (* (- 1 x) (- 1 x))
           (* 100 (* (- y (* x x)) (- y (* x x))))))
      '(1 1))
;; => (0 #(0 0))
```

---

## [xva — valuation adjustments with AAD](../examples/xva.eta)

Builds on the AD library to compute **CVA** (Credit Valuation
Adjustment) and **FVA** (Funding Valuation Adjustment . See the full
[xVA walkthrough](xva.md) for detailed commentary.

```scheme
;; Financial building blocks — all AD-aware
(defun discount-factor (r t)        ;; DF = e^{-rt}
  (dexp (d* (d* -1 r) t)))
(defun survival-prob (hazard-rate t) ;; Q = e^{-λt}
  (dexp (d* (d* -1 hazard-rate) t)))
(defun expected-exposure (notional sigma t)  ;; EE ≈ Nσ√t
  (d* notional (d* sigma (dsqrt t))))

;; CVA = LGD × Σ EE(t) × DF(t) × ΔPD(t)
;; FVA = Σ EE(t) × DF(t) × s_f × Δt
;; Total xVA = CVA + FVA

;; One backward pass → all 6 sensitivities
(grad (lambda (notional sigma r hazard-rate lgd funding-spread)
        (total-xva notional sigma r hazard-rate lgd funding-spread))
      '(1000000 0.20 0.05 0.02 0.60 0.012))
;; => (xva-value  #(∂/∂N  ∂/∂σ  ∂/∂r  ∂/∂λ  ∂/∂LGD  ∂/∂s_f))
```

---

## [european — Black-Scholes Greeks with AAD](../examples/european.eta)

Computes first- and second-order option Greeks using tape-based AD.
The normal CDF uses a polynomial approximation through which the tape
records and differentiates automatically.  See the full
[European Greeks walkthrough](european.md) for detailed commentary.

```scheme
;; Normal CDF — tape records every arithmetic step automatically
(defun norm-cdf (x)
  (let ((xv (if (tape-ref? x) (tape-ref-value x) x)))
    (if (< xv 0)
        (- 1.0 (norm-cdf (* -1 x)))
        (let ((t (/ 1.0 (+ 1.0 (* 0.2316419 x)))))
          (- 1.0 (* (* inv-sqrt-2pi (exp (* -0.5 (* x x))))
                     (* t (+ 0.319381530 ...))))))))

;; Black-Scholes: C = S·Φ(d₁) − K·e⁻ʳᵀ·Φ(d₂)
(defun bs-call-price (S K r sigma T) ...)

;; First-order Greeks — one backward pass
(grad (lambda (S K r sigma T)
        (bs-call-price S K r sigma T))
      '(100.0 90.0 0.03 0.30 0.5))
;; => (14.88  #(Delta  ∂C/∂K  Rho  Vega  −Theta))

;; Second-order Greeks — grad applied to Delta expression
(defun bs-delta-fn (S K r sigma T)
  (norm-cdf (bs-d1 S K r sigma T)))

(grad (lambda (S K r sigma T)
        (bs-delta-fn S K r sigma T))
      '(100.0 90.0 0.03 0.30 0.5))
;; => (0.75  #(Gamma  ...  ...  Vanna  ...))
```

---

## [sabr — SABR vol surface with tape-based AD](../examples/sabr.eta)

Computes the Hagan et al. (2002) SABR implied volatility approximation
and all model sensitivities using Eta's **tape-based reverse-mode AD**.
See the full [SABR walkthrough](sabr.md) for detailed commentary.

```scheme
;; Tape-based AD — plain arithmetic, transparent recording
(defun ndpow (a b) (exp (* b (log a))))

;; SABR ATM vol: σ = α/F^(1-β) × [1 + correction × T]
(defun sabr-atm-vol (F T alpha beta rho nu) ...)

;; All 4 sensitivities in one backward pass
(grad (lambda (alpha beta rho nu)
         (sabr-implied-vol F K T alpha beta rho nu))
       (list alpha-val beta-val rho-val nu-val))
;; => (sigma  #(∂σ/∂α  ∂σ/∂β  ∂σ/∂ρ  ∂σ/∂ν))
```

> [!TIP]
> **Why tape-based AD?**  The SABR formula has ~50 elementary ops on 4
> scalar params.  The tape records each op at the C++ level (~32 bytes,
> zero closure allocations).  A single reverse sweep yields all 4 Greeks.

---

## [logic relations](../examples/logic.eta)

Relational logic programming built on Eta's native unification engine.
This example introduces the key abstraction over the raw `findall`/`db-branches`
pattern shown in [`unification.eta` §8](../examples/unification.eta) — wrapping a
fact database as a **named relation function** that returns goal branches, so the
same function works in all query directions without any changes to the search engine.

> [!TIP]
> **See also:** [Logic Programming — `std.logic` reference](logic.md) for the
> full documentation of `findall`, `run1`, `succeeds?`, `membero`, and the
> underlying trail/unification primitives.

### The relation abstraction

Instead of inlining the database traversal every time:

```scheme
;; ── Raw (unification.eta style) ─────────────────────────────────────────────
(let* ((pv (logic-var))
       (cv (logic-var))
       (_ (== pv 'tom))
       (sols
         (findall (lambda () (deref-lvar cv))
                  (map* (lambda (fact)              ; traversal embedded at call site
                           (lambda ()
                             (and (== pv (car fact))
                                  (== cv (cadr fact)))))
                        parent-db))))
  (println sols))
; => (bob liz)
```

name the traversal once as a **relation**:

```scheme
;; ── Relational (logic.eta style) ────────────────────────────────────────────
(define parent-db
  '((tom bob) (tom liz) (bob ann) (bob pat) (pat jim)))

;; parento returns one goal branch per fact.
;; p or c may be a logic variable (wildcard) or a concrete value (filter).
(defun parento (p c)
  (map* (lambda (fact)
           (lambda ()
             (and (== p (car fact))
                  (== c (cadr fact)))))
         parent-db))
```

The engine — `findall` + trail mark/unwind — is identical.  Only the
*abstraction boundary* moves: the caller sees a reusable relation, not raw loops.

### Bidirectional queries

Because `parento` tests membership by unification, the **same function** acts
as a forward lookup, backward lookup, or full enumeration depending solely on
which arguments are free:

```scheme
;; Forward — who are tom's children?
(let* ((cv (logic-var)))
  (findall (lambda () (deref-lvar cv)) (parento 'tom cv)))
; => (bob liz)

;; Backward — who are ann's parents?
(let* ((pv (logic-var)))
  (findall (lambda () (deref-lvar pv)) (parento pv 'ann)))
; => (bob)

;; Unconstrained — enumerate all pairs
(let* ((pv (logic-var)) (cv (logic-var)))
  (findall (lambda () (cons (deref-lvar pv) (deref-lvar cv)))
           (parento pv cv)))
; => ((tom . bob) (tom . liz) (bob . ann) (bob . pat) (pat . jim))

;; Membership test — does parent(tom, liz) hold?
(succeeds? (lambda () (run1 (lambda () #t) (parento 'tom 'liz))))
; => #t
```

### `membero` — nondeterministic list membership

`(import std.logic)` now exports `membero`:

```scheme
;; membero returns one branch per element; findall collects successful ones
(let* ((x (logic-var)))
  (findall (lambda () (deref-lvar x)) (membero x '(a b c d))))
; => (a b c d)

;; Compose with an arithmetic guard — keep only even elements
(let* ((x (logic-var)))
  (findall (lambda () (deref-lvar x))
           (map* (lambda (branch)
                    (lambda ()
                      (and (branch) (= (mod (deref-lvar x) 2) 0))))
                 (membero x '(1 2 3 4 5 6)))))
; => (2 4 6)
```

### Derived relations — `grandparento`

Relations compose by materialising intermediate results with an inner `findall`,
then flattening the per-intermediate branch lists.  The trail is clean between
the two stages:

```scheme
;; grandparent(GP, GC) :- parent(GP, Mid), parent(Mid, GC).
(defun grandparento (gp gc)
  (let* ((mid  (logic-var))
         (mids (findall (lambda () (deref-lvar mid))
                        (parento gp mid))))    ; stage 1 — materialise Mid values
    (flatten
      (map* (lambda (mid-val)
               (parento mid-val gc))           ; stage 2 — per-mid branches for GC
             mids))))

;; All grandparent→grandchild pairs
(let* ((gp (logic-var)) (gc (logic-var)))
  (findall (lambda () (cons (deref-lvar gp) (deref-lvar gc)))
           (grandparento gp gc)))
; => ((tom . ann) (tom . pat) (bob . jim))

;; Grandchildren of tom
(let* ((gc (logic-var)))
  (findall (lambda () (deref-lvar gc)) (grandparento 'tom gc)))
; => (ann pat)

;; Grandparents of jim
(let* ((gp (logic-var)))
  (findall (lambda () (deref-lvar gp)) (grandparento gp 'jim)))
; => (bob)

(succeeds? (lambda () (run1 (lambda () #t) (grandparento 'tom 'ann))))
; => #t
```

### Prolog comparison

| Prolog | Eta relational |
|--------|---------------|
| `parent(tom,bob).` … (facts) | `(define parent-db '((tom bob) …))` |
| `parent(P,C) :- member((P,C), Db).` | `(defun parento (p c) (map* … parent-db))` |
| `grandparent(GP,GC) :- parent(GP,M), parent(M,GC).` | `(defun grandparento (gp gc) …)` |
| `findall(C, parent(tom,C), Cs).` | `(findall (lambda () (deref-lvar cv)) (parento 'tom cv))` |
| `\+ parent(tom,jim).` | `(naf (lambda () (run1 (lambda () #t) (parento 'tom 'jim))))` |

---

## Imports

Eta's module system controls which names are visible across files.
Every source file contains one or more `(module …)` forms with
explicit `import` and `export` declarations.

### Defining a reusable module

Save as **`greeting.eta`**:

```scheme
(module greeting
  (import std.io)
  (export say-hello)
  (begin
    (defun say-hello (name)
      (println (string-append "Hello, " name "!")))))
```

### Importing from another file

Save as **`app.eta`** in the same directory:

```scheme
(module app
  (import greeting)
  (begin
    (say-hello "world")))
```

```bash
etai app.eta          # prints: Hello, world!
```

`etai` auto-adds the input file's directory to the module search path.
For modules in other directories use `--path` or `ETA_MODULE_PATH`.

### Import clause variants

```scheme
;; All exports
(import greeting)

;; Only specific names
(import (only std.math pi e))

;; Everything except certain names
(import (except std.collections sort))

;; Rename on import
(import (rename std.math (pi PI) (e E)))

;; Prefix — namespace-style qualified access
(import (prefix std.math math:))
;; use math:pi, math:even?, math:gcd, etc.
```

Prefix is especially useful when two modules export the same name:

```scheme
(module app
  (import (prefix mod-a a:))
  (import (prefix mod-b b:))
  (begin
    (a:process data)
    (b:process data)))
```

---

## Running in the REPL

The prelude is auto-loaded so all standard library functions are
available immediately:

```
$ eta_repl
Loaded C:\...\stdlib\prelude.eta
eta REPL - type an expression and press Enter.
Use Ctrl+C or (exit) to quit.
eta> (map* square '(1 2 3 4 5))
=> (1 4 9 16 25)
eta> (filter even? (range 1 21))
=> (2 4 6 8 10 12 14 16 18 20)
eta> (foldl + 0 (range 1 101))
=> 5050
eta> (define double (lambda (x) (* 2 x)))
eta> (map* (compose double square) '(1 2 3))
=> (2 8 18)
```

You can also import user-defined modules in the REPL.  Point the REPL
at the directory containing your `.eta` files with `--path`:

```
$ eta_repl --path ./mylibs
eta> (import greeting)
eta> (say-hello "REPL")
Hello, REPL!
```
