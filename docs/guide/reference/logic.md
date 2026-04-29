# Logic Programming

[<- Back to README](../../../README.md) · [CLP(FD)](clp.md) ·
[CLP(R)](clpr.md) · [CLP(B)](clpb.md) · [Runtime & GC](runtime.md) ·
[Modules & Stdlib](modules.md)

---

## 1. Overview

Eta has **native structural unification** in the VM (`logic-var`,
`unify`, trail ops, `copy-term`) and builds higher-level logic
programming in stdlib modules:

- `std.logic` — goal combinators (`==`, `conde`, `fresh`, `findall`)
  and miniKanren-style search helpers.
- `std.db` — fact-table-backed relations, indexing, and tabling.

Logic code lives in-process with normal Eta code (closures, modules,
exceptions, GC) instead of requiring a separate Prolog runtime. The
trail is **unified** with the CLP machinery, so a single
`(unwind-trail m)` undoes bindings, attribute writes, integer-domain
narrowings, and CLP(R) tableau rows atomically.

If you are coming from Prolog, jump straight to §10 for a translation
table.

---

## 2. Core VM Primitives

### Special forms / opcodes

| Form | Opcode | Description |
|------|--------|-------------|
| `(logic-var)` | `MakeLogicVar` | Allocate a fresh, unbound logic variable |
| `(unify a b)` | `Unify` | Structural unification; returns `#t`/`#f`, trails any bindings |
| `(deref-lvar x)` | `DerefLogicVar` | Walk the logic-var chain to the bound value (or self if unbound) |
| `(trail-mark)` | `TrailMark` | Snapshot the current trail depth as a fixnum |
| `(unwind-trail mark)` | `UnwindTrail` | Roll back every trail entry above `mark` |
| `(copy-term t)` | `CopyTerm` | Deep copy with fresh unbound vars; sharing within `t` is preserved |

### Runtime builtins

| Form | Description |
|------|-------------|
| `(logic-var? x)` | `#t` iff `x` is currently unbound |
| `(ground? t)` | `#t` iff `t` (recursively) contains no unbound logic vars |
| `(logic-var/named 'name)` | Debug-labelled logic var |
| `(var-name v)` | Returns the label string or `#f` |
| `(set-occurs-check! mode)` / `(occurs-check-mode)` | `'always`, `'never`, `'error` |
| `(term 'f a1 ... aN)` | Construct a compound term |
| `(compound? t)`, `(functor t)`, `(arity t)`, `(arg i t)` | Compound-term introspection |

---

## 3. The Unification Algorithm

### 3.1 Robinson-style structural unification

`VM::unify(a, b)` is a textbook Robinson algorithm with path-compressed
dereferencing and an optional occurs-check:

```
unify(a, b):
    a = walk(a)              ; follow var chain to root
    b = walk(b)
    if a == b                          -> succeed
    if logic-var?(a)                   -> bind(a, b); succeed
    if logic-var?(b)                   -> bind(b, a); succeed
    if both atoms (eq?-compatible)     -> succeed
    if both pairs:
        unify(car(a), car(b)) and unify(cdr(a), cdr(b))
    if both vectors of same length:
        for i in 0..n-1: unify(a[i], b[i])
    if both compound, same functor/arity:
        for i in 0..arity-1: unify(arg(i,a), arg(i,b))
    otherwise                          -> fail (no trail change)
```

`bind(v, t)`:

1. *Optional* occurs-check (controlled by `set-occurs-check!`)
   walks `t` to make sure `v` doesn't appear inside it.
2. Push a `Bind` entry on the trail recording `v`'s previous slot.
3. Write `t` into `v`'s binding slot.
4. **Domain check.** If `v` carries a CLP `Domain` attribute,
   verify `t` ∈ domain; if not, `bind` fails (and only the
   trail entry is rolled back — the surrounding `unify` returns `#f`).
5. **Attribute hooks.** Any `register-attr-hook!` callback registered
   on a key present on `v` is invoked synchronously; any
   `register-prop-attr!` thunks are pushed onto the FIFO propagation
   queue for outer-`unify` drain.

### 3.2 Worked example — pair unification

```scheme
(define a (logic-var))
(define b (logic-var))
(unify (list 'f a 2) (list 'f 1 b))
; ⇒ #t
(deref-lvar a)  ; ⇒ 1
(deref-lvar b)  ; ⇒ 2
```

The recursion descends both lists in lock-step, hits the atom
equality `f = f`, then binds `a ↦ 1` and `2 ↦ b ⇒ b ↦ 2`. Two
trail entries are pushed; if any later step fails, both bindings
roll back together.

### 3.3 Variable chains

Two unbound vars can be unified before either is ground. The first
becomes a *forwarding pointer* to the second; subsequent
`deref-lvar` calls walk the chain (and *path-compress* it on the
way out, so chains stay shallow):

