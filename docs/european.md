# European Option Greeks with AAD

[← Back to README](../README.md) · [AAD Deep-Dive](aad.md) ·
[xVA Example](xva.md) · [Examples](examples.md)

---

## Overview

[`examples/european.eta`](../examples/european.eta) computes
**Black-Scholes option Greeks** — both first-order and second-order —
using the [reverse-mode AD library](aad.md).

**Key ideas demonstrated:**

- Custom AD primitives with analytic backward derivatives (`dnorm-cdf`, `dnorm-pdf`)
- First-order Greeks (Delta, Vega, Theta, Rho) in a **single backward pass**
- Second-order Greeks (Gamma, Vanna, Volga) by applying `grad` to Greek expressions
- Schwarz's theorem as a built-in consistency check

```bash
etai examples/european.eta
```

> [!NOTE]
> This example uses the **Black-Scholes closed-form** rather than Monte
> Carlo simulation.  The AD technique is identical — every `d*`, `dexp`,
> `dnorm-cdf` call would work the same way inside a path-level MC loop.
> The closed form lets us verify every Greek against its known analytic
> value.

---

## Market Parameters

| Parameter | Symbol | Value | Description |
|-----------|--------|-------|-------------|
| Spot price | *S* | 100.0 | Current price of the underlying |
| Strike | *K* | 90.0 | Option strike price |
| Risk-free rate | *r* | 3 % | Continuous compounding |
| Volatility | *σ* | 30 % | Annualised implied volatility |
| Maturity | *T* | 0.5 | Time to expiry in years |

These match the standard Python autograd benchmark
(`F=100, vol=0.3, K=90, T=0.5, IR=0.03`).

---

## Statistical Functions

### Normal PDF — `norm-pdf`

$$\varphi(x) = \frac{1}{\sqrt{2\pi}} \, e^{-x^2/2}$$

```scheme
(define inv-sqrt-2pi 0.3989422804014327)
(defun norm-pdf (x)
  (* inv-sqrt-2pi (exp (* -0.5 (* x x)))))
```

### Normal CDF — `norm-cdf-approx`

$$\Phi(x) \approx 1 - \varphi(x) \cdot \bigl(a_1 t + a_2 t^2 + a_3 t^3 + a_4 t^4 + a_5 t^5\bigr)$$

where $t = 1/(1 + 0.2316419 \, x)$.  This is Abramowitz & Stegun
formula 26.2.17 with maximum absolute error < 7.5 × 10⁻⁸.

```scheme
(defun norm-cdf-approx (x)
  (if (< x 0)
      (- 1.0 (norm-cdf-approx (- 0 x)))
      (let ((t (/ 1.0 (+ 1.0 (* 0.2316419 x)))))
        (- 1.0 (* (norm-pdf x)
                   (* t (+ 0.319381530 ...)))))))
```

### AD-Lifted CDF — `dnorm-cdf` (Custom VJP)

```scheme
(defun dnorm-cdf (a)
  (let ((a (ensure-dual a)))
    (let ((va (dual-val a))
          (ba (dual-bp a)))
      (cons (norm-cdf-approx va)
            (lambda (adj)
              (ba (* adj (norm-pdf va))))))))
```

**Forward:** Φ(x) via polynomial approximation  
**Backward:** Φ'(x) = φ(x) — the exact analytic derivative

> [!TIP]
> This is the **custom VJP** pattern (called `@custom_vjp` in JAX,
> `Function.backward()` in PyTorch).  The forward path uses an
> efficient approximation, but the backward path uses the known
> analytic derivative.  AD gradients are therefore exact to
> floating-point precision — the polynomial approximation error
> does **not** propagate into derivatives.

### AD-Lifted PDF — `dnorm-pdf`

```scheme
(defun dnorm-pdf (a)
  (let ((a (ensure-dual a)))
    (let ((va (dual-val a)) (ba (dual-bp a)))
      (let ((pv (norm-pdf va)))
        (cons pv
              (lambda (adj)
                (ba (* adj (* (- 0 va) pv)))))))))
```

**Forward:** φ(x)  
**Backward:** φ'(x) = −x · φ(x)

---

## Black-Scholes Formulas

### d₁ and d₂

$$d_1 = \frac{\ln(S/K) + (r + \sigma^2/2) \cdot T}{\sigma \sqrt{T}}$$

$$d_2 = d_1 - \sigma\sqrt{T}$$

```scheme
(defun bs-d1 (S K r sigma T)
  (d/ (d+ (dlog (d/ S K))
          (d* (d+ r (d/ (d* sigma sigma) 2)) T))
      (d* sigma (dsqrt T))))
```

