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

### The Approach: Grad-on-Greek

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

