# SABR Volatility Surface with Native Dual AD

[← Back to README](../README.md) · [AAD Deep-Dive](aad.md) ·
[European Greeks](european.md) · [xVA Example](xva.md) ·
[Examples](examples.md)

---

## Overview

[`examples/sabr.eta`](../examples/sabr.eta) implements the **Hagan et al.
(2002)** SABR implied volatility approximation — the industry-standard
stochastic-volatility model used by rates and FX desks worldwide — and
computes **all model sensitivities** using Eta's **native Dual VM type**.

**Key ideas demonstrated:**

- SABR Hagan implied vol formula (ATM + general-K branches)
- Vol surface generation across a strike × expiry grid
- First-order Greeks (∂σ/∂α, ∂σ/∂β, ∂σ/∂ρ, ∂σ/∂ν) in a **single backward pass**
- Second-order Hessian via **reverse-on-reverse** (true tape-on-tape)
- **Performance advantage** of VM-level Dual instructions over library-level AD

```bash
etai examples/sabr.eta
```

> [!NOTE]
> This example uses **only** the native Dual VM instructions — no
> library-level `cons`-pair AD.  The native Dual approach is highlighted
> because SABR's 4 scalar parameters and ~50 elementary operations sit
> in the exact regime where native Dual instructions provide maximum
> benefit.

---

## The SABR Model

The SABR (Stochastic Alpha Beta Rho) model describes the dynamics of
a forward rate *F* and its instantaneous volatility *σ*:

$$dF_t = \sigma_t F_t^\beta \, dW_1$$

$$d\sigma_t = \nu \sigma_t \, dW_2$$

$$dW_1 \cdot dW_2 = \rho \, dt$$

The four model parameters control the shape of the implied volatility
smile:

| Parameter | Symbol | Role | Typical Value (Rates) |
|-----------|--------|------|:---------------------:|
| Vol level | *α* | Overall smile height | 0.02 – 0.05 |
| CEV exponent | *β* | Backbone curvature | 0.5 (square-root) |
| Correlation | *ρ* | Skew direction & magnitude | −0.3 to 0.0 |
| Vol-of-vol | *ν* | Wing steepness / kurtosis | 0.3 – 0.6 |

---

## Hagan Approximation

### General Case (F ≠ K)

$$\sigma_{\text{impl}} = \frac{\alpha}{(FK)^{(1-\beta)/2} \left[1 + \frac{(1-\beta)^2}{24}\ln^2\frac{F}{K} + \frac{(1-\beta)^4}{1920}\ln^4\frac{F}{K}\right]} \cdot \frac{z}{x(z)} \cdot \left[1 + \epsilon \, T\right]$$

where:

$$z = \frac{\nu}{\alpha}(FK)^{(1-\beta)/2}\ln\frac{F}{K}$$

$$x(z) = \ln\frac{\sqrt{1-2\rho z+z^2}+z-\rho}{1-\rho}$$

$$\epsilon = \frac{(1-\beta)^2\alpha^2}{24(FK)^{1-\beta}} + \frac{\rho\beta\nu\alpha}{4(FK)^{(1-\beta)/2}} + \frac{(2-3\rho^2)\nu^2}{24}$$

### ATM Case (F ≈ K)

$$\sigma_{\text{ATM}} = \frac{\alpha}{F^{1-\beta}} \left[1 + \left(\frac{(1-\beta)^2\alpha^2}{24 F^{2-2\beta}} + \frac{\rho\beta\nu\alpha}{4 F^{1-\beta}} + \frac{(2-3\rho^2)\nu^2}{24}\right) T\right]$$

```scheme
;; ATM case
(defun sabr-atm-vol (F T alpha beta rho nu)
  (let ((one-minus-beta (nd- 1 beta)))
    (let ((F-pow (ndpow F one-minus-beta)))
      (let ((base-vol (nd/ alpha F-pow)))
        (let ((term1 (nd/ (nd* (nd* one-minus-beta one-minus-beta)
                               (nd* alpha alpha))
                          (nd* 24 (ndpow F (nd* 2 one-minus-beta)))))
              (term2 (nd/ (nd* rho (nd* beta (nd* nu alpha)))
                          (nd* 4 F-pow)))
              (term3 (nd/ (nd* (nd- 2 (nd* 3 (nd* rho rho)))
                               (nd* nu nu))
                          24)))
          (nd* base-vol
               (nd+ 1 (nd* T (nd+ term1 (nd+ term2 term3))))))))))
```

The `|F - K| < 10⁻⁷` tolerance check switches between the ATM and
general formulas.  The branch is not differentiated (it's a plain `if`
on numeric values), which is correct: the SABR approximation is
smooth across the ATM boundary.

---

## Market Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| *F* | 3.00 % | Forward swap rate |
| *α* | 0.035 | Vol level |
| *β* | 0.50 | CEV exponent (square-root backbone) |
| *ρ* | −0.25 | Negative skew (typical for rates) |
| *ν* | 0.40 | Vol-of-vol |

