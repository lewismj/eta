# CLP(FD) — Constraint Logic Programming over Integers

[<- Back to README](../../../README.md) · [Logic](logic.md) ·
[CLP(R)](clpr.md) · [CLP(B)](clpb.md) · [Runtime & GC](runtime.md) ·
[Architecture](../../architecture.md)

---

## 1. Overview

Eta's CLP stack is split across three peer modules; this page covers
the integer / finite-domain side. CLP(R) and CLP(B) have their own
reference pages.

| Module | Domain | Page |
|--------|--------|------|
| `std.clp` | `clp(Z)` and `clp(FD)` | this page |
| `std.clpr` | `clp(R)` (real intervals + simplex / QP) | [`clpr.md`](clpr.md) |
| `std.clpb` | `clp(B)` (`{0,1}` reified Boolean) | [`clpb.md`](clpb.md) |

All three sit on the same VM unification + trail substrate. **No new
opcodes** are required for CLP itself — the functionality is provided
by native `%clp-*` builtins plus Eta-level wrappers.

```scheme
(import std.clp)
```

This page assumes you know the basic logic primitives
(`logic-var`, `unify`, `trail-mark`, `unwind-trail`); see
[`logic.md`](logic.md) for that material.

---

## 2. Runtime Substrate

### 2.1 Domain stores

A logic var becomes a *constrained* integer / FD variable when a
`Z` or `FD` domain is attached to it. Two storage shapes:

| Kind | Shape | Use |
|------|-------|-----|
| `ZDomain {lo, hi}` | Closed integer interval | dense ranges (`[1, 1000]`) |
| `FDDomain {v₁, v₂, …}` | Explicit sorted set | sparse / hole-bearing sets |

Both are immutable values; updates copy-on-write into a new domain.
The previous handle is recorded in a `Domain` trail entry, so
narrowing is fully undoable.

`%clp-get-domain v` returns one of:

- `(z lo hi)`
- `(fd v₁ v₂ ...)`
- `(r lo hi lo-open? hi-open?)` — see [`clpr.md`](clpr.md)

### 2.2 Domain-aware unification

`VM::unify` enforces CLP domains directly:

- Binding a domained var to a ground value runs an `O(log n)` /
  `O(1)` membership check before commit.
- `RDomain` accepts in-range fixnums or flonums.
- `ZDomain` / `FDDomain` reject non-integer flonums even if the
  numeric value is in range.
- Unifying two unbound domained vars *intersects* the domains
  first (via `domain_intersect`); failure rolls back cleanly.

Cross-kind intersection (`Z` ∩ `FD` ∩ `R`) is supported and goes
through `domain_intersect` in `clp/domain.h`.

### 2.3 Propagation queue

CLP propagators are not invoked recursively from inside `bind` —
that would risk unbounded stack depth. Instead the VM keeps an
**idempotent FIFO** of propagator thunks keyed by closure
object-id:

- Mark an attribute key as a propagation key with
  `(register-prop-attr! 'k)`.
- When a var carrying that key is bound, every thunk in the
  attribute value list is queued (deduped by id).
- The queue **drains at the outer `unify` boundary**, in
  insertion order.
- Any thunk returning `#f` (or raising) fails the surrounding
  `unify` and triggers full rollback to the most recent
  enclosing trail-mark.

This is the same machinery used by all of `std.clp`,
`std.clpr`, and `std.clpb`. Diagnostic:
`(%clp-prop-queue-size)`.

### 2.4 Trail entry kinds (CLP-relevant)

`(trail-mark)` stores the current trail depth as a fixnum.
`(unwind-trail m)` rolls back every entry above that mark,
restoring bindings, attributes, **domain state, CLP(R) posted
rows, and cached simplex bounds atomically**.

| Kind | Restored state |
|------|----------------|
| `Bind` | A logic-var binding |
| `Attr` | Attributed-variable slot (incl. propagator lists) |
| `Domain` | Z / FD / R domain write or erase |
| `RealStore` | CLP(R) row-log size |
| `SimplexBound` | Cached simplex bound on one variable |

---

## 3. `std.clp` — Domain Constructors

| Form | Meaning |
|------|---------|
| `(clp:domain x lo hi)` | `x ∈ {lo, lo+1, …, hi}` (Z domain) |
| `(clp:in-fd x v₁ v₂ …)` | `x` in the explicit finite set |
| `(clp:boolean x)` | Alias for `(clp:domain x 0 1)`; see [`clpb.md`](clpb.md) |

