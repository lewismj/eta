# Relations & Tabling â€” `std.db`

[<- Back to README](../README.md) Â· [Logic](logic.md) Â·
[Fact Tables](fact-table.md) Â· [CLP](clp.md)

---

## Overview

`std.db` is a thin relation layer over [`std.fact_table`](fact-table.md)
and [`std.logic`](logic.md).  It gives Prolog-style **assert-fact! / retract-fact! /
call** semantics with first-argument indexing and optional **SLG-lite
tabling** (memoisation with cycle detection) â€” without leaving Eta or
introducing a separate database engine.

```scheme
(import std.db)        ; brings std.fact_table + std.logic with it
```

A *relation* is identified by a `(name . arity)` pair and stored in a
dedicated `FactTable` whose first column is hash-indexed by default.

---

## Defining Relations

| Form | Effect |
|------|--------|
| `(defrel '(name a1 ... aN))` | Insert a ground fact (or pattern row containing `?vars`) |
| `(defrel '(name a1 ... aN) rule-proc)` | Insert a rule row whose body is the procedure `rule-proc` (called with the actual call args) |
| `(defrel-clause '(name a1 ... aN))` | Convenience wrapper for static fact declarations |
| `(defrel-clause '(name a1 ... aN) rule-proc)` | Convenience wrapper for static rule declarations |
| `(assert-fact! '(name ...))` | Same as `defrel` for facts; canonical runtime mutation name |
| `(assert '(name ...))` | Deprecated alias of `assert-fact!` |
| `(retract-fact! '(name pattern...))` | Remove the **first** row matching `pattern` |
| `(retract '(name pattern...))` | Deprecated alias of `retract-fact!` |
| `(retract-all '(name pattern...))` | Remove **every** row matching `pattern` |
| `(index-rel! 'name col-spec)` | Build/rebuild a column index for every arity registered under `name` |

**Variable convention.** Symbols starting with `?` (e.g. `?x`, `?y`)
inside a `defrel` head act as pattern variables; repeated `?vars` must
unify consistently across the head.

```scheme
(defrel '(parent 'tom 'bob))
(defrel '(parent 'bob 'liz))
(defrel '(grandparent ?g ?c)
  (lambda (g c)
    (let ((p (logic-var)))
      (and (not (null? (call-rel 'parent g p)))
           (not (null? (call-rel 'parent p c)))))))
```

---

## Calling Relations

| Form | Returns |
|------|---------|
| `(call-rel 'name a1 ... aN)` | A *goalset*: a list of zero-arg branch thunks suitable for `findall`, `run1`, `run*` |
| `(call-rel? 'name a1 ... aN)` | `#t` iff at least one branch succeeds (convenience predicate) |

```scheme
(import std.logic)

(findall (lambda () #t)
         (call-rel 'parent 'tom (logic-var)))
;; => list of branch results, one per matching row
```

Goalsets compose naturally with the rest of `std.logic`.

---

## Tabling (SLG-lite)

| Form | Effect |
|------|--------|
| `(tabled 'name arity)` | Mark relation `(name . arity)` as tabled.  Subsequent `call-rel` invocations use a **variant-keyed answer cache** with cycle-aware fixpoint iteration |
| `(tabled-clause '(name a1 ... aN))` | Convenience wrapper that infers arity from the clause head |

Semantics:

- Cache key uses `term-variant-hash` so it is **stable under
  alpha-renaming** of logic variables.
- During table population, recursive consumers see answers accumulated
  so far (`'working` status) â€” the canonical SLG pattern that lets
  left-recursive rules terminate.
- On any DB mutation (`assert-fact!`, `retract-fact!`, `retract-all`, `defrel`,
  `tabled`) the cache is conservatively flushed.

```scheme
(tabled 'reaches 2)
(defrel '(reaches ?x ?x))
(defrel '(reaches ?x ?z)
  (lambda (x z)
    (let ((y (logic-var)))
      (and (call-rel? 'edge x y) (call-rel? 'reaches y z)))))
```

Without `tabled`, the same definition would loop on cyclic graphs.

Use the procedural forms (`defrel`, `tabled`) when relation names or heads are
computed at runtime (for example, `apply`-based registration).

---

## Trail & Backtracking

`call-rel` branch thunks are ordinary logic goals: they bind logic
vars via `unify`, all writes are trailed, and `unwind-trail` after
each failed branch restores the world exactly.  `defrel` /
`assert-fact!` mutations are **not** trailed â€” they are global side effects
on the relation table, mirroring Prolog's `assertz`.

---

## Source Locations

| Component | File |
|-----------|------|
| `std.db` module | [`stdlib/std/db.eta`](../stdlib/std/db.eta) |
| FactTable backend | [`stdlib/std/fact_table.eta`](../stdlib/std/fact_table.eta), [`eta/core/src/eta/runtime/types/fact_table.h`](../eta/core/src/eta/runtime/types/fact_table.h) |
| Logic helpers | [`stdlib/std/logic.eta`](../stdlib/std/logic.eta) |
| Tests | [`stdlib/tests/db.test.eta`](../stdlib/tests/db.test.eta) |