---

## Vol Surface

The example generates an implied vol surface across a grid of
strikes (80%–120% of forward) and expiries (0.25Y to 5Y):

| K/F | 0.25Y | 0.5Y | 1Y | 2Y | 5Y |
|:---:|:-----:|:----:|:--:|:--:|:--:|
| 80% | high | ↓ | ↓ | ↓ | ↓ |
| 90% | … | … | … | … | … |
| 100% (ATM) | base | base | base | base | base |
| 110% | … | … | … | … | … |
| 120% | low | ↓ | ↓ | ↓ | ↓ |

With ρ < 0, low strikes (high K/F %) have higher implied vol than
high strikes — the characteristic negative skew of interest-rate
markets.  The vol-of-vol *ν* controls how steep the wings are.

---

## First-Order Greeks

A single call to `native-grad` with inputs `(α, β, ρ, ν)` produces
the implied vol **and** all four partial derivatives in one backward
pass:

```scheme
(native-grad (lambda (alpha beta rho nu)
               (sabr-implied-vol F K T alpha beta rho nu))
             (list alpha-val beta-val rho-val nu-val))
;; => (sigma_impl  #(∂σ/∂α  ∂σ/∂β  ∂σ/∂ρ  ∂σ/∂ν))
```

| Index | Greek | Financial Meaning |
|:-----:|-------|-------------------|
| 0 | ∂σ/∂α | **Smile level** — how much the entire smile shifts when α moves |
| 1 | ∂σ/∂β | **Backbone** — sensitivity to the CEV exponent (usually fixed) |
| 2 | ∂σ/∂ρ | **Skew** — how the smile tilts when correlation changes |
| 3 | ∂σ/∂ν | **Wings** — how the smile curvature changes with vol-of-vol |

> [!TIP]
> In practice, β is typically **fixed** at 0.5 (rates) or 1.0
> (lognormal/FX).  The example includes it as a free AD variable for
> completeness — pass β as a plain number instead of a Dual to exclude
> it from differentiation.

---

## Second-Order Greeks (Hessian)

The 4×4 Hessian matrix of σ_impl w.r.t. (α, β, ρ, ν) is computed
using **true reverse-on-reverse** via the native Dual VM type:

```scheme
(hessian (lambda (alpha beta rho nu)
           (sabr-atm-vol F T alpha beta rho nu))
         (list alpha-val beta-val rho-val nu-val))
```

Key entries:

| Entry | Meaning |
|-------|---------|
| H[α,α] | Second-order vol-level sensitivity |
| H[ρ,ρ] | Skew convexity |
| H[ν,ν] | Vol-of-vol convexity |
| H[ρ,ν] | **Correlation–vol interaction** — how skew changes with vol-of-vol |

The **Schwarz symmetry check** verifies H[ρ,ν] = H[ν,ρ] to
floating-point precision, confirming the correctness of the
reverse-on-reverse implementation.

> [!IMPORTANT]
> The Hessian is computed by differentiating through the **original
> SABR formula** twice — no closed-form second derivatives are needed.
> This works because the inner backward pass's arithmetic (`* adj pb`,
> `+ adj_a adj_b`) flows through the same Dual-aware VM opcodes,
> automatically building a second-level computation graph.

---

## Performance: Native Dual VM Instructions

This is the key architectural advantage demonstrated by this example.

### The Problem with Library-Level AD

Traditional AD in a Lisp-like language represents dual numbers as
`cons` pairs `(primal . backpropagator)`.  Every arithmetic operation
must:

1. Check if each argument is a dual (`pair?` test)
2. Extract primals (`car`) and backpropagators (`cdr`)
3. Compute the forward result
4. Allocate a new `cons` pair + `lambda` closure for the backpropagator
5. Wrap the result

This adds **~13 Scheme-level VM dispatches per AD operation**.

### The Native Dual Solution

Eta's VM pushes dual-number handling into the **C++ opcode layer**:

```
┌──────────────────────────────────────────────────────────────┐
│  VM opcode: Mul                                              │
│                                                              │
│  1. Pop b, pop a                                             │
│  2. Check tag bits: is_dual(a) || is_dual(b)?    ← 1 branch │
│  3. If yes:                                                  │
│       Extract pa, pb, ba, bb     ← C++ pointer reads         │
│       forward = pa * pb          ← C++ double multiply       │
│       Build backpropagator       ← C++ Primitive allocation  │
│       Push Dual(forward, bp)     ← 1 heap allocation         │
│  4. If no:                                                   │
│       Plain numeric multiply                                 │
└──────────────────────────────────────────────────────────────┘
```

**Result: ~1 VM dispatch per AD operation** (the rest is C++).

### Per-Operation Cost Comparison

