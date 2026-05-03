# European Option Greeks with AAD

[← Back to README](../../../README.md) · [AAD Deep-Dive](aad.md) ·
[xVA Example](xva.md) · [Examples](../examples-tour.md)

---

## Overview

[`cookbook/quant/european.eta`](../../../cookbook/quant/european.eta) computes
**Black-Scholes option Greeks** — both first-order and second-order —
using Eta's built-in [tape-based reverse-mode AD](aad.md).

**Key ideas demonstrated:**

- Tape-based AD with transparent recording — plain `+`, `*`, `log`, `exp` etc.
- First-order Greeks (Delta, Vega, Theta, Rho) in a **single backward pass**
- Second-order Greeks (Gamma, Vanna, Volga) by applying `grad` to Greek expressions
- Schwarz's theorem as a built-in consistency check

```bash
etai cookbook/quant/european.eta
```

> [!NOTE]
> This example uses the **Black-Scholes closed-form** rather than Monte
> Carlo simulation.  The AD technique is identical — every `+`, `exp`,
> `norm-cdf` call would work the same way inside a path-level MC loop.
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

### Normal CDF — `norm-cdf`

$$\Phi(x) \approx 1 - \varphi(x) \cdot \bigl(a_1 t + a_2 t^2 + a_3 t^3 + a_4 t^4 + a_5 t^5\bigr)$$

where $t = 1/(1 + 0.2316419 \, x)$.  This is Abramowitz & Stegun
formula 26.2.17 with maximum absolute error < 7.5 × 10⁻⁸.

All arithmetic uses plain `+`, `*`, `/` — the tape records every step
transparently when the argument is a TapeRef.

```scheme
(defun norm-cdf (x)
  (let ((xv (branch-primal x)))
    (if (< xv 0)
        (- 1.0 (norm-cdf (* -1 x)))
        (let ((t (/ 1.0 (+ 1.0 (* 0.2316419 x)))))
          (- 1.0 (* (* inv-sqrt-2pi (exp (* -0.5 (* x x))))
                     (* t (+ 0.319381530 ...))))))))
```

`branch-primal` is an explicit helper that reads tape values via
`tape-ref-value-of` using the current gradient tape context.

> [!TIP]
> Because the polynomial approximation is computed with standard
> tape-tracked arithmetic, the chain rule flows through every step
> automatically.  No custom VJP is needed — the tape records the
> exact sequence of operations and the backward pass differentiates
> through them.

---

## Black-Scholes Formulas

### d₁ and d₂

$$d_1 = \frac{\ln(S/K) + (r + \sigma^2/2) \cdot T}{\sigma \sqrt{T}}$$

$$d_2 = d_1 - \sigma\sqrt{T}$$

```scheme
(defun bs-d1 (S K r sigma T)
  (/ (+ (log (/ S K))
        (* (+ r (/ (* sigma sigma) 2)) T))
     (* sigma (sqrt T))))
```

### Call Price

$$C = S \cdot \Phi(d_1) - K \cdot e^{-rT} \cdot \Phi(d_2)$$

```scheme
(defun bs-call-price (S K r sigma T)
  (let ((d1  (bs-d1 S K r sigma T))
        (svt (* sigma (sqrt T))))
    (let ((d2 (- d1 svt)))
      (- (* S (norm-cdf d1))
         (* (* K (exp (* -1 (* r T))))
            (norm-cdf d2))))
```

> [!NOTE]
> All functions use **plain arithmetic** — `+`, `-`, `*`, `/`, `log`,
> `exp`, `sqrt`.  When called inside `grad`, the arguments are TapeRefs
> and the VM records every operation onto the active tape.  No `d+`,
> `dlog`, `dexp` wrappers are needed.

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

### Grad-on-Greek

To compute Gamma (∂²C/∂S²), we express **Delta as a function**
and differentiate it:

$$\Delta(S,K,r,\sigma,T) = \Phi(d_1)$$

```scheme
(defun bs-delta-fn (S K r sigma T)
  (norm-cdf (bs-d1 S K r sigma T)))

(grad (lambda (S K r sigma T)
        (bs-delta-fn S K r sigma T))
      '(100.0 90.0 0.03 0.30 0.5))
;; => (delta #(∂Δ/∂S  ∂Δ/∂K  ∂Δ/∂r  ∂Δ/∂σ  ∂Δ/∂T))
;;             Gamma                   Vanna
```

Similarly, for Volga (∂²C/∂σ²), express **Vega as a function**:

$$\mathcal{V}(S,K,r,\sigma,T) = S \cdot \varphi(d_1) \cdot \sqrt{T}$$

