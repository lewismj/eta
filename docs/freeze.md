# Attributed-Variable Combinators (`freeze`, `dif`)

[<- Back to README](../README.md) · [Logic](logic.md) · [CLP](clp.md) ·
[Finalizers & Guardians](finalizers.md)

---

## Overview

`std.freeze` exposes two high-level combinators built on Eta's
attributed-variable substrate:

| Form | Purpose |
|------|---------|
| `(freeze v goal-thunk)` | Suspend `goal-thunk` until `v` becomes bound |
| `(dif x y)` | Structural disequality — succeeds iff `x` and `y` cannot become equal without further bindings |

Both are pure stdlib: they use the public attribute primitives
(`put-attr`, `get-attr`, `register-attr-hook!`, `attr-var?`) and the
unified VM trail.  No new opcodes.

```scheme
(import std.freeze)
```

---

## Attribute Primitives Recap

| Form | Description |
|------|-------------|
| `(put-attr v 'mod val)` | Attach attribute `mod` with value `val` to logic-var `v` (trailed) |
| `(get-attr v 'mod)` | Read attribute `mod`, or `#f` |
| `(del-attr v 'mod)` | Remove attribute `mod` (trailed) |
| `(attr-var? v)` | `#t` iff `v` has at least one attribute |
| `(register-attr-hook! 'mod hook)` | Register `(hook var bound-val attr-val)` — fires when `var` is bound; returning `#f` fails the unification and triggers rollback |

Hooks fire **inside** unification.  A hook that needs to post further
constraints typically returns `#t` and queues work on the
`'clp.prop` async queue (see [`clp.md`](clp.md)).

---

## `freeze`

```scheme
(freeze v goal-thunk) -> #t
```

- If `v` is already ground, runs `goal-thunk` immediately.
- Otherwise appends `goal-thunk` to a pending list under the `'freeze`
  attribute on `v`.  When `v` is later bound, the registered hook
  drains the list **once**, running each thunk in order.  If any thunk
  returns `#f` (or `'()`), the binding fails and the trail rolls back.

```scheme
(define x (logic-var))
(freeze x (lambda () (println (list 'x 'is (deref-lvar x))) #t))
;; ... later ...
(unify x 42)   ; prints "x is 42" as a side effect of unify
```

---

## `dif`

```scheme
(dif x y) -> #t | #f
```

Maintains, per participating variable, a list of *witness* terms that
must never become structurally equal.  On every binding the `'dif`
hook re-probes each pending witness:

- if the pair is now equal → constraint violated → unify fails
- if the pair can never unify → constraint trivially satisfied → drop
- otherwise → keep the witness

Probing uses a `trail-mark` / `unify` / `unwind-trail` sandbox so it
never leaves bindings behind.

```scheme
(define x (logic-var))
(define y (logic-var))
(dif x y)
(unify x 1) (unify y 1)  ; => #f, dif rolled back
```

`dif` composes with `findall` / `run*` / `run-n` from
[`std.logic`](logic.md): on backtrack the trail restores witnesses
exactly.

---

## Composition

Both combinators share the same trail-and-attribute discipline as
CLP(R) and CLP(FD), so they freely combine:

```scheme
(import std.clp)
(import std.freeze)

(define x (logic-var))
(clp:domain x 0 9)
(freeze x (lambda () (println (list 'bound (deref-lvar x))) #t))
(clp:labeling (list x))
;; freeze hook fires once per labelled choice, with full backtrack support.
```

---

## Source Locations

| Component | File |
|-----------|------|
| `std.freeze` module | [`stdlib/std/freeze.eta`](../stdlib/std/freeze.eta) |
| Attribute substrate | [`eta/core/src/eta/runtime/vm/vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp) |
| Tests | [`stdlib/tests/attrvar_freeze_dif_trail.test.eta`](../stdlib/tests/attrvar_freeze_dif_trail.test.eta) |

