# xVA — Valuation Adjustments with Adjoint AD

[← Back to README](../README.md) · [AAD Deep-Dive](aad.md) ·
[Examples](examples.md) · [Architecture](architecture.md)

---

## Overview

[`examples/xva.eta`](../examples/xva.eta) builds on the
[reverse-mode AD library](aad.md) to illustrate **xVA** 
calculations and their **sensitivities**.

**Key ideas:**

- CVA (Credit Valuation Adjustment) and FVA (Funding Valuation
  Adjustment) computed analytically with lifted AD operations
- Market-risk sensitivities are obtained in a single backward
  pass. 
- Build domain-specific primitives (`discount-factor`,
  `survival-prob`, `expected-exposure`) on top of the generic AD
  library
- Additional AD operations (`dsqrt`, `dpow`) composed from `dexp`
  and `dlog`

```bash
etai examples/xva.eta
```

---

## What Are xVAs?

| Adjustment | Full Name | What It Captures |
|------------|-----------|------------------|
| **CVA** | Credit Valuation Adjustment | Expected loss from counterparty default |
| **DVA** | Debit Valuation Adjustment | Benefit from own default (controversial) |
| **FVA** | Funding Valuation Adjustment | Cost of funding uncollateralised exposure |
| **KVA** | Capital Valuation Adjustment | Cost of regulatory capital |
| **MVA** | Margin Valuation Adjustment | Cost of posting initial margin |

The example implements **CVA** and **FVA**.

---

## Model Setup

### Trade

An at-the-money forward contract (or interest-rate swap) with
notional *N* and maturity *T* = 5 years, uncollateralised.

### Market Parameters

| Parameter | Symbol | Value | Description |
|-----------|--------|-------|-------------|
| Notional | *N* | 1 000 000 | Trade notional (USD) |
| Volatility | *σ* | 20 % | Annualised volatility of the underlying |
| Risk-free rate | *r* | 5 % | Continuous compounding |
| Hazard rate | *λ* | 2 % | Counterparty default intensity (≈ BBB) |
| Loss given default | *LGD* | 60 % | 1 − recovery rate |
| Funding spread | *s_f* | 120 bp | Bank's unsecured funding cost over risk-free |

All six parameters are **dual-valued** — the AD system tracks
derivatives through every operation that touches them.

### Time Grid

Semi-annual buckets: *t* ∈ {0.5, 1.0, 1.5, …, 5.0}, giving 10
time steps.  The midpoint of each bucket is used for exposure and
discounting.

---

## Financial Building Blocks

### Discount factor

$$\text{DF}(r, t) = e^{-r \cdot t}$$

```scheme
(defun discount-factor (r t)
  (dexp (d* (d* -1 r) t)))
```

The risk-free discount factor.  `r` is a dual; `t` is a plain
number (a time point on the grid).  The AD system automatically
tracks ∂DF/∂r.

### Survival probability (hazard-rate model)

$$Q(\lambda, t) = e^{-\lambda \cdot t}$$

```scheme
(defun survival-prob (hazard-rate t)
  (dexp (d* (d* -1 hazard-rate) t)))
```

The probability that the counterparty has *not* defaulted by time
*t*, under a constant hazard-rate model.

### Default probability

$$\text{PD}(\lambda, t) = 1 - e^{-\lambda \cdot t}$$

```scheme
(defun default-prob (hazard-rate t)
  (d- 1 (survival-prob hazard-rate t)))
```

### Marginal default probability

$$\Delta\text{PD}(t_1, t_2) = \text{PD}(t_2) - \text{PD}(t_1)$$

The probability of defaulting in the interval $(t_1, t_2]$.

```scheme
(defun marginal-pd (hazard-rate t1 t2)
  (d- (default-prob hazard-rate t2)
      (default-prob hazard-rate t1)))
```

### Expected Positive Exposure (simplified)

$$\text{EE}(t) \approx N \cdot \sigma \cdot \sqrt{t}$$

```scheme
(defun expected-exposure (notional sigma t)
  (d* notional (d* sigma (dsqrt t))))
```

This is the leading-order approximation for the expected positive
exposure of an at-the-money forward under geometric Brownian
motion.  A production system would use Monte Carlo simulation, but
the AD plumbing is identical — every path-level operation would
use `d+`, `d*`, `dexp`, etc., and the backward pass would
propagate through all of them.

### `dsqrt` — lifted square root

```scheme
(defun dsqrt (a)
  (dexp (d* 0.5 (dlog a))))
```

Computed as $\sqrt{x} = e^{\frac{1}{2} \ln x}$.  The chain rule
through `dexp` and `dlog` automatically yields
$\partial\sqrt{x}/\partial x = 1/(2\sqrt{x})$.

---

## CVA Formula

$$\text{CVA} = \text{LGD} \times \sum_{i=1}^{n} \text{EE}(\bar{t}_i) \times \text{DF}(\bar{t}_i) \times \Delta\text{PD}(t_{i-1}, t_i)$$

where $\bar{t}_i = (t_{i-1} + t_i) / 2$ is the bucket midpoint.

**Interpretation:** for each time bucket, multiply:
1. The expected exposure if default occurs at that time
2. The discount factor to present-value it
3. The probability of defaulting in that bucket
4. The loss given default

Sum over all buckets to get the total expected loss from
counterparty credit risk.

