# CLP(B) — Boolean Constraint Logic Programming

[<- Back to README](../README.md) · [CLP](clp.md) · [Logic](logic.md)

---

## Overview

`std.clpb` is a propagation-based Boolean constraint solver layered on
the same VM substrate as `std.clp` (attributed variables, FIFO
propagation queue, unified trail).  Boolean variables carry a
`ZDomain {0, 1}`; every posting helper runs its native bounds /
support propagator once, then attaches a re-firing thunk under the
shared `'clp.prop` attribute key.  No new opcodes, no parallel
constraint store.

```scheme
(import std.clp)   ; required: provides domain primitives + queue
(import std.clpb)
```

---

## Domain

| Form | Meaning |
|------|---------|
| `(clp:boolean v)` | Declare `v ∈ {0, 1}` (alias for `(%clp-domain-z! v 0 1)`) |

`v` afterwards behaves as any other CLP integer variable: introspectable
via `clp:domain-values`, labellable via `clp:labeling`, etc.

---

## Constraints

All constraints are **reified**: every connective has the form
`z ≡ f(x, y)`, so `z` carries the truth value of the formula and can
itself participate in further constraints.

| Form | Semantics |
|------|-----------|
| `(clp:and z x y)` | `z ≡ x ∧ y` |
| `(clp:or  z x y)` | `z ≡ x ∨ y` |
| `(clp:xor z x y)` | `z ≡ x ⊕ y` |
| `(clp:imp z x y)` | `z ≡ (x → y)` |
| `(clp:eq  z x y)` | `z ≡ (x ⇔ y)` |
| `(clp:not z x)`   | `z ≡ ¬x` |
| `(clp:card xs k-lo k-hi)` | `k-lo ≤ Σ xs ≤ k-hi` |

Each posting runs the propagator once at post-time and re-fires
automatically on every later binding of any participating variable.

---

## Search & Queries

| Form | Behaviour |
|------|-----------|
| `(clp:labeling-b vars)` | Depth-first label `vars` to a satisfying assignment; **leaves `vars` bound** on success (same trail semantics as `clp:labeling`) |
| `(clp:sat? vars)` | `#t` iff some assignment satisfies the currently posted constraints; **always restores** `vars` on return via `trail-mark`/`unwind-trail` |
| `(clp:taut? vars)` | `#t` iff **no** assignment satisfies the constraints — i.e. `(not (clp:sat? vars))`.  For testing tautology of formula `F`, post `¬F` and call `clp:taut?` |

---

## Example

```scheme
(module xor-puzzle
  (import std.clp)
  (import std.clpb)
  (import std.io)
  (begin
    (define x (logic-var))
    (define y (logic-var))
    (define z (logic-var))
    (clp:boolean x) (clp:boolean y) (clp:boolean z)
    (clp:xor z x y)
    (unify z 1)                        ; force x ⊕ y = 1
    (clp:labeling-b (list x y))
    (println (list (deref-lvar x)
                   (deref-lvar y)))))  ; e.g. (0 1)
```

---

## Design Notes

- **No parallel solver state.** CLP(B) is a peer of CLP(FD): both
  share the same `'clp.prop` attribute, the same VM propagation queue,
  and the same trail-rollback semantics.
- **Model counting (`clp:sat-count`)** requires a BDD backend and is
  deferred behind an optional flag — not part of the propagation-only
  MVP.
- **Connective set** is intentionally minimal; richer formulae compose
  naturally (`(clp:and z (clp:or-aux ...) ...)`) at Eta level.

---

## Source Locations

| Component | File |
|-----------|------|
| `std.clpb` wrapper module | [`stdlib/std/clpb.eta`](../stdlib/std/clpb.eta) |
| Boolean propagators (`%clp-bool-*`) | [`eta/core/src/eta/runtime/core_primitives.h`](../eta/core/src/eta/runtime/core_primitives.h) |
| Tests | [`stdlib/tests/clpb_propagation.test.eta`](../stdlib/tests/clpb_propagation.test.eta) |