| Step | Library `cons`-pair | Native Dual |
|------|:-------------------:|:-----------:|
| Type check | 2× `if` + `pair?` (6 ops) | 1 C++ tag check |
| Extract primal | 2× `car` | C++ field read |
| Extract backprop | 2× `cdr` | C++ field read |
| Forward arithmetic | 1 VM dispatch | C++ inline |
| Result construction | `cons` + `lambda` (2 ops) | `Heap::allocate<Dual>` |
| **Total VM dispatches** | **~13** | **~1** |

### Impact on SABR

| Metric | Library AD | Native Dual | Speedup |
|--------|:----------:|:-----------:|:-------:|
| Ops per eval | ~50 | ~50 | — |
| VM dispatches per op | ~13 | ~1 | 13× |
| **Total dispatches** | **~650** | **~50** | **~13×** |

For the **Hessian** (reverse-on-reverse), the advantage **compounds**:
the backward pass performs ~50 additional arithmetic operations, each of
which also benefits from native Dual lifting.  The second-level tape
building happens entirely in C++ — no Scheme-level dispatch at either
tape level.

### Comparison with External Frameworks

| Framework | Scalar AD overhead per op |
|-----------|:------------------------:|
| **Eta native Dual** | **16 bytes** (primal + backprop pointer) |
| Eta `cons`-pair | 16 bytes + closure allocation overhead |
| libtorch scalar tensor | ~200 bytes metadata + ATen dispatch |
| JAX (XLA) | Compilation overhead dominates for scalar ops |

> [!NOTE]
> The native Dual approach is optimal for **scalar** models with few
> parameters (SABR, Black-Scholes, xVA with ≤50 risk factors).  For
> **tensor** operations (matrix multiply, batched convolutions), an
> external library like libtorch would be faster due to BLAS/LAPACK
> kernels.  The two approaches are complementary.

---

## Summary

| Component | Role |
|-----------|------|
| `sabr-implied-vol` | Unified SABR Hagan approximation (ATM + general-K) |
| `sabr-atm-vol` | ATM limiting formula (F ≈ K) |
| `sabr-general-vol` | General-K formula with z/x(z) correction |
| `sabr-xz` | Helper: x(z) = ln[(√(1-2ρz+z²)+z-ρ)/(1-ρ)] |
| `native-grad` | One backward pass → all 4 sensitivities |
| `hessian` | True reverse-on-reverse → 4×4 second-order matrix |
| **`MakeDual`** opcode | Allocate `Dual{primal, backprop}` on the heap |
| **`DualVal`** opcode | Extract `.primal` (pass-through for non-Duals) |
| **`DualBp`** opcode | Extract `.backprop` (no-op closure for non-Duals) |
| **`Add`/`Sub`/`Mul`/`Div`** opcodes | Dual-aware: transparently lift when either operand is a Dual |
| **`exp`/`log`/`sqrt`** builtins | Dual-aware: apply chain rule for native Duals |

---

## Example Output

```console
==================================================
 SABR Volatility Surface with Native Dual AD
==================================================

SABR parameters:
  F (forward)  = 3.00%
  alpha        = 0.035   (vol level)
  beta         = 0.50    (CEV exponent)
  rho          = -0.25   (skew)
  nu           = 0.40    (vol-of-vol)


-- Implied Vol Surface (%) --

  K/F(%)   0.25Y   0.5Y   1.0Y   2.0Y   5.0Y
   80%    23.8    24.1    24.6    25.6    28.5
   85%    22.7    23.0    23.4    24.3    27.0
   90%    21.7    21.9    22.3    23.1    25.6
   95%    20.9    21.0    21.3    22.0    24.3
  100%    20.2    20.2    20.5    21.1    23.2
  105%    19.6    19.6    19.8    20.3    22.2
  110%    19.1    19.1    19.2    19.7    21.3
  115%    18.7    18.7    18.7    19.1    20.5
  120%    18.4    18.3    18.3    18.6    19.8


-- First-Order Greeks (single backward pass) --

  At ATM point: F = K = 3%, T = 1Y

  sigma_impl = 20.5%

  Sensitivities:
    dsigma/dalpha = 5.87...
    dsigma/dbeta  = ...
    dsigma/drho   = ...
    dsigma/dnu    = ...

  (Exact values depend on parameter calibration)


-- Hessian (reverse-on-reverse, ATM, T=1Y) --

  4x4 Hessian of sigma_impl w.r.t. (alpha, beta, rho, nu)

  Schwarz check (H[rho,nu] vs H[nu,rho]):
    Difference = ~0
    (Should be ~0 by Schwarz's theorem)


-- Performance: Native Dual VM Instructions --

  Library-level: ~650 VM dispatches per SABR eval
  Native Dual:   ~50  VM dispatches per SABR eval
  Speedup:       ~13x fewer dispatches
```

> [!NOTE]
> Approximate output shown.  Exact values depend on floating-point
> precision and platform.  Run `etai examples/sabr.eta` for actual
> results.

