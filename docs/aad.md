# Adjoint Algorithmic Differentiation (AAD)

[← Back to README](../README.md) · [Examples](examples.md) ·
[Architecture](architecture.md) · [Modules & Stdlib](modules.md)

---

## Overview

[`examples/aad.eta`](../examples/aad.eta) implements **reverse-mode
automatic differentiation** (also called adjoint or backpropagation)
entirely in Eta.  It computes exact gradients of scalar functions
with respect to any number of input variables — the same mathematical
machinery that powers deep-learning frameworks.

**Key ideas demonstrated:**

- Closures as first-class backpropagators
- `define-syntax` for transparent operator overloading
- Higher-order functions (`foldl`, `apply`, `append`)
- Pairs, lists, and vectors working together

```bash
etai examples/aad.eta
```

---

## How Reverse-Mode AD Works

Reverse-mode AD records a computation as a graph of elementary
operations.  Each intermediate value carries a **backpropagator** — a
closure that knows how to distribute an incoming adjoint (∂loss/∂self)
back to its inputs.  A single backward pass from the output propagates
adjoints all the way to the input variables, yielding the full gradient
in one sweep regardless of the number of inputs.

```
Forward pass                 Backward pass
────────────                 ─────────────
x ──┐                        ∂f/∂x ← adj_x
    ├── z = x·y ── f         1 (seed)
y ──┘                        ∂f/∂y ← adj_y
```

---

## The Dual Representation

Every value in the AD system is a **dual** — a pair whose `car` is the
primal (numeric) value and whose `cdr` is a backpropagator closure:

```
dual = (primal-value . backprop-fn)
backprop-fn : adjoint → list of (index . adjoint-contribution)
```