```scheme
(define v1 (logic-var))
(define v2 (logic-var))
(unify v1 v2)            ; v1 → v2 (no value yet)
(unify v2 99)            ; v2 ↦ 99
(deref-lvar v1)          ; ⇒ 99 (walks v1 → v2 → 99)
```

### 3.4 Occurs-check

Unifying `z` with `(cons z '())` would create the infinite term
`(z z z ...)`. By default Eta runs the occurs-check and rejects:

```scheme
(define z (logic-var))
(unify z (cons z '()))   ; ⇒ #f, z stays unbound
```

Switch off if you know your terms are finite and need raw speed:

```scheme
(set-occurs-check! 'never)   ; or 'error to raise instead of failing
```

---

## 4. The Trail

### 4.1 Unified trail entry kinds

The trail is one stream of `TrailEntry` values; all rollback-able
state — logic, attributes, CLP integer/finite, CLP(R) — lives in the
same place:

| Kind | What it restores |
|------|------------------|
| `Bind` | A logic-var binding |
| `Attr` | An attributed-variable slot |
| `Domain` | A CLP integer / FD domain write or erase |
| `RealStore` | The CLP(R) row-log size at this mark |
| `SimplexBound` | A cached simplex bound on one variable |

`(trail-mark)` returns `trail_stack_.size()` as a fixnum.
`(unwind-trail m)` calls `entry.undo()` from the top down until the
stack reaches size `m`. This is `O(k)` in the number of trailed
operations between the mark and now.

### 4.2 Backtracking pattern

Every search combinator in the language is built on this two-line
idiom:

```scheme
(let ((m (trail-mark)))
  (let ((ok (try-branch)))
    (unwind-trail m)
    ok))
```

From [`examples/unification.eta`](../../../examples/unification.eta) §3:

```scheme
(define r    (logic-var))
(define mark (trail-mark))
(unify r 100)
(deref-lvar r)            ; ⇒ 100
(unwind-trail mark)
(logic-var? (deref-lvar r))  ; ⇒ #t — back to unbound
```

Multiple bindings are *atomically* undone by a single `unwind-trail`:

```scheme
(define p (logic-var)) (define q (logic-var))
(define m (trail-mark))
(unify p 'alpha)  (unify q 'beta)
(unwind-trail m)          ; both p and q are unbound again
```

### 4.3 GC and the trail

The garbage collector treats every trail entry as a root, so any
LispVal still reachable through `Bind` / `Attr` / `Domain` payloads
is kept live. CLP(R) participating vars and cached simplex-bound
vars are scanned by the same marker. Nothing about logic state can
be silently collected mid-search.

---

## 5. Attributed Variables

Attributed vars are the substrate underneath `freeze`, `dif`,
`std.clp`, and `std.clpr`.

| Builtin | Purpose |
|---------|---------|
| `(put-attr v 'k val)` | Attach value under key |
| `(get-attr v 'k)` | Fetch (or `#f`) |
| `(del-attr v 'k)` | Remove |
| `(attr-var? v)` | `#t` iff `v` carries any attribute |
| `(register-attr-hook! 'k handler)` | Sync hook called inside `unify` when an attributed var on key `k` is bound |
| `(register-prop-attr! 'k)` | Mark `k` as a *propagation* key — its values are lists of zero-arg thunks queued for FIFO drain |

Two execution styles coexist:

- **Sync attr hooks** — invoked directly during `unify`. Used by
  `dif` (must reject the bind immediately if a forbidden equality
  is closed).
- **Async propagation attrs** — values are lists of propagator
  thunks; the VM queues them when the carrier var is bound and
  drains the FIFO at the outer `unify` boundary. Used by all CLP
  propagators so that one bind can reasonably wake a chain of
  others without blowing the stack.

Diagnostic: `(%clp-prop-queue-size)` reports the current FIFO depth.

---

## 6. `std.logic` Library

```scheme
(import std.logic)
```

### 6.1 Classic helpers

- `(== a b)` — callable form of `unify`; returns `#t`/`#f`.
- `(copy-term* t)` — same as `copy-term` but with a per-call
  variable map for cleaner sharing.
- `(naf goal)` — *negation as failure*: `#t` iff `goal` (a thunk)
  fails, with the trail restored either way.
- `(succeeds? goal)` — non-destructive probe: `#t` iff `goal`
  succeeds; trail is restored on the way out.
- `(findall template-thunk branches)` — try each branch (a
  zero-arg thunk); on success, run `template-thunk` and collect its
  return value; rewind the trail after every branch. Returns the
  collected list.
- `(run1 template-thunk branches)` — first solution only.
- `(membero x lst)` — non-deterministic membership; returns one
  branch per element of `lst`.