Domain introspection:

| Form | Returns |
|---|---|
| `clp:domain-z?`, `clp:domain-fd?`, `clp:domain-r?` | Kind predicates |
| `clp:domain-lo`, `clp:domain-hi` | Bounds (Z / R) |
| `clp:domain-values` | Enumerated values (FD) |
| `clp:domain-empty?` | `#t` iff narrowing failed silently |

---

## 4. Propagators

The wrappers in `std.clp` install a native `%clp-fd-*` propagator,
run it once at post-time, then attach a re-firing thunk under the
shared `'clp.prop` attribute key.

| Form | Constraint |
|------|------------|
| `(clp:= a b)` | `a = b` |
| `(clp:+ a b s)` | `a + b = s` |
| `(clp:plus-offset s x k)` | `s = x + k` (constant offset; used by N-queens) |
| `(clp:abs y x)` | `y = |x|` |
| `(clp:* a b p)` | `a * b = p` |
| `(clp:sum xs s)` | `Σ xs = s` |
| `(clp:scalar-product cs xs s)` | `Σ cᵢ·xᵢ = s` |
| `(clp:element i xs v)` | `xs[i] = v` (1-based) |
| `(clp:<= a b)`, `(clp:>= a b)`, `(clp:!=)` / `(clp:<>)` | Ordering / disequality |
| `(clp:all-different xs)` | All `xᵢ` pairwise distinct (Régin-style) |

### 4.1 Bounds vs domain consistency

The arithmetic propagators (`clp:+`, `clp:*`, `clp:scalar-product`,
`clp:plus-offset`) are **bounds-consistent**: they prune the
extrema of each participating domain, and they re-fire whenever any
participating bound changes. They do not enforce full
*hyper-arc*-consistency on internal holes, because that would cost
exponential time on long sums.

`clp:all-different` is the exception. It is **domain-consistent**
via Régin's algorithm:

1. Build the *value graph* — bipartite graph with variables on one
   side and feasible values on the other; edge `(x, v)` exists iff
   `v ∈ dom(x)`.
2. Find a maximum matching. If it has fewer edges than there are
   variables, the constraint is unsatisfiable.
