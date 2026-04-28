# Adjoint Algorithmic Differentiation (AAD)

[Back to README](../../../README.md) | [Examples](../examples-tour.md) | [Modules](modules.md)

> 📓 **Interactive notebook:**
> [`examples/notebooks/AAD.ipynb`](../../../examples/notebooks/AAD.ipynb) —
> the same walkthrough as a runnable Jupyter notebook (xeus-eta kernel).

---

## Contents

- [Overview](#overview)
- [How Reverse-Mode AD Works](#how-reverse-mode-ad-works)
- [Worked Example: The Tape By Hand](#worked-example-the-tape-by-hand)
- [Core API](#core-api)
- [The `grad` Helper](#the-grad-helper)
- [Safety Model](#safety-model)
- [Nested Tape Contract](#nested-tape-contract)
- [Parallel Contract](#parallel-contract)
- [Non-Differentiability Policy](#non-differentiability-policy)
- [Gradient Checking](#gradient-checking)
- [Primitive Coverage and Domain Rules](#primitive-coverage-and-domain-rules)
- [Stdlib Helpers](#stdlib-helpers)
- [Cookbook](#cookbook)
- [Pitfalls](#pitfalls)
- [Performance Notes](#performance-notes)
- [AD Error Tags](#ad-error-tags)
- [Examples](#examples)

---

## Overview

Eta provides VM-native **reverse-mode** automatic differentiation using a tape
(Wengert list). When an operation sees a `TapeRef`, the VM records the forward
operation and later computes adjoints in a reverse sweep.

Eta uses standardized safety semantics for ownership, stale references,
cross-VM boundaries, non-differentiable branch policy, and domain failures.

> [!NOTE]
> `std.aad` is re-exported by `std.prelude`, so `grad`, `check-grad`,
> the `ad-*` piecewise helpers, and the `softplus`/`smooth-*` family are
> available in any module that imports the prelude — no extra `(import …)`
> needed.

---

## How Reverse-Mode AD Works

Given a scalar function `f(x₁, …, xₙ)`, reverse-mode AD computes the full
gradient `∇f` in **one** forward pass plus **one** backward pass, regardless
of the number of inputs. That makes it the right tool when *N is large and
the output is scalar* — the exact shape of a loss / objective / option price.

The recipe is mechanical:

1. **Forward pass** — execute `f` normally, but every primitive operation
   that touches a `TapeRef` records `(op, parents, primal)` onto the active
   tape. The tape is a flat array — append-only, no graph allocation, no
   closures.
2. **Seed** — set the adjoint of the output node to `1.0` (so we're computing
   `∂f/∂xᵢ`, not a vector-Jacobian product).
3. **Backward pass** — sweep the tape in reverse. For each entry, multiply
   the local partial derivative by the parent node's adjoint and accumulate
   into the children's adjoints (the chain rule).
4. **Read** — the adjoint of input node `xᵢ` is `∂f/∂xᵢ`.

| Mode | Cost of `∇f` | Best when |
|---|---|---|
| Symbolic differentiation | Expression-blow-up | Tiny, hand-tractable `f` |
| **Forward-mode** AD | `O(N)` evaluations of `f` | `N` small, many outputs |
| **Reverse-mode** AD (this) | `O(1)` evaluations of `f` | `N` large, scalar output |
| Finite differences | `O(N)` evaluations + `O(h²)` truncation error | Cross-checking only |

Memory cost of the tape is roughly **32 bytes per recorded operation** with
zero closure allocations — a 10 000-op forward pass uses ≈320 KB of tape.

---

## Worked Example: The Tape By Hand

Most users only ever call `grad`. To build intuition for what it actually
*does*, here is the same computation written with raw tape primitives —
exactly what `grad` does internally:

```eta
;; f(x, y) = x*y + sin(x)   at (x, y) = (2, 3)
;; Expected:  ∂f/∂x = y + cos(x) = 3 + cos(2) ≈ 2.5839
;;            ∂f/∂y = x          = 2

(let ((tape (tape-new)))                       ; 1. allocate tape
  (let ((x (tape-var tape 2.0))                ; 2. register inputs
        (y (tape-var tape 3.0)))
    (tape-start! tape)                         ; 3. begin recording
    (let ((out (+ (* x y) (sin x))))           ; 4. forward pass
      (tape-stop!)                             ; 5. stop recording
      (tape-backward! tape out)                ; 6. seed = 1, reverse sweep
      (println (tape-primal  tape out))        ;   ⇒ 2.909...
      (println (tape-adjoint tape x))          ;   ⇒ 2.5839...
      (println (tape-adjoint tape y)))))       ;   ⇒ 2.0
```

Conceptually the recorded tape looks like this (input nodes first, then one
entry per primitive, in evaluation order):

```
node 0: var      x = 2.0                       (input)
node 1: var      y = 3.0                       (input)
node 2: mul      n0 * n1     primal = 6.0      (parents: 0, 1)
node 3: sin      n0          primal = 0.909..  (parents: 0)
node 4: add      n2 + n3     primal = 6.909..  (parents: 2, 3)
```

Backward sweep, top-down through the tape (highest index first), with
`adj[4] = 1` seeded:

| Step | Node | Local rule | Adjoint propagation |
|---|---|---|---|
| 1 | `add (n4)` | `∂(a+b)/∂a = 1`, `∂/∂b = 1` | `adj[2] += 1`, `adj[3] += 1` |
| 2 | `sin (n3)` | `∂sin(x)/∂x = cos(x)` | `adj[0] += cos(2) · adj[3]` |
| 3 | `mul (n2)` | `∂(a*b)/∂a = b`, `∂/∂b = a` | `adj[0] += y · adj[2]`, `adj[1] += x · adj[2]` |

Final adjoints: `adj[x] = y + cos(x) ≈ 2.5839`, `adj[y] = x = 2.0`. ✅

> [!TIP]
> You almost never want to write tape code by hand — use `grad`. Knowing
> the mechanics, however, makes the error tags below (`:ad/stale-ref`,
> `:ad/mixed-tape`, `:ad/no-active-tape`) self-explanatory.

---

## Core API

### Tape lifecycle and values

| Primitive | Arity | Purpose |
|---|---:|---|
| `tape-new` | 0 | Create a tape |
| `tape-start!` | 1 | Push tape onto active-tape stack |
| `tape-stop!` | 0 | Pop active tape |
| `tape-clear!` | 1 | Clear entries and bump generation (invalidates old refs) |
| `tape-var` | 2 | Create an independent variable reference |
| `tape-backward!` | 2 | Reverse sweep from output ref (seed = 1) |
| `tape-adjoint` | 2 | Read adjoint for a ref |
| `tape-primal` | 2 | Read primal for a ref |
| `tape-size` | 1 | Number of tape entries |
| `tape-ref?` | 1 | Predicate |
| `tape-ref-index` | 1 | Encoded node index field |
| `tape-ref-value-of` | 2 | Explicit primal extraction with tape argument |
| `tape-ref-value` | 1 | Active-tape primal extraction (strict for TapeRefs) |

### AAD policy controls

| Primitive | Arity | Purpose |
|---|---:|---|
| `set-aad-nondiff-policy!` | 1 | Set `'strict` or `'zero-subgrad` |
| `aad-nondiff-policy` | 0 | Get current policy symbol |

---

## The `grad` Helper

`grad` is the workhorse — almost every AAD program calls it.

```text
(grad f vals)  ⇒  (primal-value gradient-vector)
```

| Argument | Type | Meaning |
|---|---|---|
| `f` | `(λ x₁ … xₙ → scalar)` | Function of `n` numeric arguments. **Must return a single scalar.** |
| `vals` | `list` of length `n` | Point at which to evaluate `f` and `∇f`. |

The result is a 2-element list `(primal grad)` where `primal` is the scalar
value of `f(vals)` and `grad` is a *vector* of partial derivatives in input
order — i.e. `grad[i] = ∂f/∂xᵢ`.

### A first example, end-to-end

```eta
;; f(x) = x² + 3x + 1  at x = 4
;; df/dx = 2x + 3 = 11

(println (grad (lambda (x) (+ (+ (* x x) (* 3 x)) 1))
               '(4)))
;; ⇒ (29 #(11))
;;     │   └─ ∂f/∂x = 11   (1-element vector for 1-input fn)
;;     └─ f(4) = 29
```

### Multiple inputs

```eta
;; Rosenbrock f(x,y) = (1-x)² + 100·(y-x²)²
;; At (1,1) the gradient is (0, 0) — the global minimum.

(println (grad (lambda (x y)
                 (+ (* (- 1 x) (- 1 x))
                    (* 100 (* (- y (* x x))
                              (- y (* x x))))))
               '(1 1)))
;; ⇒ (0 #(0 0))
```

### What can `f` look like?

`f` can use **any** taped primitive, **any** plain control flow that doesn't
inspect a tape ref's value (see [Pitfalls](#pitfalls)), `let` / `letrec`,
recursion, higher-order combinators, and the `ad-*` / `softplus` /
`smooth-*` helpers. It cannot:

- Return a vector / list / record (output must be a single number — if you
  have a vector-valued function, see [Cookbook → Jacobians](#cookbook)).
- Read raw `if` branches that depend on `(< taped-x 0)` under the strict
  policy — use `ad-relu` / `ad-clamp` / `softplus` instead.
- Cross VM boundaries with the partially-evaluated tape ref.

> [!TIP]
> If your function naturally takes a *vector* of parameters
> `(λ θ → loss)`, wrap it: `(grad (lambda args (loss-of-list args)) θ-list)`.
> The [Cookbook](#cookbook) shows the idiomatic packing pattern.

---

## Safety Model

`TapeRef` is a packed immediate with three fields:

- `tape-id`
- `generation`
- `node-index`

At every taped operation and lookup, runtime validation enforces:

- Ownership (`tape-id` must match)
- Lifecycle (`generation` must match current tape generation)
- Bounds (`node-index` must be valid)

This produces deterministic, catchable AD errors instead of silent misuse.

---

## Nested Tape Contract

`active_tape` is stack-based:

1. `tape-start!` pushes.
2. `tape-stop!` pops.
3. Nested tapes are supported.

If control exits through `raise` / `catch`, the VM unwinds tape stack depth
with the catch boundary, so outer tape context remains consistent.

Cross-tape `TapeRef` use is rejected deterministically (`:ad/mixed-tape`).

---

## Parallel Contract

AAD values are VM-local:

- `spawn-thread` / `spawn-thread-with`: fresh in-process VM per actor thread.
- `spawn` / `worker-pool`: separate process VM per worker.
- `Tape` and `TapeRef` are not transferable across VM boundaries.

Serializer checks reject `Tape` and `TapeRef` with tag `:ad/cross-vm-ref`.
Callers must extract plain numeric primals before send.

> [!IMPORTANT]
> When parallelising a Monte-Carlo gradient (e.g. pathwise Greeks in
> [`xva.eta`](../../../examples/xva.eta)), each worker must build its **own**
> local tape, run its `grad`, and send back **plain numbers** — never the
> tape or its refs.

See also: [Network and Message-Passing Parallelism](network-message-passing.md).

---

## Non-Differentiability Policy

Real objectives often contain `abs`, `max`, `min`, `relu`, `clamp` — all of
which have **kinks** (points where the derivative doesn't exist). Eta lets
you choose how to handle them globally:

```eta
(set-aad-nondiff-policy! 'strict)        ; default: refuse to lie
(set-aad-nondiff-policy! 'zero-subgrad)  ; deterministic 0 at the kink
(aad-nondiff-policy)                     ; ⇒ 'strict   (or whatever's set)
```

Kink definitions:

| Function | Kink at |
|---|---|
| `abs(x)` | `x = 0` |
| `max(a, b)` / `min(a, b)` | `a = b` |
| `relu(x)` (via `ad-relu`) | `x = 0` |
| `clamp(x, lo, hi)` (via `ad-clamp`) | `x = lo` or `x = hi` |

Comparison semantics on taped values:

- `strict` — comparison on taped operands raises `:ad/nondiff-strict`
- `zero-subgrad` — taped operands are compared by validated primals

### Side-by-side

```eta
;; ── strict (default) ───────────────────────────────────
(set-aad-nondiff-policy! 'strict)
(grad (lambda (x) (ad-relu x)) '(0.0))
;; ⇒ raises  :ad/nondiff-strict   (relu has no derivative at 0)

(grad (lambda (x) (ad-relu x)) '(1.0))
;; ⇒ (1.0 #(1.0))                 (away from the kink, fine)

;; ── zero-subgrad ───────────────────────────────────────
(set-aad-nondiff-policy! 'zero-subgrad)
(grad (lambda (x) (ad-relu x)) '(0.0))
;; ⇒ (0.0 #(0.0))                 (deterministic 0)
```

> [!WARNING]
> `'zero-subgrad` is convenient but **silently lies** at kinks — your
> optimiser may get stuck there. Prefer the smooth alternatives below
> (`softplus`, `smooth-abs`, `smooth-clamp`) in production loss functions.

---

## Gradient Checking

Every non-trivial AD program should be cross-checked against finite
differences during development. `std.aad` ships two helpers:

```text
(check-grad        f vals [rtol] [atol] [step-scale])  ⇒  bool
(check-grad-report f vals [rtol] [atol] [step-scale])  ⇒  vector
```

`check-grad-report` returns
`#[ok max-error aad-grad fd-grad rtol atol step-scale]` — the per-component
maximum absolute error plus both gradients side by side, so a failure is
self-diagnosing.

```eta
(define (loss x y) (+ (* x x) (* 3 (* x y)) (* y y)))

(check-grad loss '(1.5 -2.0))
;; ⇒ #t

(check-grad-report loss '(1.5 -2.0))
;; ⇒ #(#t 4.4e-9 #(-3.0 -1.0) #(-3.0 -1.0) 1e-5 1e-7 1.0)
;;       │  │      │            │
;;       │  │      │            └ central-diff gradient
;;       │  │      └ AAD gradient
;;       │  └ max |aad - fd| over all components
;;       └ pass/fail flag
```

Defaults:

- `rtol = 1e-5`
- `atol = 1e-7`
- Central-difference step `h = step-scale · √eps · max(1, |x|)`

Tolerance test: `|aad - fd| ≤ atol + rtol · |aad|`.

> [!TIP]
> If `check-grad` fails near a kink, that's expected — finite differences
> straddle the discontinuity. Either move the test point off the kink, or
> swap `ad-relu` for `softplus` and re-check.

---

## Primitive Coverage and Domain Rules

Taped primitives include:

- Arithmetic: `+`, `-`, `*`, `/`
- Unary math: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `log`, `sqrt`
- Piecewise numerics: `abs`, `min`, `max`
- Binary math: `pow`

Domain behavior for taped primitives:

- `log(x)`: `x > 0`, else `:ad/domain`
- `sqrt(x)`: `x ≥ 0`, else `:ad/domain`
- `asin(x)`, `acos(x)`: `-1 ≤ x ≤ 1`, else `:ad/domain`
- `pow(negative, non-integer exponent)`: `:ad/domain`
- `pow(0, negative)`: `:ad/domain`
- `pow(0, 0)`:
  - `strict`: `:ad/domain`
  - `zero-subgrad`: value `1`, zero subgrad
- `pow(0, positive < 1)`:
  - `strict`: `:ad/domain`
  - `zero-subgrad`: finite forward value with zero subgrad at singular base derivative

The current API remains scalar-focused (no tensor-aware tape extension here —
for that, see [`docs/torch.md`](torch.md)).

---

## Stdlib Helpers

`std.aad` layers ergonomic helpers on top of tape primitives.

### Piecewise helpers (policy-aware)

These helpers delegate to taped piecewise primitives and obey
`set-aad-nondiff-policy!` at kinks:

| Helper | Form | Typical use |
|---|---|---|
| `ad-abs` | `(ad-abs x)` | absolute-value terms in objectives |
| `ad-max` | `(ad-max a b ...)` | lower-bounding / ReLU-style gating |
| `ad-min` | `(ad-min a b ...)` | upper-bounding values |
| `ad-relu` | `(ad-relu x)` | `max(0, x)` activations |
| `ad-clamp` | `(ad-clamp x lo hi)` | box constraints |

See the [Non-Differentiability Policy](#non-differentiability-policy) section
for a side-by-side `'strict` vs `'zero-subgrad` example using `ad-relu`.

### Smooth helpers (branch-free approximations)

Use these when you want stable, *everywhere-defined* gradients near kinks:

| Helper | Approximates | Knob |
|---|---|---|
| `(softplus x beta)` | `max(0, x)` | larger `beta` ⇒ sharper |
| `(smooth-abs x epsilon)` | `abs(x)` | larger `epsilon` ⇒ rounder |
| `(smooth-clamp x lo hi beta)` | `clamp(x, lo, hi)` | larger `beta` ⇒ sharper corners |

```eta
(grad (lambda (x) (softplus x 8.0))  '(0.0))   ; ⇒ (0.087 #(0.5))
(grad (lambda (x) (smooth-abs x 1e-3)) '(0.0)) ; ⇒ (0.001 #(0.0))
```

`softplus` is `(1/β) · log(1 + exp(β·x))` — for `β = 8` and `|x| > 1` it
agrees with `relu` to machine precision, while remaining smooth at the
origin.

### Gradient utilities

- `(grad f vals)` — `(primal-value gradient-vector)`.
- `(check-grad f vals [rtol] [atol] [step-scale])` — pass / fail boolean.
- `(check-grad-report …)` — diagnostic vector (see above).
- `(with-checkpoint thunk)` — currently a passthrough placeholder; reserved
  for forthcoming activation-checkpointing support that will trade compute
  for tape memory in deep call stacks.

---

## Cookbook

### Functions of a vector parameter

`grad` takes a *list* of scalar arguments, not a vector. The idiomatic way
to differentiate `loss : ℝⁿ → ℝ` is `apply`:

```eta
(define (loss-list params)
  ;; params is a regular list of n numbers (or tape refs while taped)
  (let ((x (car params)) (y (cadr params)) (z (caddr params)))
    (+ (* x x) (* y y) (* z z))))

(define theta '(1.0 -2.0 3.0))
(grad (lambda args (loss-list args)) theta)
;; ⇒ (14.0 #(2.0 -4.0 6.0))
```

### Computing a Jacobian

For `g : ℝⁿ → ℝᵐ`, run `grad` once per output component:

```eta
(define (g x y)
  (list (+ (* x x) y)        ; g₁
        (* x (sin y))))      ; g₂

(define (jacobian g vals m)
  (letrec ((rows (lambda (i acc)
                   (if (= i m)
                       (reverse acc)
                       (rows (+ i 1)
                             (cons (cadr (grad (lambda args
                                                 (list-ref (apply g args) i))
                                               vals))
                                   acc))))))
    (rows 0 '())))

(jacobian g '(2.0 1.0) 2)
;; ⇒ (#(4.0 1.0)              ; ∂g₁/∂x, ∂g₁/∂y
;;    #(0.8414... 1.0806...)) ; ∂g₂/∂x, ∂g₂/∂y
```

For an `m × n` Jacobian this costs `m` forward+backward sweeps — still
better than finite differences (`2n` evaluations) whenever `m < 2n`.

### One gradient step

```eta
(define (sgd-step f x lr)
  (let ((res (grad (lambda args (apply f args)) x)))
    (let ((g (cadr res)))
      ;; xᵢ ← xᵢ − lr · ∂f/∂xᵢ
      (letrec ((upd (lambda (xs i acc)
                      (if (null? xs) (reverse acc)
                          (upd (cdr xs) (+ i 1)
                               (cons (- (car xs) (* lr (vector-ref g i)))
                                     acc))))))
        (upd x 0 '())))))

(define f (lambda (x y) (+ (* x x) (* y y))))
(sgd-step f '(1.0 1.0) 0.1)   ; ⇒ (0.8 0.8)
```

---

## Pitfalls

> [!WARNING]
> **Comparison on a taped value is not differentiable.** Code like
> `(if (< x 0) (- x) x)` works on plain numbers but raises
> `:ad/nondiff-strict` (or silently picks one branch under
> `'zero-subgrad`). Use `(ad-abs x)` or `(smooth-abs x 1e-3)` instead.

> [!WARNING]
> **Don't capture a `TapeRef` past `tape-clear!`.** Bumping the generation
> invalidates every previously-issued ref to that tape — using one raises
> `:ad/stale-ref`. Inside `grad` you don't see this; if you write your
> own driver, register variables *after* the most recent `tape-clear!`.

> [!IMPORTANT]
> **One scalar output.** `tape-backward!` seeds the *output* node's adjoint
> with `1.0`. If you want a vector-Jacobian product, build it explicitly
> via per-component `grad` calls (see the Jacobian cookbook entry).

> [!NOTE]
> **`if` on plain (un-taped) booleans is fine** — only comparisons that
> directly inspect a `TapeRef` are restricted. Branching on a value that
> was never recorded (e.g. a configuration flag, a loop counter, or the
> output of `(tape-ref-value x)` *before* `tape-start!`) is unaffected.

---

## Performance Notes

- **Tape memory** ≈ 32 bytes per recorded operation, append-only, no GC
  pressure during the forward pass.
- **No closure allocations.** Adjoints are computed by a tight loop over a
  flat `Vec<TapeEntry>` — no per-op heap traffic.
- **Cost ratio.** A reverse sweep is ≈1–3× the cost of the forward pass
  for typical mixes of `+`, `*`, `sin`, `exp`. Compare to ≈`2n×` for
  central-difference finite differences — `grad` overtakes FD even for
  `n = 2`.
- **Cache reuse.** Reusing a tape across iterations (call `tape-clear!`
  rather than `tape-new`) saves the heap-vector reservation; useful in
  optimisation inner loops.

---

## AD Error Tags

| Tag | Meaning |
|---|---|
| `:ad/mixed-tape` | refs from different tapes |
| `:ad/stale-ref` | old generation or invalid index |
| `:ad/no-active-tape` | ambient tape API used with no active tape |
| `:ad/nondiff-strict` | strict-mode kink/comparison rejection |
| `:ad/cross-vm-ref` | `Tape` / `TapeRef` attempted across VM boundary |
| `:ad/domain` | taped primitive domain violation |

Errors are emitted as `runtime.error` payloads with structured field rows.
Tests should assert tag identity and payload keys, not message text.

---

## Examples

- [examples/aad.eta](../../../examples/aad.eta) — basic AAD walkthrough
- [European option pricing](european.md) ([source](../../../examples/european.eta))
- [SABR model](sabr.md) ([source](../../../examples/sabr.eta))
- [XVA](xva.md) ([source](../../../examples/xva.eta))
