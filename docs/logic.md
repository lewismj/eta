# Logic Programming

[<- Back to README](../README.md) - [CLP](clp.md) - [Runtime & GC](runtime.md) -
[Modules & Stdlib](modules.md)

---

## Overview

Eta has native structural unification in the VM (`logic-var`, `unify`, trail
ops, `copy-term`) and builds higher-level logic programming in stdlib modules:

- `std.logic` for goal combinators and search helpers
- `std.db` for fact-table-backed relations, indexing, and tabling

This keeps logic code in-process with normal Eta code (closures, modules,
exceptions, GC) rather than requiring a separate Prolog runtime.

---

## Core VM Primitives

These are the core logic forms/builtins available to user code.

### Special Forms / Opcodes

| Form | Opcode | Description |
|------|--------|-------------|
| `(logic-var)` | `MakeLogicVar` | Fresh unbound logic variable |
| `(unify a b)` | `Unify` | Structural unification; returns `#t`/`#f` |
| `(deref-lvar x)` | `DerefLogicVar` | Follow logic-var chain |
| `(trail-mark)` | `TrailMark` | Push current trail depth mark |
| `(unwind-trail mark)` | `UnwindTrail` | Roll back to mark |
| `(copy-term t)` | `CopyTerm` | Deep copy with fresh unbound vars, sharing preserved |

### Runtime Builtins

| Form | Description |
|------|-------------|
| `(ground? t)` | `#t` if no unbound logic vars remain in `t` |
| `(logic-var/named 'name)` | Debug-labeled logic var |
| `(var-name v)` | Label string or `#f` |
| `(set-occurs-check! mode)` / `(occurs-check-mode)` | Occurs-check policy (`'always`, `'never`, `'error`) |
| `(term 'f a1 ... aN)` | Construct compound term |
| `(compound? t)`, `(functor t)`, `(arity t)`, `(arg i t)` | Compound-term introspection |

---

## Unification and Trail Model

### Structural Unification

`VM::unify` uses Robinson-style unification with dereferencing and occurs-check.
It handles:

- atoms/numbers/symbol equality
- logic-variable binding (trailed)
- cons recursion
- vector recursion
- compound term recursion

### Unified Trail Entries

All rollback-able logic/CLP state uses one trail stream:

- `Bind` - logic-var binding
- `Attr` - attributed-variable slot change
- `Domain` - CLP domain write/erase
- `RealStore` - CLP(R) posted-row log size snapshot
- `SimplexBound` - CLP(R) simplex bound snapshot

### Trail Marks

`(trail-mark)` now stores a single fixnum: `trail_stack_.size()`.

`(unwind-trail m)` replays rollback over that unified stream. There is no
separate packed mark format anymore.

### GC Roots

GC marks logic state reachable only through logic machinery, including:

- trail entries
- CLP(R) participating vars and cached simplex-bound vars
- registered attr-unify hooks
- pending propagation thunks

---

## Attributed Variables and Hooks

Builtins:

- `put-attr`, `get-attr`, `del-attr`, `attr-var?`
- `register-attr-hook!`
- `register-prop-attr!`

Two execution styles coexist:

- Sync attr hooks (`register-attr-hook!`): called directly during unify.
- Async propagation attrs (`register-prop-attr!`): attribute values are lists
  of thunks queued for outer-unify FIFO drain.

This is the substrate used by `freeze`, `dif`, `std.clp`, and `std.clpr`.

---

## `std.logic` Library

```scheme
(import std.logic)
```

### Classic Helpers

- `==` (callable unification)
- `copy-term*`
- `naf`, `succeeds?`
- `findall`, `run1`
- `membero`

### Goalset / miniKanren-Style Combinators

`std.logic` now also exports composable goalset helpers:

- Constructors: `goal`, `==o`, `succeedo`, `failo`, `fresh`, `fresh-vars`
- Composition: `disj`, `conj`
- Committed choice: `conde`, `conda`, `condu`, `onceo`
- Enumeration: `run*`, `run-n`

A goalset is a list of zero-arg branch thunks, so these compose directly with
`findall`/`run1`.

### Logic-Scoped Exceptions

- `logic-throw`
- `logic-catch`

These helpers unwind logic trail state before handler execution, so failed or
aborted branches do not leak bindings.

---

## `std.db`: Fact-Table Relations

```scheme
(import std.logic std.db)
```

`std.db` layers relational operations on top of `FactTable` storage and logic
unification.

### Public API

- `defrel`, `defrel-clause` (clause-head convenience wrapper)
- `assert-fact!`, `retract-fact!`, `retract-all` (deprecated aliases: `assert`, `retract`)
- `call-rel`, `call-rel?`
- `index-rel!`
- `tabled`, `tabled-clause` (clause-head convenience wrapper)

### How It Works

- Each relation `(name, arity)` maps to a fact table.
- Ground facts are stored directly.
- Non-ground heads / rule rows are stored with per-row metadata.
- Queries return goal-branch thunks compatible with `findall`/`run1`.
- First-argument indexing is installed by default; more indexes via `index-rel!`.
- `tabled` enables variant-key caching (`term-variant-hash`) for repeated calls.

### Example

```scheme
(import std.logic std.db)

(defrel '(parent tom bob))
(defrel '(parent bob ann))

(let ((x (logic-var)))
  (findall (lambda () (deref-lvar x))
           (call-rel 'parent 'tom x)))
; => (bob)
```

---

## Relation to Prolog

| Prolog | Eta |
|--------|-----|
| `X = Y` | `(unify x y)` or `(== x y)` |
| `findall/3` | `(findall thunk branches)` |
| `member/2` | `(membero x lst)` |
| `copy_term/2` | `(copy-term t)` / `copy-term*` |
| Attributed vars | `put-attr`/`get-attr` + hooks |
| `assert` / `retract` | `std.db` (`assert-fact!`, `retract-fact!`, `retract-all`) |
| Tabling | `std.db` `tabled` (variant cache, not full WAM/SLG engine) |
| Cut `!` | Not a core builtin; use committed-choice combinators (`onceo`, `conda`, `condu`) where appropriate |

---

## Current Limitations

- No dedicated WAM execution engine yet (WAM opcodes remain reserved).
- `std.db` tabling is pragmatic cache-based tabling, not full Prolog tabling.
- Search strategy control is library-level; there is no built-in Prolog cut op.

---

## Source Locations

| Component | File |
|-----------|------|
| Logic var type | [`types/logic_var.h`](../eta/core/src/eta/runtime/types/logic_var.h) |
| Trail entry kinds / VM state | [`vm/vm.h`](../eta/core/src/eta/runtime/vm/vm.h) |
| Unification / rollback / propagation queue | [`vm/vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp) |
| Core logic/attr builtins | [`core_primitives.h`](../eta/core/src/eta/runtime/core_primitives.h) |
| Logic special-form handling | [`expander.cpp`](../eta/core/src/eta/reader/expander.cpp), [`core_ir.h`](../eta/core/src/eta/semantics/core_ir.h), [`emitter.cpp`](../eta/core/src/eta/semantics/emitter.cpp) |
| `std.logic` | [`stdlib/std/logic.eta`](../stdlib/std/logic.eta) |
| `std.db` | [`stdlib/std/db.eta`](../stdlib/std/db.eta) |
| Fact-table runtime type | [`types/fact_table.h`](../eta/core/src/eta/runtime/types/fact_table.h) |
| Logic examples | [`examples/unification.eta`](../examples/unification.eta), [`examples/logic.eta`](../examples/logic.eta) |