3. Mark every edge that belongs to *some* maximum matching.
   Algorithmically this reduces to finding strongly-connected
   components in a directed graph constructed from the matching
   plus alternating paths to free vertices (Berge's theorem).
4. Remove every unmarked edge from the value graph; this prunes
   the corresponding values from each variable's domain.

The result: every value left in `dom(x)` *can* be extended to a
total assignment of all `xs`. This is dramatically stronger than
pairwise disequality (`O(n²)` `clp:!=` posts), and it is what makes
N-queens tractable at `n=30+` without exotic search tuning.

### 4.2 The propagation cycle, step by step

For `(clp:+ a b s)`:

1. Post-time: compute the bound box
   `[lo(a)+lo(b), hi(a)+hi(b)]`, intersect with `dom(s)`.
   Symmetrically tighten `dom(a)` against `[lo(s)-hi(b), hi(s)-lo(b)]`
   and `dom(b)`. Each tightening trails a `Domain` entry.
2. Attach the propagator thunk to the `'clp.prop` attribute of
   each of `a`, `b`, `s`.
3. Whenever any of those vars is bound (or its domain narrowed
   by another propagator), the thunk is queued; the next outer
   `unify` drains the FIFO. The thunk re-runs step 1.
4. If a domain becomes empty, the thunk returns `#f`; the
   surrounding `unify` fails; everything trails back to the most
   recent mark.

---

## 5. Labelling

Constraint posting only **prunes** domains; you still need a
search to commit each variable to a single value. `clp:labeling`
is the depth-first labeller.

```scheme
(clp:labeling vars)                         ; first solution, leftmost
(clp:labeling vars 'ff)                     ; legacy strategy alias
(clp:labeling vars 'strategy 'ff
                   'value-ordering 'down
                   'solutions 'all)
```

Supported options:

| Key | Values | Meaning |
|-----|--------|---------|
| `strategy` | `'leftmost`, `'ff` / `'smallest`, `'largest` | Variable-selection rule |
| `value-ordering` | `'up` (default), `'down` | Ascending or descending value try-order |
| `solutions` | `'first` (default), `'all`, integer `N` | How many to enumerate |
| `on-solution` | callback | Receives each solution snapshot |

**`'ff`** is *first-fail*: pick the variable whose domain has the
fewest remaining values. This is the standard heuristic for
constraint-satisfaction problems — narrowing the most-constrained
variable first prunes the search tree fastest, because failures
are detected nearer the root.

Return shape:

- `solutions = 'first` → `#t` / `#f`. **Bindings are kept** on
  success, so `(deref-lvar x)` afterwards returns the witness
  value.
- `solutions = 'all` or integer `N` → list of solution snapshots
  (alists). The variables are left **unbound** so further queries
  can still run.

`clp:solve` is a compatibility alias for first-solution labelling.

---

## 6. Worked Example — N-Queens

From [`cookbook/logic/nqueens.eta`](../../../cookbook/logic/nqueens.eta).

The puzzle: place `n` queens on an `n×n` chessboard so that no two
attack each other. Encode the row of queen `i` (1 ≤ i ≤ n) as the
known fact "queen `i` is in row `i`", and let the variable `Qᵢ`
denote its column.

### 6.1 Constraint encoding

Three `all-different` constraints — one per attack direction:

- **Columns.** `all-different(Q₁, …, Qₙ)` — no two queens in the
  same column.
- **`↘` diagonals.** Two queens `(i, Qᵢ)` and `(j, Qⱼ)` lie on the
  same descending diagonal iff `Qᵢ − i = Qⱼ − j`, i.e. iff
  `Qᵢ − i = Qⱼ − j`. So `all-different(Qᵢ − i)`.
- **`↗` diagonals.** Similarly `Qᵢ + i = Qⱼ + j` ⇒
  `all-different(Qᵢ + i)`.

But Eta's `clp:all-different` works on logic vars, not on raw
arithmetic expressions. So we introduce *auxiliary* variables
`aᵢ = Qᵢ + i` and `bᵢ = Qᵢ − i` via `clp:plus-offset`, and call
`all-different` on each list.

### 6.2 Code walkthrough

```scheme
(defun make-fd-vars (n lo hi)
  (let loop ((i 0) (acc '()))
    (if (>= i n) (reverse acc)
        (let ((v (logic-var)))
          (clp:domain v lo hi)         ; Qᵢ ∈ [1, n]
          (loop (+ i 1) (cons v acc))))))

(defun post-offsets (qs offs)          ; aᵢ = Qᵢ + offᵢ
  (let loop ((qs qs) (offs offs) (acc '()))
    (cond
      ((null? qs) (reverse acc))
      (else
        (let ((a (logic-var)))
          (clp:domain a -1000 1000)
          (clp:plus-offset a (car qs) (car offs))
          (loop (cdr qs) (cdr offs) (cons a acc)))))))

(defun solve-nqueens (n)
  (let* ((qs    (make-fd-vars n 1 n))
         (rows  (range 1 (+ n 1)))
         (nrows (map (lambda (i) (- i)) rows))
         (as    (post-offsets qs rows))      ; Qᵢ + i
         (bs    (post-offsets qs nrows)))    ; Qᵢ − i
    (clp:all-different qs)
    (clp:all-different as)
    (clp:all-different bs)
    (if (clp:labeling qs 'strategy 'ff)
        (map deref-lvar qs)
        #f)))
```

### 6.3 What happens when you call `(solve-nqueens 8)`

1. **Post-time pruning.** The three `all-different` calls trigger
   Régin's matching algorithm immediately. For `n=8` this already
   removes a handful of values (e.g. `Q₁ = Qₙ` is impossible
   because then both diagonals would coincide for `i = 1, n`).
2. **Labelling with `'ff`.** The labeller picks the variable with
   the smallest current domain (initially all are size 8 — ties
   broken left-to-right, so it starts with `Q₁`).
3. **Branch.** Try `Q₁ = 1`. This re-fires every propagator
   touching `Q₁`, `a₁`, or `b₁`. The matching is recomputed and
   typically prunes 1–3 columns from each remaining `Qᵢ`.
4. **Recurse.** The labeller picks the next variable by `ff` —
   often whichever queen had its domain pinched hardest by step 3.
5. **Backtrack on failure.** If a propagator leaves any domain
   empty, the surrounding `unify` returns `#f`; the labeller
   triggers `unwind-trail` to the choicepoint mark, then tries the
   next value.

For `n = 8` this terminates in a handful of milliseconds with
`(1 5 8 6 3 7 2 4)` (or any of the other 91 valid solutions; the
labeller returns the first one in lex order under `value-ordering
'up`). For comparison, the same problem with pairwise `clp:!=`
posts (no Régin) needs ~100× more backtracks at `n = 12`.

---

## 7. Worked Example — SEND + MORE = MONEY

From [`cookbook/logic/send-more-money.eta`](../../../cookbook/logic/send-more-money.eta).

```
    S E N D
  + M O R E
  ─────────
  M O N E Y
```

Each letter is a distinct digit `[0..9]`; `S` and `M` are non-zero
(no leading zeros). Unique solution: `9567 + 1085 = 10652`.

### 7.1 Linear encoding

Expand `SEND + MORE = MONEY` column by column with carries, then
collapse to a *single* linear equation by multiplying through:

```
1000·S + 100·E + 10·N + 1·D
+ 1000·M + 100·O + 10·R + 1·E
= 10000·M + 1000·O + 100·N + 10·E + 1·Y
```

Move everything to one side:

```
1000·S + 91·E − 90·N + 1·D − 9000·M − 900·O + 10·R − 1·Y = 0
```

This is exactly the shape `clp:scalar-product` accepts.

### 7.2 Code

```scheme
(let ((s (logic-var)) (e (logic-var)) (n (logic-var)) (d (logic-var))
      (m (logic-var)) (o (logic-var)) (r (logic-var)) (y (logic-var)))
  (clp:domain s 1 9)  (clp:domain m 1 9)            ; leading non-zero
  (for-each (lambda (v) (clp:domain v 0 9))
            (list e n d o r y))
  (clp:all-different (list s e n d m o r y))
  (clp:scalar-product '(1000 91 -90 1 -9000 -900 10 -1)
                      (list s e n d m o r y)
                      0)
  (clp:labeling (list s e n d m o r y) 'strategy 'ff))
```

### 7.3 Why first-fail matters here

After posting:

- Régin prunes `all-different` so each of the eight digits has
  domain `[0, 9]` minus values forced by other domains. Initially
  this is barely a narrowing — sizes are 8 or 9.
- The scalar-product propagator runs bounds reasoning on each
  variable and shaves `M = 1` immediately (because the only way to
  get a 5-digit sum from two 4-digit numbers is for `M` to be `1`).
- That triggers `O = 0` (carry from the most-significant column).

After this initial cascade `M` and `O` are ground, and the labeller
starts on the next-smallest domain. `'ff` is essential: leftmost
labelling would attack `S` first, which has the largest remaining
domain and yields a much wider search tree.

The whole search terminates in well under a millisecond.

---

## 8. Integer Optimization — Branch-and-Bound

```scheme
(clp:minimize cost goal-thunk)
(clp:maximize cost goal-thunk)
```

Both implement classical branch-and-bound:

```
best = +inf                      ; for minimize
loop:
  m = trail-mark
  if (goal-thunk) :
      v = deref-lvar(cost)
      if v < best :
          best = v
          remember witness
  unwind-trail(m)
  ; tighten the cost domain so future iterations must beat the incumbent
  if best < +inf :
      try post  cost <= best - 1
      if infeasible -> done
  else :
      done
return (best, witness)
```

The post-and-fail pattern relies on the unified trail: each
iteration runs the goal under a fresh trail-mark, captures the
witness if it beats the incumbent, and rolls back. The loop
terminates because `cost` has a finite integer domain that is
strictly tightened each iteration.

For *real* objectives (LP / QP), use `clp:r-minimize` /
`clp:rq-minimize` from [`std.clpr`](clpr.md) instead — those don't
need branch-and-bound because the simplex / active-set solvers
return the optimum directly.

---

## 9. Composition with `findall` / `run1`

CLP variables are ordinary logic variables, so they compose with
the search combinators in [`std.logic`](logic.md):

```scheme
;; Enumerate all solutions of a small puzzle
(let ((x (logic-var)) (y (logic-var)))
  (clp:domain x 1 5) (clp:domain y 1 5)
  (clp:!= x y)
  (findall (lambda () (cons (deref-lvar x) (deref-lvar y)))
           (list (lambda () (clp:labeling (list x y) 'solutions 'first)))))
```

For *all* solutions, prefer `(clp:labeling vars 'solutions 'all)`
which returns a list of snapshots without leaving bindings — much
cheaper than wrapping label calls inside `findall`.

CLP also composes with fact-table queries from [`std.db`](logic.md#9-stddb--fact-table-relations):
table reads produce normal numbers which can be combined into CLP
expressions with no special bridge primitive.

---

## 10. CLP(R), CLP(B), and Mixed Modelling

The continuous-domain story is in [`clpr.md`](clpr.md) — real
intervals, simplex-backed feasibility/bounds/optimization, convex QP.

The Boolean story is in [`clpb.md`](clpb.md) — reified `{0,1}`
constraints (`clp:and`, `clp:or`, `clp:xor`, `clp:imp`, `clp:eq`,
`clp:not`, `clp:card`) plus `clp:labeling-b` / `clp:sat?` /
`clp:taut?`. CLP(B) variables are technically `clp:domain v 0 1`
under the hood, so they coexist transparently with `std.clp`
(e.g. you can put a Boolean indicator into a `clp:scalar-product`).

Mixed integer / Boolean / continuous models are supported as long
as each variable carries one domain kind. Cross-kind unification
intersects via `domain_intersect` (see §2.2) — but the typical
pattern is to keep integer and real layers separate and link them
through ground intermediate values.

---

## 11. Current Limitations

- **CLP(R) posting is linear.** Only the optimization path (`clp:rq-*`)
  accepts convex quadratic objectives. See [`clpr.md`](clpr.md).
- **CLP(R) posting rejects vars** that already carry a non-`R`
  domain (you cannot mix Z and R on the same logic var).
- **No SAT solver under CLP(B).** The propagator covers
  bounds-consistency on each connective; for full satisfiability
  you must label.
- **No core cut.** Search-strategy control is library-level.

---

## 12. Source Locations

| Component | File |
|-----------|------|
| Domain types (`Z`, `FD`, `R`) + intersection | [`clp/domain.h`](../../../eta/core/src/eta/runtime/clp/domain.h) |
| Domain store | [`clp/constraint_store.h`](../../../eta/core/src/eta/runtime/clp/constraint_store.h) |
| Linear / quadratic linearizer | [`clp/linear.cpp`](../../../eta/core/src/eta/runtime/clp/linear.cpp), [`clp/quadratic.cpp`](../../../eta/core/src/eta/runtime/clp/quadratic.cpp) |
| CLP(R) append log + simplex-bound cache | [`clp/real_store.h`](../../../eta/core/src/eta/runtime/clp/real_store.h) |
| Simplex backend | [`clp/simplex.cpp`](../../../eta/core/src/eta/runtime/clp/simplex.cpp) |
| Active-set QP backend | [`clp/qp_solver.cpp`](../../../eta/core/src/eta/runtime/clp/qp_solver.cpp) |
| FM reference oracle | [`clp/fm.cpp`](../../../eta/core/src/eta/runtime/clp/fm.cpp) |
| Unification + rollback + propagation queue | [`vm/vm.h`](../../../eta/core/src/eta/runtime/vm/vm.h), [`vm/vm.cpp`](../../../eta/core/src/eta/runtime/vm/vm.cpp) |
| CLP builtins (`%clp-domain-*`, `%clp-fd-*`, `%clp-r-*`, `%clp-r-qp-*`, `%clp-bool-*`) | [`core_primitives.h`](../../../eta/core/src/eta/runtime/core_primitives.h) |
| `std.clp` wrappers | [`stdlib/std/clp.eta`](../../../stdlib/std/clp.eta) |
| `std.clpr` wrappers | [`stdlib/std/clpr.eta`](../../../stdlib/std/clpr.eta) |
| `std.clpb` wrappers | [`stdlib/std/clpb.eta`](../../../stdlib/std/clpb.eta) |
| CLP(FD) tests | [`stdlib/tests/clp.test.eta`](../../../stdlib/tests/clp.test.eta) |
| CLP(R) tests | [`stdlib/tests/clpr_simplex.test.eta`](../../../stdlib/tests/clpr_simplex.test.eta), [`stdlib/tests/clpr_optimization.test.eta`](../../../stdlib/tests/clpr_optimization.test.eta), [`stdlib/tests/clpr_qp_optimization.test.eta`](../../../stdlib/tests/clpr_qp_optimization.test.eta) |
| Worked examples | [`cookbook/logic/nqueens.eta`](../../../cookbook/logic/nqueens.eta), [`cookbook/logic/send-more-money.eta`](../../../cookbook/logic/send-more-money.eta), [`cookbook/quant/portfolio-lp.eta`](../../../cookbook/quant/portfolio-lp.eta) |

