# SABR Volatility Model 

[← Back to README](../../../README.md) · [AAD Deep-Dive](aad.md) ·
[European Greeks](european.md) · [xVA Example](xva.md) ·
[Examples](../examples-tour.md)

---

## Overview

[`cookbook/quant/sabr.eta`](../../../cookbook/quant/sabr.eta) implements the **Hagan et al.
(2002)** SABR implied volatility approximation and computes **all model
sensitivities** using Eta's built-in **tape-based reverse-mode AD**.

**Key ideas demonstrated:**

- SABR Hagan implied vol formula (ATM + general-K branches)
- Vol surface generation across a strike × expiry grid
- First-order Greeks (∂σ/∂α, ∂σ/∂β, ∂σ/∂ρ, ∂σ/∂ν) in a **single backward pass**
- Plain arithmetic (`+`, `-`, `*`, `/`, `exp`, `log`, `sqrt`) with transparent tape recording

```bash
etai cookbook/quant/sabr.eta
```

> [!NOTE]
> The SABR formula uses **plain arithmetic** — no lifted `nd+`, `nd*`,
> `ndlog` wrappers.  The VM transparently records onto a tape when
> TapeRef operands are present, making the source code identical to
> a non-AD version.

---

## The SABR Model

The SABR (Stochastic Alpha Beta Rho) model describes the dynamics of
a forward rate *F* and its instantaneous volatility *σ*:

$$dF_t = \sigma_t F_t^\beta \, dW_1$$

$$d\sigma_t = \nu \sigma_t \, dW_2$$

$$dW_1 \cdot dW_2 = \rho \, dt$$

| Parameter | Symbol | Role | Typical Range |
|-----------|--------|------|:-------------:|
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
(defun sabr-atm-vol (F T alpha beta rho nu)
  (let ((one-minus-beta (- 1 beta)))
    (let ((F-pow (ndpow F one-minus-beta)))
      (let ((base-vol (/ alpha F-pow)))
        (let ((term1 (/ (* (* one-minus-beta one-minus-beta)
                            (* alpha alpha))
                         (* 24 (ndpow F (* 2 one-minus-beta)))))
              (term2 (/ (* rho (* beta (* nu alpha)))
                         (* 4 F-pow)))
              (term3 (/ (* (- 2 (* 3 (* rho rho)))
                            (* nu nu))
                         24)))
          (* base-vol
             (+ 1 (* T (+ term1 (+ term2 term3))))))))))
```

The `|F − K| < 10⁻⁷` tolerance switches between the ATM and general
formulas.  The branch is a plain `if` on numeric values and is not
differentiated — correct because the SABR approximation is smooth
across the ATM boundary.

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

The example generates an implied vol surface across strikes
(80 %–120 % of forward) and expiries (0.25 Y–5 Y):

| K/F | 0.25Y | 0.5Y | 1Y | 2Y | 5Y |
|:---:|:-----:|:----:|:--:|:--:|:--:|
| 80% | high | ↓ | ↓ | ↓ | ↓ |
| 90% | … | … | … | … | … |
| 100% (ATM) | base | base | base | base | base |
| 110% | … | … | … | … | … |
| 120% | low | ↓ | ↓ | ↓ | ↓ |

With ρ < 0, low strikes have higher implied vol — the characteristic
negative skew of interest-rate markets.

---

## First-Order Greeks

A single `grad` call with inputs `(α, β, ρ, ν)` produces the
implied vol **and** all four partial derivatives in one backward pass:

```scheme
(grad (lambda (alpha beta rho nu)
         (sabr-implied-vol F K T alpha beta rho nu))
       (list alpha-val beta-val rho-val nu-val))