### Call Price

$$C = S \cdot \Phi(d_1) - K \cdot e^{-rT} \cdot \Phi(d_2)$$

```scheme
(defun bs-call-price (S K r sigma T)
  (let ((d1  (bs-d1 S K r sigma T))
        (svt (d* sigma (dsqrt T))))
    (let ((d2 (d- d1 svt)))
      (d- (d* S (dnorm-cdf d1))
          (d* (d* K (dexp (d* -1 (d* r T))))
              (dnorm-cdf d2))))))
```

---

## First-Order Greeks

A single call to `grad` on `bs-call-price` with inputs
`(S, K, r, σ, T)` produces the price **and** all five partial
derivatives in one backward pass:

```scheme
(grad (lambda (S K r sigma T)
        (bs-call-price S K r sigma T))
      '(100.0 90.0 0.03 0.30 0.5))
;; => (price #(∂C/∂S  ∂C/∂K  ∂C/∂r  ∂C/∂σ  ∂C/∂T))
```

| Index | Greek | Formula | Approx Value |
|-------|-------|---------|:------------:|
| 0 | **Delta** (∂C/∂S) | Φ(d₁) | 0.750 |
| 2 | **Rho** (∂C/∂r) | K·T·e⁻ʳᵀ·Φ(d₂) | 30.0 |
| 3 | **Vega** (∂C/∂σ) | S·φ(d₁)·√T | 22.5 |
| 4 | **−Theta** (∂C/∂T) | see closed form | 8.6 |

**Normalised conventions** (matching standard quoting):

| Greek | Normalisation | Approx Value |
|-------|--------------|:------------:|
| Vega (per 1% vol) | ÷ 100 | 0.225 |
| Rho (per 1% rate) | ÷ 100 | 0.300 |
| Theta (per day) | ÷ 365.25 | −0.023 |

---

## Second-Order Greeks

### The First Approach: Grad-on-Greek

To compute Gamma (∂²C/∂S²), we express **Delta as an AD function**
and differentiate it:

$$\Delta(S,K,r,\sigma,T) = \Phi(d_1)$$

```scheme
(defun bs-delta-fn (S K r sigma T)
  (dnorm-cdf (bs-d1 S K r sigma T)))

(grad (lambda (S K r sigma T)
        (bs-delta-fn S K r sigma T))
      '(100.0 90.0 0.03 0.30 0.5))
;; => (delta #(∂Δ/∂S  ∂Δ/∂K  ∂Δ/∂r  ∂Δ/∂σ  ∂Δ/∂T))
;;             Gamma                   Vanna
```

Similarly, for Volga (∂²C/∂σ²), express **Vega as an AD function**:

$$\mathcal{V}(S,K,r,\sigma,T) = S \cdot \varphi(d_1) \cdot \sqrt{T}$$

```scheme
(defun bs-vega-fn (S K r sigma T)
  (d* S (d* (dnorm-pdf (bs-d1 S K r sigma T)) (dsqrt T))))

(grad (lambda (S K r sigma T)
        (bs-vega-fn S K r sigma T))
      '(100.0 90.0 0.03 0.30 0.5))
;; => (vega #(∂V/∂S  ∂V/∂K  ∂V/∂r  ∂V/∂σ  ∂V/∂T))
;;            Vanna                  Volga
```

### Second-Order Results

| Greek | Definition | Closed Form | Approx Value |
|-------|-----------|-------------|:------------:|
| **Gamma** | ∂²C/∂S² | φ(d₁) / (S·σ·√T) | 0.015 |
| **Vanna** | ∂²C/∂S∂σ | −φ(d₁)·d₂/σ | −0.489 |
| **Volga** | ∂²C/∂σ² | Vega·d₁·d₂/σ | 23.3 |

### Schwarz's Theorem Check

The mixed partial derivative ∂²C/∂S∂σ can be computed two ways:

1. **∂Delta/∂σ** — from the `grad(bs-delta-fn)` call
2. **∂Vega/∂S** — from the `grad(bs-vega-fn)` call

By Schwarz's theorem these must be equal.  The example prints both
values so the user can verify they match to floating-point precision.

---

## Comparison with Python autograd

The Python benchmark using `autograd.elementwise_grad`:

```python
from autograd import elementwise_grad as egrad

gradient_func = egrad(call_price, (0, 1, 3, 4))
gradient_func2 = egrad(egrad(call_price, (0)))  # second derivative

delta, vega, theta, rho = gradient_func(F, vol, K, T, IR, steps, trials)
gamma = gradient_func2(F, vol, K, T, IR, steps, trials)
```

