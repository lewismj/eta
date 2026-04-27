# xVA — Valuation Adjustments with Tape-Based AD

[← Back to README](../README.md) · [AAD Deep-Dive](aad.md) ·
[Examples](examples.md) · [Architecture](architecture.md)

---

> [!IMPORTANT]
> **Scope — illustrative, not production.** This example demonstrates
> the **AAD machinery** that production xVA desks use, but the
> **financial model is deliberately simplified** so the maths fits on
> one page. In particular it uses:
>
> - a **closed-form scalar exposure** $EE(t) \approx N\sigma\sqrt{t}$
>   (no term-structure model, no Monte Carlo, no path-level
>   revaluation, no Bermudan exercise);
> - a **constant hazard rate** $\lambda$ (no CDS-bootstrapped term
>   structure);
> - **single-curve discounting** $DF = e^{-rt}$ (no OIS / forwarding
>   curve split, no multi-currency CSA);
> - **no netting, no collateral / CSA, no MPoR** — exposures are
>   summed per-trade rather than netted, so CVA is materially
>   over-stated for a real book;
> - **no wrong-way risk** — $EE \perp PD$ is assumed (see
>   [causal](causal.md) for how to relax this with `do`-calculus);
> - **FCA only** — no FBA, KVA, or MVA;
> - a **coarse 10-bucket** semi-annual time grid.
>
> For a realistic IR-swaption book the CVA number this example
> produces would be off by **1–2 orders of magnitude**. What *is*
> production-grade is the tape architecture: the same `+`, `-`,
> `*`, `exp`, `log`, `sqrt` that record here would record through a
> Hull–White Monte Carlo path generator unchanged, giving full
> AAD Greeks at ≈3–5× the cost of a single forward pass regardless
> of the number of risk factors. The path from this example to a
> realistic engine is mostly **adding finance** (exposure simulator,
> curves, netting/CSA) rather than changing the runtime.
> 
> See [wrong-way risk](xva-wwr.md) Wrong-Way Risk using `do`-calculus.

---

## Overview

[`examples/xva.eta`](../examples/xva.eta) builds on
Eta's built-in [tape-based reverse-mode AD](aad.md) to illustrate **xVA**
calculations and their **sensitivities**.

**Key ideas:**

- CVA (Credit Valuation Adjustment) and FVA (Funding Valuation
  Adjustment) computed analytically with **plain arithmetic** — the
  VM transparently records onto a tape when TapeRef operands are present
- Market-risk sensitivities are obtained in a single backward
  pass
- Build domain-specific primitives (`discount-factor`,
  `survival-prob`, `expected-exposure`) using plain `+`, `*`, `exp`,
  `log`, `sqrt` — no lifted wrappers needed
- Zero closure allocations — all tape recording happens at the C++ level

```bash
etai examples/xva.eta
```

> [!NOTE]
> The xVA building blocks use **plain arithmetic** — no `d+`, `d*`,
> `dexp` wrappers.  The VM transparently records onto a tape when
> TapeRef operands are present, making the source code identical to
> a non-AD version.

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

## Setup

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

All six parameters are registered as **TapeRef** variables — the tape
automatically records derivatives through every operation that touches them.

### Time Grid

Semi-annual buckets: *t* ∈ {0.5, 1.0, 1.5, …, 5.0}, giving 10
time steps.  The midpoint of each bucket is used for exposure and
discounting.

---

## Building Blocks

### Discount factor

$$\text{DF}(r, t) = e^{-r \cdot t}$$

```scheme
(defun discount-factor (r t)
  (exp (* (* -1 r) t)))
```

The risk-free discount factor.  `r` is a TapeRef when called inside
`grad`; `t` is a plain number (a time point on the grid).  The tape
automatically tracks ∂DF/∂r.

### Survival probability (hazard-rate model)

$$Q(\lambda, t) = e^{-\lambda \cdot t}$$

```scheme
(defun survival-prob (hazard-rate t)
  (exp (* (* -1 hazard-rate) t)))
```

The probability that the counterparty has *not* defaulted by time
*t*, under a constant hazard-rate model.

### Default probability

$$\text{PD}(\lambda, t) = 1 - e^{-\lambda \cdot t}$$

```scheme
(defun default-prob (hazard-rate t)
  (- 1 (survival-prob hazard-rate t)))
```

### Marginal default probability

$$\Delta\text{PD}(t_1, t_2) = \text{PD}(t_2) - \text{PD}(t_1)$$

The probability of defaulting in the interval $(t_1, t_2]$.

```scheme
(defun marginal-pd (hazard-rate t1 t2)
  (- (default-prob hazard-rate t2)
     (default-prob hazard-rate t1)))
```

### Expected Positive Exposure (simplified)

$$\text{EE}(t) \approx N \cdot \sigma \cdot \sqrt{t}$$