The backpropagator takes a single adjoint (the "how much does the final
result change per unit change in *me*" quantity) and returns a flat list
of `(variable-index . contribution)` pairs.

### `dual-val` — extract the primal

```scheme
(defun dual-val (d)
  (if (pair? d) (car d) d))
```

If `d` is already a plain number (not a dual), return it as-is.  This
lets lifted operations accept both dual and plain numeric arguments
without explicit wrapping.

### `dual-bp` — extract the backpropagator

```scheme
(defun dual-bp (d)
  (if (pair? d)
      (cdr d)
      (lambda (adj) '())))
```

For a plain number the backpropagator is the zero function — constants
have no upstream variables to propagate to.

### `make-const` — wrap a constant

```scheme
(defun make-const (v)
  (cons v (lambda (adj) '())))
```

Creates a dual with a known numeric value and a no-op backpropagator.
Used when literal numbers appear inside an AD expression.

### `make-var` — wrap an input variable

```scheme
(defun make-var (v idx)
  (cons v (lambda (adj) (list (cons idx adj)))))
```

Creates a dual for the `idx`-th input variable.  The backpropagator
returns a single-element list mapping the variable's index to the
incoming adjoint — "the gradient contribution for variable `idx` is
exactly `adj`."

### `ensure-dual` — auto-promote plain numbers

```scheme
(defun ensure-dual (x)
  (if (pair? x) x (make-const x)))
```

Every lifted operation calls `ensure-dual` on its arguments so that
users can freely mix dual values with plain numeric literals like `3`
or `100`.

---

## Lifted Arithmetic Operations

Each lifted operation computes the **forward** (primal) result and
builds a **backward** closure that applies the chain rule.

### `d+` — addition

```scheme
(defun d+ (a b)
  (let ((a (ensure-dual a))
        (b (ensure-dual b)))
    (let ((va (dual-val a)) (vb (dual-val b))
          (ba (dual-bp a))  (bb (dual-bp b)))
      (cons (+ va vb)
            (lambda (adj)
              (append (ba adj) (bb adj)))))))
```

**Forward:** z = a + b

**Backward:** ∂z/∂a = 1, ∂z/∂b = 1 — the adjoint passes through
unchanged to both inputs.

### `d-` — subtraction

```scheme
(defun d- (a b)
  (let ((a (ensure-dual a))
        (b (ensure-dual b)))
    (let ((va (dual-val a)) (vb (dual-val b))
          (ba (dual-bp a))  (bb (dual-bp b)))
      (cons (- va vb)
            (lambda (adj)
              (append (ba adj) (bb (* -1 adj))))))))
```

**Forward:** z = a − b

**Backward:** ∂z/∂a = 1, ∂z/∂b = −1 — the adjoint for `b` is negated.

### `d*` — multiplication

```scheme
(defun d* (a b)
  (let ((a (ensure-dual a))
        (b (ensure-dual b)))
    (let ((va (dual-val a)) (vb (dual-val b))
          (ba (dual-bp a))  (bb (dual-bp b)))
      (cons (* va vb)
            (lambda (adj)
              (append (ba (* adj vb)) (bb (* adj va))))))))
```

**Forward:** z = a · b

**Backward:** ∂z/∂a = b, ∂z/∂b = a — the classic product rule.  Each
input's adjoint is scaled by the *other* input's primal value.

### `d/` — division

```scheme
(defun d/ (a b)
  (let ((a (ensure-dual a))
        (b (ensure-dual b)))
    (let ((va (dual-val a)) (vb (dual-val b))
          (ba (dual-bp a))  (bb (dual-bp b)))
      (cons (/ va vb)
            (lambda (adj)
              (append (ba (* adj (/ 1 vb)))
                      (bb (* adj (* -1 (/ va (* vb vb)))))))))))
```

**Forward:** z = a / b

**Backward:** ∂z/∂a = 1/b, ∂z/∂b = −a/b²

### `dsin` — sine

```scheme
(defun dsin (a)
  (let ((a (ensure-dual a)))
    (let ((va (dual-val a))
          (ba (dual-bp a)))
      (cons (sin va)
            (lambda (adj)
              (ba (* adj (cos va))))))))
```

**Forward:** z = sin(a)

**Backward:** ∂z/∂a = cos(a)

### `dcos` — cosine

```scheme
(defun dcos (a)
  (let ((a (ensure-dual a)))
    (let ((va (dual-val a))
          (ba (dual-bp a)))
      (cons (cos va)
            (lambda (adj)
              (ba (* adj (* -1 (sin va)))))))))
```

**Forward:** z = cos(a)

**Backward:** ∂z/∂a = −sin(a)

### `dexp` — exponential

```scheme
(defun dexp (a)
  (let ((a (ensure-dual a)))
    (let ((va (dual-val a))
          (ba (dual-bp a)))
      (let ((ev (exp va)))
        (cons ev
              (lambda (adj)
                (ba (* adj ev))))))))
```

**Forward:** z = eᵃ

**Backward:** ∂z/∂a = eᵃ — the exponential is its own derivative.
Note that `ev` is computed once and shared between the forward result
and the backward closure.

### `dlog` — natural logarithm

```scheme
(defun dlog (a)
  (let ((a (ensure-dual a)))
    (let ((va (dual-val a))
          (ba (dual-bp a)))
      (cons (log va)
            (lambda (adj)
              (ba (* adj (/ 1 va))))))))
```

**Forward:** z = ln(a)

**Backward:** ∂z/∂a = 1/a

---

## The `ad` Macro

Writing `(d+ (d* x y) (dsin x))` is correct but noisy.  The `ad`
macro, defined with `define-syntax` / `syntax-rules`, rewrites
natural arithmetic into lifted calls:

```scheme
(define-syntax ad
  (syntax-rules (+ * - / sin cos exp log)
    ((_ (+ a b))   (d+ (ad a) (ad b)))
    ((_ (* a b))   (d* (ad a) (ad b)))
    ((_ (- a b))   (d- (ad a) (ad b)))
    ((_ (/ a b))   (d/ (ad a) (ad b)))
    ((_ (sin a))   (dsin (ad a)))
    ((_ (cos a))   (dcos (ad a)))
    ((_ (exp a))   (dexp (ad a)))
    ((_ (log a))   (dlog (ad a)))
    ((_ x)         x)))
```

The macro walks the expression tree recursively.  Literal keywords
(`+`, `*`, `sin`, etc.) in the `syntax-rules` literal list ensure
that only *these specific names* are rewritten.  Everything else (a
variable, a number, a function call) falls through to the catch-all
`((_ x) x)` rule and is left unchanged.

**Example expansion:**

```scheme
(ad (+ (* x y) (sin x)))
;; expands to:
(d+ (d* x y) (dsin x))
```

---

## Gradient Computation

### `collect-adjoints` — accumulate contributions

```scheme
(defun collect-adjoints (n adj-list)
  (let ((result (make-vector n 0)))
    (letrec ((loop (lambda (xs)
                     (if (null? xs)
                         result
                         (let ((p (car xs)))
                           (let ((i (car p))
                                 (v (cdr p)))
                             (vector-set! result i
                               (+ (vector-ref result i) v))
                             (loop (cdr xs))))))))
      (loop adj-list))))
```

After the backward pass the backpropagator returns a flat list of
`(index . value)` pairs.  A variable may appear multiple times (once
for each path through the graph).  `collect-adjoints` sums these into
a dense vector of length `n` (the number of input variables).

### `grad` — top-level gradient driver

```scheme
(defun grad (f vals)
  (letrec ((make-vars
             (lambda (vs idx)
               (if (null? vs) '()
                   (cons (make-var (car vs) idx)
                         (make-vars (cdr vs) (+ idx 1)))))))
    (let ((duals (make-vars vals 0)))
      (let ((out (apply f duals)))
        (let ((primal (dual-val out))
              (adjoints ((dual-bp out) 1)))
          (list primal (collect-adjoints (length vals) adjoints)))))))
```

**Steps:**

1. **Lift inputs** — wrap each numeric value into a `make-var` dual
   with a unique index (0, 1, 2, …).
2. **Forward pass** — call `f` with the dual-valued arguments.  All
   lifted operations build the backpropagator graph as they execute.
3. **Backward pass** — seed the output's backpropagator with adjoint
   = 1 (∂f/∂f = 1).  This returns the flat list of adjoint
   contributions.
4. **Collect** — sum contributions into a gradient vector.
5. **Return** — `(primal-value gradient-vector)`.

---

## Worked Examples

### 1. Simple multiplication — f(x, y) = x · y

```scheme
(grad (lambda (x y) (d* x y)) '(3 4))
;; => (12 #(4 3))
```

| | Value |
|---|---|
| Primal | 3 × 4 = **12** |
| ∂f/∂x | y = **4** |
| ∂f/∂y | x = **3** |

The `d*` backpropagator returns `(ba (* adj vb))` for `x` and
`(bb (* adj va))` for `y`.  With `adj = 1`:

- x's contribution: `1 × 4 = 4`
- y's contribution: `1 × 3 = 3`

### 2. Polynomial — g(x) = x² + 3x + 1

```scheme
(grad (lambda (x) (ad (+ (+ (* x x) (* 3 x)) 1))) '(4))
;; => (29 #(11))
```

| | Value |
|---|---|
| Primal | 16 + 12 + 1 = **29** |
| dg/dx | 2x + 3 = 2(4) + 3 = **11** |

The `(* x x)` term contributes two paths through `d*`, each
delivering `adj × x = 1 × 4`, which are summed by
`collect-adjoints` to give `4 + 4 = 8`.  The `(* 3 x)` term
contributes `3`.  Total: `8 + 3 = 11`.

### 3. Transcendental — f(x, y) = x · y + sin(x)

```scheme
(grad (lambda (x y) (ad (+ (* x y) (sin x)))) '(2 3))
;; => (6.909.. #(3.583.. 2))
```

| | Value |
|---|---|
| Primal | 2 × 3 + sin(2) ≈ **6.909** |
| ∂f/∂x | y + cos(x) = 3 + cos(2) ≈ **3.584** |
| ∂f/∂y | x = **2** |

The `x` variable appears in both `(* x y)` and `(sin x)`.  Each
sub-expression's backpropagator contributes to the same index; they
are summed automatically.

### 4. Chain rule — h(x) = exp(2x)

```scheme
(grad (lambda (x) (ad (exp (* 2 x)))) '(1))
;; => (7.389.. #(14.778..))
```

| | Value |
|---|---|
| Primal | e² ≈ **7.389** |
| dh/dx | 2 · e² ≈ **14.778** |

This exercises nested backpropagation: `dexp` passes `adj × eᵛ` to
`d*`, which in turn distributes to the constant `2` (no-op) and to
`x` (receiving `adj × eᵛ × 2`).

### 5. sin(x) at x = 0

```scheme
(grad (lambda (x) (dsin x)) '(0))
;; => (0 #(1.0))
```

| | Value |
|---|---|
| Primal | sin(0) = **0** |
| df/dx | cos(0) = **1** |

### 6. exp(x) at x = 0

```scheme
(grad (lambda (x) (dexp x)) '(0))
;; => (1 #(1.0))
```

| | Value |
|---|---|
| Primal | e⁰ = **1** |
| df/dx | e⁰ = **1** |

### 7. log(x) at x = 2

```scheme
(grad (lambda (x) (dlog x)) '(2))
;; => (0.693.. #(0.5))
```

| | Value |
|---|---|
| Primal | ln(2) ≈ **0.693** |
| df/dx | 1/x = 1/2 = **0.5** |

### 8. Rosenbrock at the minimum — f(x, y) = (1−x)² + 100(y−x²)²

```scheme
(grad (lambda (x y)
        (ad (+ (* (- 1 x) (- 1 x))
               (* 100 (* (- y (* x x))
                         (- y (* x x)))))))
      '(1 1))
;; => (0 #(0 0))
```

| | Value |
|---|---|
| Primal | (1−1)² + 100(1−1)² = **0** |
| ∂f/∂x | **0** |
| ∂f/∂y | **0** |

The point (1, 1) is the global minimum of the Rosenbrock function,
so both partial derivatives are exactly zero.  This is a classic test
for numerical optimisation code.

### 9. Dot product with higher-order functions

```scheme
(grad (lambda (x y z)
        (foldl d+ (make-const 0)
          (list (ad (* x 1))
                (ad (* y 2))
                (ad (* z 3)))))
      '(1 1 1))
;; => (6 #(1 2 3))
```

| | Value |
|---|---|
| Primal | 1·1 + 1·2 + 1·3 = **6** |
| Gradient | **(1, 2, 3)** — the weight vector |

This shows that lifted operations compose cleanly with standard
library functions like `foldl`.  Each `(ad (* var weight))` produces
a dual, and `d+` accumulates them.  The gradient is simply the
constant weight vector — exactly what you'd expect from a linear
function.

---

## Summary

| Component | Role |
|-----------|------|
| `make-var` / `make-const` | Construct dual values (leaf nodes of the computation graph) |
| `d+`, `d*`, `d-`, `d/` | Lifted arithmetic — forward compute + backward chain rule |
| `dsin`, `dcos`, `dexp`, `dlog` | Lifted transcendentals |
| `ad` macro | Syntactic sugar — rewrites `(+ a b)` → `(d+ (ad a) (ad b))` |
| `collect-adjoints` | Sum per-variable adjoint contributions into a gradient vector |
| `grad` | Top-level driver: lift inputs → forward → backward → collect |