| | Python (MC, 1M paths) | Eta (BS closed-form) |
|---|:---:|:---:|
| Price | ~12.6 (MC noise) | ~14.88 (exact) |
| Delta | ~0.746 | ~0.750 |
| Gamma | ~0.015 | ~0.015 |
| Vega (per 1%) | ~0.225 | ~0.225 |
| Theta (per day) | ~−0.025 | ~−0.023 |
| Rho (per 1%) | ~0.298 | ~0.300 |

> [!NOTE]
> The Python MC values include Monte-Carlo noise (seed-dependent).
> The Eta closed-form values are exact to floating-point precision.
> Both approaches use the same AD technique — the only difference is
> the pricing function being differentiated.

---

## How Second-Order AD Works Here

The example uses what is effectively **reverse-over-reverse** AD,
applied at the mathematical level rather than the tape level:

1. **Inner differentiation (conceptual):** The Black-Scholes formula
   gives us Delta = Φ(d₁) as a *known* function of the market params.
2. **Outer differentiation (actual):** We express Delta using the AD
   operations (`dlog`, `d/`, `dsqrt`, `dnorm-cdf`) and call `grad`,
   which builds a fresh computation graph and runs a backward pass.

The result is exact second-order derivatives with no finite-difference
approximations and no numerical noise.

> [!IMPORTANT]
> ### On True Reverse-over-Reverse (Tape-on-Tape)
>
> In the approach above, we explicitly write the Delta/Vega formulas
> and differentiate them.  This works because Black-Scholes Greeks
> have known closed forms.
>
> A **true reverse-over-reverse** system (as in JAX, PyTorch, or
> Adept) would instead make the backpropagator closures' *internal
> arithmetic* (`* adj vb`, `+ adj adj`) use lifted AD operations.
> The backward pass itself would then build a computation graph,
> and a second backward pass on that graph would yield second-order
> derivatives *automatically* — even for functions whose first-order
> Greeks have no closed form (e.g., Monte Carlo pricers, exotic
> payoffs, PDE solvers).
>
> **Trade-offs:**
>
> | | Grad-on-Greek (this example) | True tape-on-tape |
> |---|---|---|
> | Requires closed-form Greeks? | Yes | No |
> | Implementation complexity | Low | High (two-level tapes) |
> | Works for MC / exotics? | No | Yes |
> | Memory overhead | 1× | 2–3× (nested tapes) |
> | Perturbation confusion risk | None | Must be handled carefully |
>
> For European options the grad-on-Greek approach is ideal.  For a
> production xVA Gamma/cross-Gamma engine with thousands of risk
> factors, true tape-on-tape reverse-over-reverse is essential.

---

## §5  Reverse-on-Reverse via Native Dual VM Type

The example now **also** demonstrates true tape-on-tape
reverse-on-reverse using Eta's dedicated **native Dual heap object**
and specialised VM opcodes.

> [!IMPORTANT]
> ### The Opcode Approach: Why It Matters
>
> Traditional AD libraries implement dual numbers as *library-level*
> wrappers (e.g., a Scheme `cons` pair `(primal . backpropagator)`).
> Every arithmetic operation must check at runtime whether its
> arguments are wrapped duals — via Scheme-level `if`/`pair?` tests.
>
> Eta pushes this into the **VM itself**.  The `Dual` is a first-class
> heap object (`ObjectKind::Dual`), and the four arithmetic opcodes
> (`Add`, `Sub`, `Mul`, `Div`) plus the builtin `exp`, `log`, `sqrt`
> primitives **natively** detect Dual operands.  When a Dual is
> encountered the opcode handler transparently:
>
> 1. Extracts primals and computes the forward result
> 2. Builds a backpropagator closure with chain-rule logic
> 3. Wraps the result in a new `Dual(forward_result, backpropagator)`
>
> This means the backward pass's own arithmetic (`* adj pb`,
> `+ adj_a adj_b`, etc.) also flows through the same Dual-aware
> opcodes.  Seeding the outer backward pass with a Dual-valued adjoint
> automatically creates a **second-level computation graph** — true
> reverse-on-reverse with **zero library-level dispatch overhead**.

### Dedicated Opcodes

Three opcodes in [`bytecode.h`](../eta/core/src/eta/runtime/vm/bytecode.h)
manage the Dual lifecycle:

| Opcode | Stack Effect | Description |
|--------|-------------|-------------|
| `MakeDual` | `primal backprop →` **dual** | Pop a backpropagator closure and a primal value; allocate a `Dual{primal, backprop}` on the heap and push its boxed reference |
| `DualVal` | `x →` **primal** | Pop `x`; if `x` is a `Dual`, push its `.primal` field — otherwise push `x` unchanged (pass-through) |
| `DualBp` | `x →` **closure** | Pop `x`; if `x` is a `Dual`, push its `.backprop` closure — otherwise push a no-op `λ(adj) → '()` |

These compile directly from the Eta forms `make-dual`, `dual-primal`,
and `dual-backprop`.

### Dual-Aware Arithmetic Opcodes

> [!IMPORTANT]
> The critical innovation is inside `do_binary_arithmetic` in
> [`vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp).  When **any**
> of `Add`, `Sub`, `Mul`, or `Div` encounters a Dual operand, the
> opcode handler enters its AD-lifting path **at the C++ level** —
> no Scheme-side dispatch is involved.

The lifting path for each opcode follows the same pattern:

```
┌──────────────────────────────────────────────────────────────┐
│  do_binary_arithmetic(op)                                    │
│                                                              │
│  1. pop b, pop a                                             │
│  2. if (is_dual(a) || is_dual(b)):                           │
│       pa = primal(a),  pb = primal(b)                        │
│       ba = backprop(a), bb = backprop(b)                     │
│                                                              │
│       push pa, push pb                                       │
│       forward_result = do_binary_arithmetic(op)  ← recurse   │
│                                                              │
│       build backpropagator closure:                          │
│         λ(adj) → { compute adj_a, adj_b using chain rule;    │
│                     merge ba(adj_a) ++ bb(adj_b) }           │
│                                                              │
│       push Dual(forward_result, backpropagator)              │
│  3. else:                                                    │
│       plain numeric arithmetic                               │
└──────────────────────────────────────────────────────────────┘
```

Chain-rule adjoint formulas per opcode:

| Opcode | z = | ∂z/∂a | ∂z/∂b | `adj_a` | `adj_b` |
|--------|-----|-------|-------|---------|---------|
| `Add` | a + b | 1 | 1 | `adj` | `adj` |
| `Sub` | a − b | 1 | −1 | `adj` | `adj * (−1)` |
| `Mul` | a × b | b | a | `adj * pb` | `adj * pa` |
| `Div` | a / b | 1/b | −a/b² | `adj / pb` | `adj * (−a/b²)` |

> [!NOTE]
> The `adj * pb` and `adj / pb` computations in the backpropagator
> themselves call `dual_binary_op()`, which re-enters
> `do_binary_arithmetic`.  If `adj` is a Dual (because the *outer*
> `grad` seeded it with a Dual-valued adjoint), this recursive call
> triggers AD lifting **again** — building a second-level tape
> automatically.

### Dual-Aware Builtin Primitives

The builtins `exp`, `log`, and `sqrt` in
[`core_primitives.h`](../eta/core/src/eta/runtime/core_primitives.h)
follow the same pattern.  Each checks whether its argument is a
`Dual` and, if so, builds a lifted result:

| Primitive | Forward | Backward (adjoint rule) |
|-----------|---------|------------------------|
| `exp(Dual(x, bp))` | `Dual(eˣ, …)` | `bp(adj * eˣ)` |
| `log(Dual(x, bp))` | `Dual(ln x, …)` | `bp(adj / x)` |
| `sqrt(Dual(x, bp))` | `Dual(√x, …)` | `bp(adj / (2√x))` |

The backward closures are allocated as `Primitive` heap objects with
explicit `gc_roots` vectors so that captured `LispVal`s (primals,
parent backpropagators) survive garbage collection cycles.

### GC-Safety of Backpropagator Closures

> [!IMPORTANT]
> Each backpropagator is a `Primitive` that captures `LispVal`
> references (`pa`, `pb`, `ba`, `bb`).  These are registered in the
> `Primitive.gc_roots` vector, which the mark-sweep GC traverses
> during the mark phase.  Without this, deeply nested
> reverse-on-reverse tapes would suffer from dangling-pointer
> crashes after a mid-computation GC cycle.

### How Reverse-on-Reverse Works End-to-End

Consider computing the Hessian column for variable *j*:

```
1. Wrap x_j in a Dual: make-dual(x_j, λ(adj) → [(0 . adj)])
2. Call native-grad(f, inputs)
   a. native-grad wraps each variable as a Dual seed
   b. Forward pass: all arithmetic (Add/Sub/Mul/Div, exp, log, sqrt)
      sees Dual operands → builds level-1 tape
   c. Backward pass: call backpropagator with adjoint = 1
      → collects gradient vector (level-1)
3. Because x_j was itself a Dual, the backward pass's arithmetic
   (+, -, *, /) hits the Dual-lifting path again:
   → level-2 tape is built for the inner backward pass
4. Extract H[i][j] from the level-2 backpropagators
```

```mermaid
graph LR
    SEED["seed x_j as Dual"]
    FWD["Forward pass<br/>(level-1 tape)"]
    BWD1["Backward pass<br/>(adj = 1)"]
    BWD2["Level-2 tape<br/>(adj is Dual)"]
    HESS["H[i][j]"]

    SEED --> FWD --> BWD1 --> BWD2 --> HESS
```

### Hessian Computation

```scheme
(defun hessian-col (f vals col-idx)
  ;; Seed variable col-idx with a native Dual adjoint.
  (let ((seed-vals (make-seed-duals vals col-idx)))
    (let ((result (native-grad f seed-vals)))
      ;; Gradient entries are now Duals whose backpropagators
      ;; encode the second-order structure.
      (extract-hessian-col result (length vals)))))

(defun hessian (f vals)
  ;; Full n×n Hessian: one hessian-col per variable.
  …)
```

### Results

The native Dual reverse-on-reverse produces the same second-order
Greeks as the grad-on-Greek approach:

| Greek | Grad-on-Greek | Reverse-on-Reverse | Match? |
|-------|:---:|:---:|:---:|
| Gamma (∂²C/∂S²) | 0.01499 | 0.01499 | ✓ |
| Vanna (∂²C/∂S∂σ) | −0.4890 | −0.4890 | ✓ |
| Volga (∂²C/∂σ²) | 23.29 | 23.29 | ✓ |

The Schwarz symmetry check (H[0][3] vs H[3][0]) matches to ~2×10⁻⁷,
limited only by floating-point precision.

> [!IMPORTANT]
> ### Full Reverse-on-Reverse — No Closed-Form Greeks Required
>
> Unlike the grad-on-Greek approach (§3–§4), the opcode-level
> reverse-on-reverse path does **not** require the user to write
> explicit Delta or Vega functions.  The Hessian is computed purely
> by differentiating through the **original pricing function**
> `bs-call-price-nd` twice — the VM handles everything automatically.
>
> This means the same technique works for:
>
> - **Monte Carlo pricers** with path-level payoffs
> - **Exotic options** (barriers, Asians, lookbacks)
> - **PDE solvers** where Greeks have no closed form
> - **xVA engines** with thousands of risk factors
>
> The only requirement is that the pricing code uses the `nd+`, `nd-`,
> `nd*`, `nd/`, `ndexp`, `ndlog`, `ndsqrt` lifted operations (or
> equivalently, the VM-level `+`, `-`, `*`, `/`, `exp`, `log`,
> `sqrt` which lift transparently when a native Dual is detected).

---

## Summary

| Component | Role |
|-----------|------|
| `norm-pdf` / `norm-cdf-approx` | Plain-arithmetic distribution functions |
| `dnorm-cdf` / `dnorm-pdf` | AD-lifted with custom VJP (exact backward) |
| `bs-d1` | d₁ = [ln(S/K) + (r+σ²/2)T] / (σ√T) |
| `bs-call-price` | C = S·Φ(d₁) − K·e⁻ʳᵀ·Φ(d₂) |
| `bs-delta-fn` | Δ = Φ(d₁) — differentiable for Gamma/Vanna |
| `bs-vega-fn` | 𝒱 = S·φ(d₁)·√T — differentiable for Volga |
| `grad` (1st call) | Price + all first-order Greeks |
| `grad` (2nd call) | Gamma, Vanna, Volga via grad-on-Greek |
| **`MakeDual`** opcode | Allocate `Dual{primal, backprop}` on the heap |
| **`DualVal`** opcode | Extract `.primal` (pass-through for non-Duals) |
| **`DualBp`** opcode | Extract `.backprop` (no-op closure for non-Duals) |
| **`Add`/`Sub`/`Mul`/`Div`** opcodes | Dual-aware: transparently lift when either operand is a Dual |
| **`exp`/`log`/`sqrt`** builtins | Dual-aware: apply chain rule for native Duals |
| `native-grad` / `hessian` | True reverse-on-reverse via native Dual VM type |
| `make-dual` / `dual?` / `dual-primal` / `dual-backprop` | Native Dual primitives |