```scheme
(defun expected-exposure (notional sigma t)
  (* notional (* sigma (sqrt t))))
```

This is the leading-order approximation for the expected positive
exposure of an at-the-money forward under geometric Brownian
motion.  A production system would use Monte Carlo simulation, but
the AD plumbing is identical — every path-level operation uses
plain arithmetic, and the tape records transparently.

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
    (* lgd
      (* (expected-exposure notional sigma t-mid)
        (* (discount-factor r t-mid)
           (marginal-pd hazard-rate t-prev t-curr))))))

(defun cva-loop (notional sigma r hazard-rate lgd times prev-t acc)
  (if (null? times)
      acc
      (cva-loop notional sigma r hazard-rate lgd
        (cdr times) (car times)
        (+ acc (cva-bucket notional sigma r hazard-rate lgd
                           prev-t (car times))))))

(defun compute-cva (notional sigma r hazard-rate lgd)
  (cva-loop notional sigma r hazard-rate lgd
    '(0.5 1.0 1.5 2.0 2.5 3.0 3.5 4.0 4.5 5.0)
    0.0 0))
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
    (* (expected-exposure notional sigma t-mid)
      (* (discount-factor r t-mid)
        (* funding-spread dt)))))
```

---

## Total xVA (_Ignoring KVA etc._)

$$\text{xVA} = \text{CVA} + \text{FVA}$$

```scheme
(defun total-xva (notional sigma r hazard-rate lgd funding-spread)
  (+ (compute-cva notional sigma r hazard-rate lgd)
     (compute-fva notional sigma r funding-spread)))
```

All six parameters are TapeRef inputs when called inside `grad`.
A single call to `grad` evaluates the forward pass *and* the backward
pass, returning the xVA value and all six partial derivatives.

---

## Sensitivities (Greeks)

The `grad` function creates TapeRef variables, activates the tape,
evaluates the function, then runs a single backward sweep:

```scheme
(grad (lambda (notional sigma r hazard-rate lgd funding-spread)
        (total-xva notional sigma r hazard-rate lgd funding-spread))
      '(1000000 0.20 0.05 0.02 0.60 0.012))
;; => (xva-value  #(∂/∂N  ∂/∂σ  ∂/∂r  ∂/∂λ  ∂/∂LGD  ∂/∂s_f))
```

|  Greek     | Parameter | Risk Interpretation |
|------------|-----------|---------------------|
| ∂xVA/∂*N*  | Notional | xVA per unit notional (linearity check) |
| ∂xVA/∂*σ*  | Volatility | **Vega** — exposure to vol moves |
| ∂xVA/∂*r*  | Risk-free rate | **Rho** — rate sensitivity of discounting & exposure |
| ∂xVA/∂*λ*  | Hazard rate | **CS01** — credit-spread sensitivity |
| ∂xVA/∂*LGD* | Loss given default | Recovery-rate sensitivity |
| ∂xVA/∂*s_f* | Funding spread | Funding-spread sensitivity |

In production these sensitivities drive:

- **Hedging** — delta-hedge CVA with CDS, FVA with funding
  instruments
- **P&L explain** — attribute daily P&L to each risk factor
- **Regulatory capital** — SA-CVA and BA-CVA under Basel III /
  FRTB require first-order sensitivities

---

## Tape-Based AD Architecture

The xVA computation involves ~100 elementary operations on 6 scalar
parameters — ideal for Eta's tape-based AD.  When `grad` creates
TapeRef variables and activates the tape, the VM's `+`, `-`, `*`,
`/`, `exp`, `log`, `sqrt` transparently record each operation
(~32 bytes per entry, zero closure allocations).

### Key Benefits

| Metric | Tape-based AD |
|--------|:------------:|
| Ops per xVA eval | ~100 |
| Memory per op | ~32 bytes |
| Closures allocated | 0 |
| VM dispatches per op | 1 |
| Backward pass | Single reverse sweep |

### Why AAD Matters Here

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
operation is transparently recorded by the tape, and the backward
pass propagates through all of them.

---

## Summary

| Component | Role |
|-----------|------|
| `discount-factor` | Risk-free discounting, DF = e^{−rt} |
| `survival-prob` / `default-prob` | Hazard-rate credit model |
| `marginal-pd` | Bucket-level default probability |
| `expected-exposure` | Simplified EE ≈ Nσ√t |
| `compute-cva` | CVA = LGD × Σ EE × DF × ΔPD |
| `compute-fva` | FVA = Σ EE × DF × s_f × Δt |
| `total-xva` | CVA + FVA with full gradient |
| `grad` | One backward pass → all 6 sensitivities |
| Tape-aware arithmetic | `+`/`-`/`*`/`/`, `exp`/`log`/`sqrt` — transparent recording |