```scheme
(defun bs-vega-fn (S K r sigma T)
  (* S (* (norm-pdf (bs-d1 S K r sigma T)) (sqrt T))))

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

## How Tape-Based AD Works

The `grad` function:

1. Creates a fresh **tape** (Wengert list)
2. Registers each input as an **independent variable** (TapeRef)
3. **Activates the tape** — the VM's `+`, `-`, `*`, `/`, `exp`, `log`,
   `sqrt` now transparently record every operation involving a TapeRef
4. Evaluates the function — a computation graph is built (~32 bytes
   per tape entry)
5. Runs the **backward pass** — sweeps the tape in reverse,
   accumulating adjoints via the chain rule
6. Extracts adjoints for each input variable

```
┌──────────────────────────────────────────────────────────────┐
│  tape-aware arithmetic (Add/Sub/Mul/Div)                     │
│                                                              │
│  1. pop b, pop a                                             │
│  2. if (is_tape_ref(a) || is_tape_ref(b)):                   │
│       pa = tape.primal(a),  pb = tape.primal(b)              │
│       forward_result = pa op pb                              │
│       tape.append(op, result, arg_a_idx, arg_b_idx)          │
│       push TapeRef(new_index)                                │
│  3. else:                                                    │
│       plain numeric arithmetic                               │
│  ──────────────────────────────────────────────────────────  │
│  Backward sweep:                                             │
│       for each entry in reverse:                             │
│         compute ∂result/∂arg_a and ∂result/∂arg_b            │
│         arg_a.adjoint += result.adjoint * ∂result/∂arg_a     │
│         arg_b.adjoint += result.adjoint * ∂result/∂arg_b     │
└──────────────────────────────────────────────────────────────┘
```

**Key advantages over closure-based AD:**

| | Tape-based (current) | Closure-based (library) |
|---|---|---|
| Per-op overhead | ~32 bytes, zero closures | cons pair + closure + captures |
| Recording | C++ level (no Scheme dispatch) | Scheme-level if/pair?/cons |
| Backward pass | Single reverse sweep over flat array | Recursive closure calls |
| Memory layout | Cache-friendly contiguous array | Pointer-chasing heap objects |
| GC pressure | Minimal (one tape allocation) | High (closure per operation) |

---

## Summary

| Component | Role |
|-----------|------|
| `norm-pdf` | φ(x) — normal PDF (tape-recorded) |
| `norm-cdf` | Φ(x) — polynomial CDF (tape-recorded, chain rule flows through) |
| `bs-d1` | d₁ = [ln(S/K) + (r+σ²/2)T] / (σ√T) |
| `bs-call-price` | C = S·Φ(d₁) − K·e⁻ʳᵀ·Φ(d₂) |
| `bs-delta-fn` | Δ = Φ(d₁) — differentiable for Gamma/Vanna |
| `bs-vega-fn` | 𝒱 = S·φ(d₁)·√T — differentiable for Volga |
| `grad` (1st call) | Price + all first-order Greeks |
| `grad` (2nd call) | Gamma, Vanna, Volga via grad-on-Greek |
| `tape-new` / `tape-start!` / `tape-stop!` | Tape lifecycle management |
| `tape-var` / `tape-backward!` / `tape-adjoint` | Variable registration, backward sweep, adjoint extraction |
| `+`/`-`/`*`/`/`/`exp`/`log`/`sqrt` | Tape-aware: transparently record when TapeRef operands present |

## Example

```console
==================================================
 European Option Greeks with Tape-Based AAD
==================================================

Market parameters:
  S     = 100.0  (spot)
  K     =  90.0  (strike)
  r     =  3%    (risk-free rate)
  sigma =  30%   (volatility)
  T     =  0.5   (maturity, years)


-- First-Order Greeks (single backward pass) --

  Price   = 14.8807

  Delta   = 0.74967
  Rho     = 30.0431
  Vega    = 22.4859
  -Theta  = 8.54837

  Normalised:
    Vega  (per 1% vol)  = 0.224859
    Rho   (per 1% rate) = 0.300431
    Theta (per day)     = -0.0234042


-- Second-Order Greeks (grad-on-Greek) --

  grad(Delta):
    Delta         = 0.74967
    Gamma (dD/dS)  = 0.0149906
    Vanna (dD/dsigma) = -0.488997

  grad(Vega):
    Vega          = 22.4859
    Vanna (dV/dS)  = -0.488997
    Volga (dV/dsigma) = 23.2861


-- Schwarz's Theorem Check --

  Vanna (dDelta/dsigma) = -0.488997
  Vanna (dVega/dS)      = -0.488997
  Difference            = -5.55112e-17
  (Should be ~0 by Schwarz's theorem)

-- Summary --

  Greek      | AD Value
  -----------+-----------
  Price      | 14.8807
  Delta      | 0.74967
  Vega       | 22.4859
  Rho        | 30.0431
  -Theta     | 8.54837
  Gamma      | 0.0149906
  Vanna      | -0.488997
  Volga      | 23.2861


-- Tape-Based AD Architecture --

  The tape (Wengert list) records ~32 bytes per operation.
  No closures allocated — recording happens at the C++ level.
  The backward sweep accumulates adjoints in a single pass.

  For this BS pricing example:
    - ~30 elementary operations recorded per evaluation
    - One backward sweep yields all 5 first-order Greeks
    - Second-order Greeks via nested grad calls
```
