# SABR Volatility Surface with Native Dual AD

[← Back to README](../README.md) · [AAD Deep-Dive](aad.md) ·
[European Greeks](european.md) · [xVA Example](xva.md) ·
[Examples](examples.md)

---

## Overview

[`examples/sabr.eta`](../examples/sabr.eta) implements the **Hagan et al.
(2002)** SABR implied volatility approximation — a stochastic-volatility model. It
computes **all model sensitivities** using Eta's **native Dual VM type**.

**Key ideas demonstrated:**

- SABR Hagan implied vol formula (ATM + general-K branches)
- Vol surface generation across a strike × expiry grid
- First-order Greeks (∂σ/∂α, ∂σ/∂β, ∂σ/∂ρ, ∂σ/∂ν) in a **single backward pass**
- Second-order Hessian via **reverse-on-reverse** (true tape-on-tape).

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
Eta's bytecode VM has a **dedicated heap object kind** and **three
dedicated opcodes** for reverse-mode automatic differentiation.  The
arithmetic opcodes (`Add`, `Sub`, `Mul`, `Div`) and transcendental
builtins (`exp`, `log`, `sqrt`) are all **Dual-aware** — they
transparently detect Dual operands and build the backward computation
graph in C++ without any Scheme-level dispatch.

### What Is a Native Dual?

A **Dual** is a first-class heap object (like a cons cell, vector, or
closure) that pairs a forward value with a backward function:

```cpp
// eta/runtime/types/dual.h
struct Dual {
    LispVal primal {};      // The forward (numeric) value
    LispVal backprop {};    // A closure: adjoint → list of (index . contribution)
};
```

It lives in the VM's managed heap as `ObjectKind::Dual` — a dedicated
tag in the heap's object-kind enum alongside `Cons`, `Vector`,
`Closure`, etc.  Because the VM knows the exact memory layout, it can
read the `primal` and `backprop` fields with direct C++ pointer
dereferences — no Scheme-level `car`/`cdr` dispatches, no type-check
bytecodes, no closure calls.

### The Three Dual Opcodes

The bytecode instruction set includes three opcodes that expose the
native Dual type to Eta code:

| Opcode | Stack effect | Description |
|--------|-------------|-------------|
| `MakeDual` | `[primal backprop →  dual]` | Allocates a `Dual{primal, backprop}` on the managed heap |
| `DualVal` | `[dual → primal]` | Extracts `.primal` — passes through unchanged if the value is not a Dual |
| `DualBp` | `[dual → backprop]` | Extracts `.backprop` — returns a no-op closure `(λ(adj) '())` if not a Dual |

These compile from the Eta-level primitives `make-dual`, `dual-primal`,
and `dual-backprop`.  Together they let user code construct and
destructure Dual values at opcode speed.

### How the Arithmetic Opcodes Lift

The real power comes from the **existing** arithmetic opcodes.  When the
VM executes `Add`, `Sub`, `Mul`, or `Div`, it calls `do_binary_arithmetic()`
which checks whether either operand is a Dual:

```
┌──────────────────────────────────────────────────────────────┐
│  VM opcode: Mul                                              │
│                                                              │
│  1. Pop b, pop a                                             │
│  2. Check heap tag: is_dual(a) || is_dual(b)?    ← 1 branch  │
│  3. If Dual detected:                                        │
│       pa, pb = a.primal, b.primal  ← C++ field reads         │
│       ba, bb = a.backprop, b.backprop                        │
│       forward = pa * pb            ← recurse on primals      │
│       Build C++ backpropagator:                              │
│         λ(adj) → append(ba(adj*pb), bb(adj*pa))              │
│       Push Dual(forward, backprop) ← 1 heap allocation       │
│  4. If plain numbers:                                        │
│       Standard fixnum/flonum multiply                        │
└──────────────────────────────────────────────────────────────┘
```

The chain-rule logic for each operation is hard-coded in C++:

| Opcode | Forward | ∂z/∂a | ∂z/∂b |
|--------|---------|-------|-------|
| `Add` | `a + b` | `1` | `1` |
| `Sub` | `a − b` | `1` | `−1` |
| `Mul` | `a × b` | `b` | `a` |
| `Div` | `a / b` | `1/b` | `−a/b²` |