### 6.2 miniKanren-style combinators

Goalsets are lists of zero-arg branch thunks, so they compose
directly with `findall` and `run1`.

| Constructor | Meaning |
|---|---|
| `(goal thunk)` | Wrap a procedure as a one-branch goalset |
| `(==o a b)` | Unify-as-a-goal; one branch that succeeds iff `(== a b)` |
| `(succeedo)` / `(failo)` | Trivial goals |
| `(fresh (v ...) body ...)` | Allocate fresh logic vars in scope of `body` |
| `(fresh-vars n)` | Returns a list of `n` fresh vars |
| `(disj g ...)` | Disjunction — concatenate branch lists |
| `(conj g ...)` | Conjunction — Cartesian-product through trail-marks |
| `(conde clause ...)` | miniKanren `conde` (each clause is a goalset list) |
| `(conda ...)` / `(condu ...)` | Committed-choice variants of `conde` |
| `(onceo g)` | Take just the first branch |

| Enumeration | Meaning |
|---|---|
| `(run* template g ...)` | All solutions |
| `(run-n n template g ...)` | First `n` solutions |

### 6.3 Logic-scoped exceptions

- `(logic-throw tag payload)`
- `(logic-catch tag handler body)`

These unwind logic trail state **before** the handler runs, so
failed/aborted branches leak no bindings.

---

## 7. Worked Example — Pattern Matching via Unification

From [`examples/unification.eta`](../../../examples/unification.eta) §7,
a tiny tagged-message dispatcher built only from `unify` and
`trail-mark`:

```scheme
(defun dispatch (msg)
  ;; Try (add x y)
  (let* ((x (logic-var)) (y (logic-var))
         (m (trail-mark))
         (ok (unify msg (list 'add x y))))
    (if ok
        (begin (println (+ (deref-lvar x) (deref-lvar y)))
               (unwind-trail m))
        (begin (unwind-trail m)
          ;; Try (greet name)
          (let* ((name (logic-var))
                 (m2 (trail-mark))
                 (ok2 (unify msg (list 'greet name))))
            (if ok2
                (begin (print "Hello, ")
                       (println (deref-lvar name))
                       (unwind-trail m2))
                (begin (unwind-trail m2)
                       (println "unknown message"))))))))

(dispatch '(add 3 4))      ; ⇒ 7
(dispatch '(greet world))  ; ⇒ Hello, world
(dispatch '(unknown))      ; ⇒ unknown message
```

Each branch saves a trail-mark, attempts to unify the incoming term
with a pattern that contains fresh vars, and rolls back on failure.
This is *exactly* what `match` macros do — Eta gives you the
substrate directly.

---

## 8. Worked Example — Relational Database

From [`examples/logic.eta`](../../../examples/logic.eta), the canonical
parent/grandparent example.

### 8.1 Facts and the `parento` relation

```scheme
(define parent-db
  '((tom bob) (tom liz) (bob ann) (bob pat) (pat jim)))

;; (parento p c) returns one branch per fact; each branch unifies
;; the caller-supplied logic vars p, c against the fact's fields.
(defun parento (p c)
  (map* (lambda (fact)
          (lambda ()
            (and (== p (car fact))
                 (== c (cadr fact)))))
        parent-db))
```

The crucial property: `parento` is **directionless**. Either
argument can be a fresh logic var (acts as an output) or a concrete
value (acts as a filter):

```scheme
(let ((cv (logic-var)))
  (findall (lambda () (deref-lvar cv))
           (parento 'tom cv)))
;; ⇒ (bob liz)         — children of tom

(let ((pv (logic-var)))
  (findall (lambda () (deref-lvar pv))
           (parento pv 'ann)))
;; ⇒ (bob)             — parents of ann

(let ((pv (logic-var)) (cv (logic-var)))
  (findall (lambda () (cons (deref-lvar pv) (deref-lvar cv)))
           (parento pv cv)))
;; ⇒ ((tom . bob) (tom . liz) (bob . ann) (bob . pat) (pat . jim))
```

`findall` runs every branch, captures the template, and rewinds the
trail between attempts — so `pv` and `cv` start fresh each time.

### 8.2 Composing relations — `grandparento`

The Prolog rule is:

```
grandparent(GP, GC) :- parent(GP, Mid), parent(Mid, GC).
```

In Eta, two-step join via an intermediate `findall`:

```scheme
(defun grandparento (gp gc)
  (let* ((mid  (logic-var))
         (mids (findall (lambda () (deref-lvar mid))
                        (parento gp mid))))      ; step 1: (gp,mid) pairs
    (flatten
      (map* (lambda (mid-val)
              (parento mid-val gc))              ; step 2: (mid,gc) pairs
            mids))))

(let ((gp (logic-var)) (gc (logic-var)))
  (findall (lambda () (cons (deref-lvar gp) (deref-lvar gc)))
           (grandparento gp gc)))
;; ⇒ ((tom . ann) (tom . pat) (bob . jim))
```