;; => (sigma_impl  #(∂σ/∂α  ∂σ/∂β  ∂σ/∂ρ  ∂σ/∂ν))
```

| Index | Greek | Financial Meaning |
|:-----:|-------|-------------------|
| 0 | ∂σ/∂α | **Smile level** — entire smile shifts when α moves |
| 1 | ∂σ/∂β | **Backbone** — sensitivity to the CEV exponent |
| 2 | ∂σ/∂ρ | **Skew** — smile tilt when correlation changes |
| 3 | ∂σ/∂ν | **Wings** — smile curvature with vol-of-vol |

> [!TIP]
> In practice β is typically **fixed** (0.5 for rates, 1.0 for FX).
> Pass it as a plain number instead of a TapeRef to exclude it from
> differentiation.

---

## Tape-Based AD Architecture

SABR's ~50 elementary operations on 4 scalar parameters work
naturally with Eta's tape-based AD.  When `grad` creates TapeRef
variables and activates the tape, the VM's `+`, `-`, `*`, `/`,
`exp`, `log`, `sqrt` transparently record each operation (~32 bytes
per entry, zero closure allocations).

### Key Benefits

| Metric | Tape-based AD |
|--------|:------------:|
| Ops per SABR eval | ~50 |
| Memory per op | ~32 bytes |
| Closures allocated | 0 |
| VM dispatches per op | 1 |
| Backward pass | Single reverse sweep |

### Comparison with External Frameworks

| Framework | Scalar AD overhead per op |
|-----------|:------------------------:|
| **Eta tape** | **~32 bytes** (op + primal + indices + adjoint) |
| Library `cons`-pair | ~16 bytes + closure allocation overhead |
| libtorch scalar tensor | ~200 bytes metadata + ATen dispatch |
| JAX (XLA) | Compilation overhead dominates for scalar ops |

> [!NOTE]
> The tape approach is optimal for **scalar** models with few parameters
> (SABR, Black-Scholes, xVA with ≤ 50 risk factors).  For **tensor**
> workloads (matrix multiply, batched convolutions), libtorch is faster
> due to BLAS/LAPACK kernels.  The two approaches are complementary.

---

## Summary

| Component | Role |
|-----------|------|
| `sabr-implied-vol` | Unified Hagan approximation (ATM + general-K) |
| `sabr-atm-vol` | ATM limiting formula (F ≈ K) |
| `sabr-general-vol` | General-K formula with z / x(z) correction |
| `sabr-xz` | Helper: x(z) = ln[(√(1−2ρz+z²)+z−ρ) / (1−ρ)] |
| `grad` | One backward pass → all 4 sensitivities |
| `branch-primal` | Explicit primal extraction helper backed by `tape-ref-value-of` |
| Tape-aware arithmetic | `+`/`-`/`*`/`/`, `exp`/`log`/`sqrt`/`pow` — transparent recording |

---

## Example Output

> [!TIP]
> `etac -O` compiles with optimisations; `etai` runs `.eta` or `.etac`
> files directly.

```console
$ etai sabr.eta
==================================================
 SABR Volatility Surface with Tape-Based AD
==================================================

SABR parameters:
  F (forward)  = 3.00%
  alpha        = 0.035   (vol level)
  beta         = 0.50    (CEV exponent)
  rho          = -0.25   (skew)
  nu           = 0.40    (vol-of-vol)


-- Implied Vol Surface (%) --

  K/F(%)   0.25Y   0.5Y   1Y   2Y   5Y
   80%   23.0051  23.0618  23.1753  23.4022  24.0829
   85%   22.1764  22.2312  22.3409  22.5602  23.2181
   90%   21.4442  21.4974  21.6037  21.8163  22.4541
   95%   20.8057  20.8573  20.9607  21.1674  21.7876
  100%   20.2577  20.3081  20.409  20.6107  21.2159
  105%   19.7969  19.8463  19.945  20.1426  20.7352
  110%   19.4188  19.4674  19.5644  19.7586  20.341
  115%   19.1179  19.1658  19.2615  19.453  20.0275
  120%   18.8876  18.935  19.0297  19.2192  19.7877


-- First-Order Greeks (single backward pass) --

  At ATM point: F = K = 3%, T = 1Y

  sigma_impl = 20.409%

  Sensitivities:
    dsigma/dalpha = 5.82147
    dsigma/dbeta  = -0.71583
    dsigma/drho   = 0.00406239
    dsigma/dnu    = 0.0109325

  Interpretation:
    dalpha -> smile level shift
    dbeta  -> backbone / CEV curvature
    drho   -> skew sensitivity
    dnu    -> wing / kurtosis sensitivity


-- OTM Greeks: K = 2.4% (80% moneyness), T = 1Y --

  sigma_impl = 23.1753%

  Sensitivities:
    dsigma/dalpha = 6.03833
    dsigma/dbeta  = -0.765843
    dsigma/drho   = -0.0340376
    dsigma/dnu    = 0.0623731


-- Tape-Based AD Architecture --

  The SABR Hagan formula involves ~50 elementary operations
  on 4 scalar parameters — ideal for tape-based AD.
  When grad activates the tape, each operation (`+`, `-`, `*`, `/`,
  `exp`, `log`, `sqrt`) transparently records onto the tape
  (32 bytes per entry, zero closure allocations).

  Key benefits:
    - Single reverse sweep for the backward pass
    - No closure allocations — efficient memory usage
    - Transparent recording with plain arithmetic
```