The backpropagator is allocated as a C++ `Primitive` (a heap-managed
callable that captures `pa`, `pb`, `ba`, `bb` by value).  When adjoint
arithmetic inside the backpropagator encounters further Duals — as it
does during reverse-on-reverse — the same `do_binary_arithmetic()` path
fires again, building a second-level computation graph automatically.

### Transcendental Functions

The builtins `exp`, `log`, and `sqrt` apply the same pattern:

| Builtin | Forward | Backprop: `adj →` |
|---------|---------|-------------------|
| `exp(x)` | `eˣ` | `bp(adj × eˣ)` |
| `log(x)` | `ln x` | `bp(adj / x)` |
| `sqrt(x)` | `√x` | `bp(adj / 2√x)` |

Each detects a Dual argument via the `ObjectKind::Dual` heap tag,
computes the forward value with `std::exp` / `std::log` / `std::sqrt`,
then allocates a Dual whose backpropagator captures the adjoint
arithmetic.  All chain-rule bookkeeping runs in C++ — the backward
closure calls `dual_binary_op()` for its `×` and `/`, so nested Duals
propagate correctly.

### Why This Matters for AAD

In a bytecode VM, every operation costs one **dispatch** — the VM reads
the opcode, jumps to the handler, and executes it.  Library-level AD
(representing duals as `cons` pairs) turns a single `*` into a cascade
of ~13 dispatches (type checks, field extracts, forward arithmetic,
closure construction).  Native Duals collapse that cascade into a
single dispatch whose handler does all the work in compiled C++.

**Result: ~1 VM dispatch per AD operation** (the rest is C++).

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

> [!TIP]
> ```etai``` can run the .eta or .etac files directly. Use ```etac -O``` to compile with optimizations for a 
> more realistic performance profile.

```console
C:\tmp\eta-v0.1.0-win-x64\examples>etai sabr.etac
C:\tmp\eta-v0.1.0-win-x64\examples>etac -O sabr.eta
compiled sabr.eta > sabr.etac (110 functions, 1 module(s))

C:\tmp\eta-v0.1.0-win-x64\examples>etai sabr.etac
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


-- Hessian (reverse-on-reverse, ATM, T=1Y) --

  4x4 Hessian of sigma_impl w.r.t. (alpha, beta, rho, nu)
  Using dedicated MakeDual / DualVal / DualBp opcodes.

    H[alpha,alpha]  = -0.412349
    H[rho,rho]      = -0.0080829
    H[nu,nu]        = 0.0305214
    H[rho,nu]       = 0.0152078
    H[nu,rho]       = 0.0152078

  Schwarz check (H[rho,nu] vs H[nu,rho]):
    Difference = 0
    (Should be ~0 by Schwarz's theorem)


-- Performance: Native Dual VM Instructions --

  The SABR Hagan formula involves ~50 elementary operations
  on 4 scalar parameters ÔÇö the ideal regime for native Duals.

  Library-level cons-pair AD (d+, d*, ...):
    Per op: 2x ensure-dual (if + pair? + cons) = 6 ops
            2x dual-val (car) + 2x dual-bp (cdr) = 4 ops
            1x forward arithmetic + 1x cons + 1x lambda = 3 ops
    Total: ~13 Scheme-level VM instructions per AD op
    SABR: ~50 ops x 13 = ~650 VM dispatches

  Native Dual VM instructions (nd+, nd*, ...):
    Per op: VM checks ObjectKind::Dual tag in C++ (~1 branch)
            Extracts primal/backprop at C++ level
            Builds result Dual with 1x Heap::allocate<Dual>
    Total: ~1 VM dispatch per AD op (rest is C++)
    SABR: ~50 ops x 1 = ~50 VM dispatches

  Speedup: ~13x fewer VM dispatches for first-order.
  For Hessian (reverse-on-reverse), the advantage compounds:
    the backward pass arithmetic also uses Dual-aware opcodes,
    so both tape levels benefit from native C++ lifting.

  Memory: native Dual = 16 bytes (primal + backprop pointer)
          cons-pair   = 16 bytes (car + cdr) + closure overhead
          libtorch scalar tensor = ~200 bytes metadata per op
```


