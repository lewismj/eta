# Adjoint Algorithmic Differentiation (AAD)

[← Back to README](../README.md) · [Examples](examples.md) ·
[Architecture](architecture.md) · [Modules & Stdlib](modules.md)

---

## Contents

- [Overview](#overview)
- [How Reverse-Mode AD Works](#how-reverse-mode-ad-works)
- [Tape-Based AD](#tape-based-ad)
  - [Tape API](#tape-api)
  - [Tape Usage Pattern](#tape-usage-pattern)
  - [The `grad` Helper](#the-grad-helper)
  - [Tape Internal Structure](#tape-internal-structure)
- [Summary](#summary)
- [Finance Examples](#finance-examples)

---

## Overview

[`examples/aad.eta`](../examples/aad.eta) implements **reverse-mode
automatic differentiation** (also called adjoint or backpropagation)
using a **tape-based (Wengert list)** approach built into the Eta VM.
It computes exact gradients of scalar functions with respect to any
number of input variables — the same mathematical machinery that
powers deep-learning frameworks.

Eta provides tape-based reverse-mode AD:

| Approach | Description |
|----------|-------------|
| **Tape (Wengert list)** | ~32 bytes/op, zero closure allocations, VM-native recording |

Standard arithmetic (`+`, `-`, `*`, `/`) and transcendentals (`sin`,
`cos`, `exp`, `log`, `sqrt`) are **automatically recorded** when a
`TapeRef` operand is detected — no macro rewriting or lifted operators
needed.

**Key ideas demonstrated:**

- VM-native tape recording of arithmetic and transcendentals
- Higher-order functions (`foldl`, `apply`)
- Vectors for gradient collection
- Zero-overhead: plain arithmetic becomes tape-aware transparently

```bash
etai examples/aad.eta
```

---

## How Reverse-Mode AD Works

Reverse-mode AD records a computation as a graph of elementary
operations on a **tape** (Wengert list).  A single backward sweep from
the output propagates adjoints all the way to the input variables,
yielding the full gradient in one pass regardless of the number of
inputs.

```
Forward pass                 Backward pass
────────────                 ─────────────
x ──┐                        ∂f/∂x ← adj_x
    ├── z = x·y ── f         1 (seed)
y ──┘                        ∂f/∂y ← adj_y
```

---

## Tape-Based AD

The tape (Wengert list) approach records all arithmetic into a flat
array of ~32-byte entries.  The VM intercepts `+`, `-`, `*`, `/`,
`sin`, `cos`, `exp`, `log`, and `sqrt` when any operand is a
**TapeRef** — a lightweight NaN-boxed index into the tape.  No
closures are allocated and no macro rewriting is required.

### Tape API

| Builtin | Arity | Description |
|---------|-------|-------------|
| `tape-new` | 0 | Create a fresh, empty tape |
| `tape-start!` | 1 | Activate a tape (enables recording) |
| `tape-stop!` | 0 | Deactivate the active tape |
| `tape-var` | 2 | Register an independent variable: `(tape-var tape value)` → TapeRef |
| `tape-backward!` | 2 | Run reverse sweep: `(tape-backward! tape output-ref)` |
| `tape-adjoint` | 2 | Read accumulated adjoint: `(tape-adjoint tape ref)` → number |
| `tape-primal` | 2 | Read forward value: `(tape-primal tape ref)` → number |
| `tape-ref?` | 1 | Predicate: is the value a TapeRef? |
| `tape-ref-index` | 1 | Extract the raw index from a TapeRef |
| `tape-size` | 1 | Number of entries on the tape |

### Tape Usage Pattern

```scheme
(define tape (tape-new))           ; 1. Create tape
(define x (tape-var tape 2.0))     ; 2. Register inputs
(define y (tape-var tape 3.0))
(tape-start! tape)                 ; 3. Activate recording
(define z (+ (* x y) (sin x)))    ; 4. Forward pass (auto-recorded)
(tape-stop!)                       ; 5. Stop recording
(tape-backward! tape z)            ; 6. Reverse sweep
(tape-adjoint tape x)              ; 7. Read gradients → 3 + cos(2)
(tape-adjoint tape y)              ;    → 2
```

### The `grad` Helper

The [`examples/aad.eta`](../examples/aad.eta) file provides a
convenient `grad` function that wraps the tape API:

```scheme
(defun grad (f vals)
  (let ((tape (tape-new))
        (n    (length vals)))
    (let ((vars (letrec ((mk (lambda (vs acc)
                               (if (null? vs) (reverse acc)
                                   (mk (cdr vs)
                                       (cons (tape-var tape (car vs)) acc))))))
                  (mk vals '()))))
      (tape-start! tape)
      (let ((output (apply f vars)))
        (tape-stop!)
        (tape-backward! tape output)
        (let ((grad-vec (make-vector n 0)))
          (letrec ((collect (lambda (vs i)
                              (if (null? vs) grad-vec
                                  (begin
                                    (vector-set! grad-vec i
                                      (tape-adjoint tape (car vs)))
                                    (collect (cdr vs) (+ i 1)))))))
            (collect vars 0))
          (list (tape-primal tape output) grad-vec))))))
```

Usage — gradients are computed with plain arithmetic:

```scheme
(grad (lambda (x y) (+ (* x y) (sin x))) '(2 3))
;; => (6.909.. #(2.583.. 2))
```

### Tape Internal Structure

Each tape entry is ~32 bytes:

```
┌────────┬──────┬───────┬─────────┬──────────┐
│ TapeOp │ left │ right │ primal  │ adjoint  │
│ (1B)   │(4B)  │ (4B)  │ (8B)    │ (8B)     │
└────────┴──────┴───────┴─────────┴──────────┘
```

Supported operations: `Const`, `Var`, `Add`, `Sub`, `Mul`, `Div`,
`Exp`, `Log`, `Sqrt`, `Sin`, `Cos`.

The backward pass sweeps entries in reverse order, applying the chain
rule at each node.  Adjoints accumulate additively — a variable used
in multiple sub-expressions receives the sum of all path contributions.

---

## Summary

| Component | Role |
|-----------|------|
| `tape-new` | Create a fresh, empty tape |
| `tape-start!` / `tape-stop!` | Activate / deactivate recording |
| `tape-var` | Register an independent variable on the tape |
| `tape-backward!` | Run the reverse sweep from an output node |
| `tape-adjoint` / `tape-primal` | Read accumulated adjoint / forward value |
| `grad` | Top-level driver: register inputs → forward → backward → collect |

---

## Finance Examples

All of these examples build on the tape-based AD described above —
plain arithmetic is transparently recorded, and gradients are obtained
in a single backward pass.

| Example | Description | Docs |
|---------|-------------|------|
| [`aad.eta`](../examples/aad.eta) | Core AD walkthrough — `grad` helper, multi-variable gradients | *(this page)* |
| [`xva.eta`](../examples/xva.eta) | CVA & FVA valuation adjustments with market-risk sensitivities | [xVA](xva.md) |
| [`european.eta`](../examples/european.eta) | Black-Scholes Greeks (1st & 2nd order), custom VJP, Schwarz check | [European Greeks](european.md) |
| [`sabr.eta`](../examples/sabr.eta) | SABR Hagan implied vol, strike × expiry vol surface Greeks | [SABR](sabr.md) |

```bash
etai examples/aad.eta
etai examples/xva.eta
etai examples/european.eta
etai examples/sabr.eta
```