The pattern scales unchanged to ancestor of any depth. For larger
fact tables, use `std.db` (next section) which adds first-argument
indexing and variant tabling.

---

## 9. `std.db` — Fact-Table Relations

```scheme
(import std.logic std.db)
```

`std.db` layers relational operations on top of the in-memory
`FactTable` runtime type and the unification primitives.

### 9.1 Public API

| Form | Meaning |
|---|---|
| `(defrel '(name arg ...))` | Add a fact (or rule head) |
| `(retract '(name arg ...))` | Remove the first matching fact |
| `(retract-all 'name)` | Drop the whole relation |
| `(call-rel 'name arg ...)` | Returns a goal-branch list compatible with `findall`/`run1` |
| `(call-rel? 'name arg ...)` | Boolean probe |
| `(index-rel! 'name n)` | Build a hash index on argument `n` |
| `(tabled 'name)` | Enable variant-key memoization |

### 9.2 How it works

- Each `(name, arity)` maps to a `FactTable`. Ground rows go in
  directly; non-ground rows carry per-row metadata.
- Queries return a list of branch thunks compatible with
  `findall`/`run1`; each thunk does the trail-mark-unify-rewind dance.
- **First-argument indexing** is on by default — querying with a
  ground first argument hits a hash bucket in `O(1)` instead of a
  linear scan.
- `(tabled 'name)` keys the result of every call by
  `term-variant-hash` so repeated calls with the same shape return
  the cached branch list. This is the same idea as Prolog tabling
  but cache-based, not the WAM SLG engine.

### 9.3 Example

```scheme
(import std.logic std.db)

(defrel '(parent tom bob))
(defrel '(parent bob ann))
(index-rel! 'parent 0)         ; hash on the parent slot

(let ((x (logic-var)))
  (findall (lambda () (deref-lvar x))
           (call-rel 'parent 'tom x)))
;; ⇒ (bob)
```

---

## 10. Relation to Prolog

| Prolog | Eta |
|--------|-----|
| `X = Y` | `(unify x y)` or `(== x y)` |
| `findall/3` | `(findall thunk branches)` |
| `member/2` | `(membero x lst)` |
| `copy_term/2` | `(copy-term t)` / `copy-term*` |
| Attributed vars | `put-attr` / `get-attr` + hooks |
| `assert` / `retract` | `std.db` (`defrel`, `retract`, `retract-all`) |
| Tabling | `std.db` `tabled` (variant cache, not full WAM/SLG) |
| Cut `!` | Not a core builtin; use `onceo`, `conda`, `condu` |
| `\+ G` (NAF) | `(naf G)` |
| `bagof/3` | use `findall`; group at Eta level |
| First-argument indexing | `index-rel!` |

---

## 11. Current Limitations

- **No dedicated WAM engine.** The bytecode VM evaluates branch
  thunks directly; WAM opcodes are reserved but unused.
- **No core cut.** Search-strategy control is library-level; the
  miniKanren-style `onceo`/`conda`/`condu` cover most cut idioms.
- **Tabling is cache-based.** Not full SLG resolution — recursive
  table calls with growing answer sets won't auto-converge; use
  bottom-up evaluation patterns for fixed-point relations.

---

## 12. Source Locations

| Component | File |
|-----------|------|
| Logic var type | [`types/logic_var.h`](../../../eta/core/src/eta/runtime/types/logic_var.h) |
| Trail entry kinds & VM state | [`vm/vm.h`](../../../eta/core/src/eta/runtime/vm/vm.h) |
| Unification, rollback, propagation queue | [`vm/vm.cpp`](../../../eta/core/src/eta/runtime/vm/vm.cpp) |
| Core logic / attr builtins | [`core_primitives.h`](../../../eta/core/src/eta/runtime/core_primitives.h) |
| Logic special-form handling | [`expander.cpp`](../../../eta/core/src/eta/reader/expander.cpp), [`core_ir.h`](../../../eta/core/src/eta/semantics/core_ir.h), [`emitter.cpp`](../../../eta/core/src/eta/semantics/emitter.cpp) |
| `std.logic` | [`stdlib/std/logic.eta`](../../../stdlib/std/logic.eta) |
| `std.db` | [`stdlib/std/db.eta`](../../../stdlib/std/db.eta) |
| Fact-table runtime type | [`types/fact_table.h`](../../../eta/core/src/eta/runtime/types/fact_table.h) |
| Logic examples | [`examples/unification.eta`](../../../examples/unification.eta), [`examples/logic.eta`](../../../examples/logic.eta) |