```scheme
(defun cva-bucket (notional sigma r hazard-rate lgd t-prev t-curr)
  (let ((t-mid (* 0.5 (+ t-prev t-curr))))
    (d* lgd
      (d* (expected-exposure notional sigma t-mid)
        (d* (discount-factor r t-mid)
            (marginal-pd hazard-rate t-prev t-curr))))))

(defun cva-loop (notional sigma r hazard-rate lgd times prev-t acc)
  (if (null? times)
      acc
      (cva-loop notional sigma r hazard-rate lgd
        (cdr times) (car times)
        (d+ acc (cva-bucket notional sigma r hazard-rate lgd
                            prev-t (car times))))))

(defun compute-cva (notional sigma r hazard-rate lgd)
  (cva-loop notional sigma r hazard-rate lgd
    '(0.5 1.0 1.5 2.0 2.5 3.0 3.5 4.0 4.5 5.0)
    0.0 (make-const 0)))
```

---

## FVA Formula

$$\text{FVA} = \sum_{i=1}^{n} \text{EE}(\bar{t}_i) \times \text{DF}(\bar{t}_i) \times s_f \times \Delta t_i$$

**Interpretation:** the bank must fund the expected positive
exposure at its unsecured funding spread $s_f$.  Each bucket
contributes exposure × discount × spread × time.

```scheme
(defun fva-bucket (notional sigma r funding-spread t-prev t-curr)
  (let ((t-mid (* 0.5 (+ t-prev t-curr)))
        (dt    (- t-curr t-prev)))
    (d* (expected-exposure notional sigma t-mid)
      (d* (discount-factor r t-mid)
        (d* funding-spread dt)))))
```

---

## Total xVA

$$\text{xVA} = \text{CVA} + \text{FVA}$$

```scheme
(defun total-xva (notional sigma r hazard-rate lgd funding-spread)
  (d+ (compute-cva notional sigma r hazard-rate lgd)
      (compute-fva notional sigma r funding-spread)))
```

All six parameters are dual-valued inputs.  A single call to
`grad` evaluates the forward pass *and* the backward pass,
returning the xVA value and all six partial derivatives.

---

## Sensitivities (Greeks)

The `grad` function seeds the output's backpropagator with
adjoint = 1 and collects contributions for every input variable:

```scheme
(grad (lambda (notional sigma r hazard-rate lgd funding-spread)
        (total-xva notional sigma r hazard-rate lgd funding-spread))
      '(1000000 0.20 0.05 0.02 0.60 0.012))
;; => (xva-value  #(∂/∂N  ∂/∂σ  ∂/∂r  ∂/∂λ  ∂/∂LGD  ∂/∂s_f))
```

| Greek | Parameter | Risk Interpretation |
|-------|-----------|---------------------|
| ∂xVA/∂*N* | Notional | xVA per unit notional (linearity check) |
| ∂xVA/∂*σ* | Volatility | **Vega** — exposure to vol moves |
| ∂xVA/∂*r* | Risk-free rate | **Rho** — rate sensitivity of discounting & exposure |
| ∂xVA/∂*λ* | Hazard rate | **CS01** — credit-spread sensitivity |
| ∂xVA/∂*LGD* | Loss given default | Recovery-rate sensitivity |
| ∂xVA/∂*s_f* | Funding spread | Funding-spread sensitivity |

In production these sensitivities drive:

- **Hedging** — delta-hedge CVA with CDS, FVA with funding
  instruments
- **P&L explain** — attribute daily P&L to each risk factor
- **Regulatory capital** — SA-CVA and BA-CVA under Basel III /
  FRTB require first-order sensitivities

---

## Why AAD Matters Here

A typical xVA book at a large bank contains hundreds of thousands
of trades across many counterparties.  The exposure simulation
might involve millions of Monte Carlo paths.  Computing Greeks by
finite difference ("bump and reval") requires *one full
re-evaluation per parameter* — if you have 10 000 risk factors,
that is 10 000× the base cost.

With AAD the cost of the **full gradient** is bounded by a small
constant multiple (typically 3–5×) of the cost of a *single*
forward evaluation, **regardless of the number of risk factors**.
This is the same complexity advantage that backpropagation gives
neural-network training.

```
Finite difference:   cost = O(N) × forward_pass
AAD (reverse mode):  cost = O(1) × forward_pass   (for all N Greeks)
```

The Eta example demonstrates the principle with 6 parameters and
an analytic exposure model.  Scaling to a full Monte Carlo engine
changes nothing about the AD machinery — every arithmetic
operation simply uses the `d`-prefixed functions, and the backward
pass propagates through all of them.

---

## Summary

| Component | Role |
|-----------|------|
| `discount-factor` | Risk-free discounting, DF = e^{−rt} |
| `survival-prob` / `default-prob` | Hazard-rate credit model |
| `marginal-pd` | Bucket-level default probability |
| `expected-exposure` | Simplified EE ≈ Nσ√t |
| `dsqrt` / `dpow` | AD-lifted power functions |
| `compute-cva` | CVA = LGD × Σ EE × DF × ΔPD |
| `compute-fva` | FVA = Σ EE × DF × s_f × Δt |
| `total-xva` | CVA + FVA with full gradient |
| `grad` | One backward pass → all 6 sensitivities |

