# Logic Programming

[← Back to README](../README.md) · [Bytecode & VM](bytecode-vm.md) ·
[Runtime & GC](runtime.md) · [Architecture](architecture.md) ·
[Modules & Stdlib](modules.md)

---

## Overview

Eta provides **native structural unification** as a first-class VM feature,
giving you the core machinery of a Prolog engine without leaving the language.
Six built-in special forms are compiled directly to dedicated opcodes; no
external library, no interpretation overhead, and no separate runtime process.

```scheme
(define x (logic-var))          ; allocate a fresh unbound variable
(unify x (cons 1 (cons 2 '()))) ; bind it to a term: x = (1 2)
(println (deref-lvar x))        ; => (1 2)
(println (ground? x))           ; => #t  (no unbound vars in the term)
```

The [**`std.logic`**](../stdlib/std/logic.eta) library builds Prolog-style
`findall`, `naf`, `copy-term`, and `run1` combinators on top of those primitives.

```bash
etai examples/unification.eta
```

---

## The Six Built-in Primitives

These are **special forms** handled at every level of the pipeline
(expander → semantic analyser → emitter → VM).  They are not function calls —
each compiles to a single dedicated opcode.

| Form | Opcode | Description |
|------|--------|-------------|
| `(logic-var)` | `MakeLogicVar` | Allocate a fresh, unbound logic variable on the heap |
| `(unify a b)` | `Unify` | Structural unification of `a` and `b`; pushes `#t` or `#f` |
| `(deref-lvar x)` | `DerefLogicVar` | Walk the variable chain to find the current value |
| `(trail-mark)` | `TrailMark` | Snapshot the trail stack depth; pushes a fixnum mark |
| `(unwind-trail mark)` | `UnwindTrail` | Undo all bindings made since `mark` |
| `(ground? t)` | *(builtin)* | `#t` iff `t` contains no unbound logic variables |

`ground?` is registered as a **builtin function** (not a special form) so it
appears in the global slot table alongside `car`, `cons`, etc.

---

## How It Is Implemented

### Logic Variables on the Heap

A logic variable is a heap-allocated object of kind `ObjectKind::LogicVar`:

```cpp
struct LogicVar {
    std::optional<LispVal> binding;   // nullopt → unbound
};
```

The `std::optional` sentinel is essential.  In Eta, `Nil == False == #f` at
the NaN-box level.  Using `Nil` as "unbound" would make it impossible to
successfully unify a variable to `#f`.  `std::optional` is the only safe
choice.

`(logic-var)` allocates a fresh `LogicVar{std::nullopt}` via `make_heap_object`
and pushes the boxed `HeapObject` reference (a 47-bit `ObjectId` in the NaN
payload).

### Dereferencing — Variable Chains

When two unbound logic variables are unified with each other, one's `binding`
is set to point to the other.  This creates a **variable chain**.  The VM
walks the chain until it finds either an unbound variable or a concrete term:

```cpp
LispVal VM::deref(LispVal v) {
    for (;;) {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return v;
        auto id = ops::payload(v);
        auto* lv = try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
        if (!lv || !lv->binding.has_value()) return v;
        v = *lv->binding;
    }
}
```

`DerefLogicVar` pops a value, calls `deref`, and pushes the result.  If `v`
is already a concrete value (fixnum, symbol, pair, …) it is returned
unchanged.

### Structural Unification — Robinson's Algorithm

`Unify` pops `b` then `a`, calls `VM::unify(a, b)`, and pushes `#t` or `#f`.
The implementation is Robinson's original algorithm (1965) with an occurs check:

```
unify(a, b):
  a = deref(a),  b = deref(b)
  if a == b             → succeed (same value, equal atoms)
  if a is unbound lvar  → if occurs_check(a, b) fail
                          else bind a → b, push to trail, succeed
  if b is unbound lvar  → if occurs_check(b, a) fail
                          else bind b → a, push to trail, succeed
  if both are Cons      → unify(car(a), car(b))  AND  unify(cdr(a), cdr(b))
  if both are Vectors   → unify element-by-element (same length required)
  otherwise             → fail
```

#### Occurs Check

Before binding variable `x` to term `t`, the occurs check walks `t`
recursively:

```cpp
bool VM::occurs_check(LispVal x, LispVal t) {
    t = deref(t);
    if (t == x) return true;          // x appears in t — would be cyclic
    if (auto* cons = ...) {
        return occurs_check(x, cons->car) || occurs_check(x, cons->cdr);
    }
    // vectors likewise
    return false;
}
```

This prevents the creation of infinite (cyclic) terms:

```scheme
(define z (logic-var))
(unify z (cons z '()))    ; => #f  — occurs check fires
```

Without the occurs check, `z` would be bound to a circular list, causing any
subsequent traversal (e.g., `ground?`, `display`) to loop forever.

### The Trail Stack

Every successful binding made inside `unify` is recorded in
`VM::trail_stack_`, a `std::vector<LispVal>` that stores the **boxed reference**
to the logic variable that was bound (not a raw pointer — raw pointers are not
stable across GC cycles in a concurrent heap):

```cpp
std::vector<LispVal> trail_stack_;   // per-VM, not shared
```

`TrailMark` reads `trail_stack_.size()` and pushes it as a fixnum.

`UnwindTrail` pops the mark fixnum, then pops entries from the trail down to
that mark, resetting each variable's binding to `std::nullopt`:

```cpp
case OpCode::UnwindTrail: {
    int64_t mark = ops::decode<int64_t>(pop()).value();
    while ((int64_t)trail_stack_.size() > mark) {
        auto var_ref = trail_stack_.back();
        trail_stack_.pop_back();
        auto id = ops::payload(var_ref);
        if (auto* lv = try_get_as<ObjectKind::LogicVar, types::LogicVar>(id))
            lv->binding = std::nullopt;
    }
    break;
}
```

This is exactly Prolog's WAM trail mechanism, expressed in terms of Eta's
heap and NaN-boxed values.

### GC Integration

The trail stack is a GC root.  During `collect_garbage()` the VM marks every
`LispVal` in `trail_stack_`:

```cpp
for (auto v : trail_stack_) visit(v);
```

This prevents a logic variable that is referenced only by the trail (e.g., an
intermediate variable created inside a failed branch) from being swept while
a backtracking context is still active.

The `LogicVar` heap type also participates in the `HeapVisitor` protocol:

```cpp
void visit_logic_var(const types::LogicVar& lv) override {
    if (lv.binding.has_value()) callback(*lv.binding);
}
```

A bound variable's binding target is reachable; an unbound variable contributes
nothing to the live set.

### `ground?` — Recursive Term Inspection

`ground?` is a native C++ builtin that walks the term graph recursively:

```cpp
std::function<bool(LispVal)> is_ground = [&](LispVal v) -> bool {
    LispVal curr = v;
    for (;;) {
        if (!is_boxed(curr) || tag(curr) != Tag::HeapObject)
            return true;                       // inline primitive — always ground
        auto id = payload(curr);
        if (auto* lv = try_get_as<LogicVar>(id)) {
            if (!lv->binding.has_value()) return false;   // unbound
            curr = *lv->binding;               // follow chain
        } else if (auto* cons = try_get_as<Cons>(id)) {
            return is_ground(cons->car) && is_ground(cons->cdr);
        } else if (auto* vec = try_get_as<Vector>(id)) {
            for (auto& elem : vec->elements)
                if (!is_ground(elem)) return false;
            return true;
        } else {
            return true;          // string, closure, port — always ground
        }
    }
};
```

---

## Compilation Pipeline

The six special forms pass through all pipeline stages.  Here is the path for
`(unify a b)`:

```
Source text
  "(unify a b)"
      │
      ▼ Expander (expander.cpp)
  Recognised as reserved keyword "unify"
  handler: handle_unify(lst)
      → validates arity (must be exactly 2 args)
      → expands sub-forms a and b recursively
      → returns (unify <expanded-a> <expanded-b>)
      │
      ▼ SemanticAnalyzer (semantic_analyzer.cpp)
  handler for "unify" form → emits IR node:
      core::Unify { lhs: NodePtr, rhs: NodePtr }
      │
      ▼ Emitter (emitter.cpp)
  emit_unify(node):
      emit_node(node.lhs)   // push a
      emit_node(node.rhs)   // push b
      emit(OpCode::Unify)
      │
      ▼ VM (vm.cpp)  case OpCode::Unify:
      LispVal b = pop(), a = pop()
      push(unify(a, b) ? True : False)
```

The same path applies to all six forms, each with its own Core IR node type:
`MakeLogicVar`, `Unify`, `DerefLogicVar`, `TrailMark`, `UnwindTrail`.

---

## End-to-End Trace

Tracing `(unify x 42)` where `x` is a fresh logic variable:

```scheme
(define x (logic-var))
(unify x 42)
(println (deref-lvar x))
```

**Emitted bytecode (simplified):**

```
;; (define x (logic-var))
  MakeLogicVar               ; allocates LogicVar{nullopt}, pushes HeapObj(id=7)
  StoreGlobal  N             ; globals[N] = HeapObj(id=7)
  Pop

;; (unify x 42)
  LoadGlobal   N             ; push HeapObj(id=7)   ← x
  LoadConst    0             ; push Fixnum(42)
  Unify                      ; pop b=42, pop a=HeapObj(7)
                             ;   deref(a) = HeapObj(7)  ← unbound lvar
                             ;   deref(b) = Fixnum(42)  ← concrete
                             ;   occurs_check(a, b) = false  (42 has no vars)
                             ;   lv[7].binding = Fixnum(42)
                             ;   trail_stack_.push_back(HeapObj(7))
                             ;   push True
  Pop                        ; discard #t

;; (deref-lvar x)
  LoadGlobal   N             ; push HeapObj(id=7)
  DerefLogicVar              ; pop HeapObj(7)
                             ;   lv[7].binding = Fixnum(42)  → return 42
                             ;   push Fixnum(42)
```

**Backtracking the binding:**

```scheme
(define x (logic-var))
(define m (trail-mark))       ; m = Fixnum(0)  (trail is empty)
(unify x 99)
;; x is now 99
(unwind-trail m)
;; trail unwound to depth 0: lv[x].binding = nullopt
(logic-var? (deref-lvar x))  ; => #t  (x is unbound again)
```

```
  TrailMark                  ; push Fixnum(trail_stack_.size() = 0)
  StoreGlobal  M             ; globals[M] = Fixnum(0)

  LoadGlobal   N             ; push HeapObj(x)
  LoadConst    K             ; push Fixnum(99)
  Unify                      ; binds x→99, trail_stack_ = [HeapObj(x)]
  Pop

  LoadGlobal   M             ; push Fixnum(0)
  UnwindTrail                ; pop mark=0
                             ;   trail_stack_.size()=1 > 0
                             ;   pop HeapObj(x), lv[x].binding = nullopt
                             ;   trail_stack_ = []
```

---

## The `std.logic` Library

`(import std.logic)` provides higher-level combinators.  The module is fully
self-contained — no external dependencies.

### `==` — Unification Goal

```scheme
(defun == (a b) (unify a b))
```

Wraps the `unify` special form as an ordinary callable function.  Useful when
passing unification as a value (e.g., into `findall` branches).

```scheme
(== x 42)          ; same effect as (unify x 42)
(if (== a b) ...)  ; use result as boolean
```

### `copy-term` — Fresh Variable Copy

```scheme
(copy-term t)
```

Returns a **deep copy** of `t` in which every unbound logic variable has been
replaced by a fresh one.  Sharing is preserved: if the same unbound variable
appears at multiple positions in `t`, all those positions map to the same
fresh variable in the copy.

**Why it matters:**  If you store a parameterised rule as a term template
`(head . body)` and want to try it more than once, each attempt needs its own
independent set of variables.  `copy-term` provides that isolation.

```scheme
(define tmpl (cons (logic-var) (logic-var)))   ; (?a . ?b)

(define c1 (copy-term tmpl))
(define c2 (copy-term tmpl))

(== (car c1) 'alice)
(== (car c2) 'bob)

(deref-lvar (car c1))   ; => alice
(deref-lvar (car c2))   ; => bob
(logic-var? (car tmpl)) ; => #t  — original is untouched
```

**Implementation:** purely functional memo-threaded recursion (no mutation):

```scheme
(defun %ct-walk (x memo)
  (let ((xd (deref-lvar x)))
    (cond
      ((logic-var? xd)
       (let ((cached (%ct-assq xd memo)))
         (if cached
             (cons cached memo)
             (let ((nv (logic-var)))
               (cons nv (cons (cons xd nv) memo))))))
      ((pair? xd)
       (let* ((r1 (%ct-walk (car xd) memo))
              (r2 (%ct-walk (cdr xd) (cdr r1))))
         (cons (cons (car r1) (car r2)) (cdr r2))))
      (#t (cons xd memo)))))
```

Each call returns `(new-value . updated-memo)`.  The `eq?`-based memo lookup
(`%ct-assq`) preserves identity — two occurrences of the same unbound variable
both map to the same fresh copy.

### `naf` — Negation as Failure

```scheme
(naf goal-thunk)
```

Probes `goal-thunk` in isolation.  **Always** unwinds any bindings the goal
made, regardless of success or failure.  Returns `#t` if the goal failed;
`#f` if it succeeded.

```scheme
(define x (logic-var))
(naf (lambda () (== x 1)))   ; => #t  (x=1 succeeds, naf returns #f → wait...)
;; after naf: x is unbound regardless
```

More precisely: `(naf g)` is equivalent to `(not (succeeds? g))`.

```scheme
(defun naf (goal-thunk)
  (let* ((m  (trail-mark))
         (ok (goal-thunk)))
    (unwind-trail m)
    (not ok)))
```

### `succeeds?` — Non-Destructive Probe

```scheme
(succeeds? goal-thunk)
```

Like `naf` but returns `#t` on success and `#f` on failure.  Bindings are
always rewound — useful for pure membership tests:

```scheme
(succeeds? (lambda () (== x 42)))
;; x is still unbound after this call
```

### `findall` — Collect All Solutions

```scheme
(findall template-thunk branches)
```

`branches` is a list of zero-argument goal thunks — one per disjunctive
alternative.  For each branch that succeeds, `findall`:

1. Evaluates `(template-thunk)` while the trail bindings are live
2. Records the value
3. Unwinds the trail (backtracks)

Returns the collected values in order.  This is the Prolog `findall/3` analogue.

**Pattern — inline database query:**

```scheme
(define parent-db '((tom bob) (tom liz) (bob ann)))

(let* ((pv (logic-var))
       (cv (logic-var))
       (_ (== pv 'tom))
       (branches (map* (lambda (fact)
                         (lambda ()
                           (and (== pv (car fact))
                                (== cv (cadr fact)))))
                       parent-db))
       (sols (findall (lambda () (deref-lvar cv)) branches)))
  (println sols))
; => (bob liz)
```

> [!TIP]
> **Relational style:** the inline traversal above is correct but embeds the
> traversal logic at every call site.  [`examples/logic.eta`](../examples/logic.eta)
> shows how to lift this into a **named relation** (`parento`) that returns
> branches and works in all query directions — children-of, parents-of, all
> pairs, membership — without any changes to the search engine.

**How backtracking works inside `findall`:**

```
for each branch:
  m ← trail-mark
  ok ← run branch thunk
  if ok:
    val ← template-thunk   ; read bound values while trail is live
    unwind-trail m          ; undo this branch's bindings
    collect val
  else:
    unwind-trail m          ; undo partial bindings from failed branch
```

### `run1` — First Solution

```scheme
(run1 template-thunk branches)
```

Returns the value of `(template-thunk)` for the first succeeding branch,
or `#f` if no branch succeeds.

### `membero` — Nondeterministic List Membership

```scheme
(membero x lst)
```

Returns one goal branch per element of `lst`; each branch unifies `x` with that
element.  Pass the result directly to `findall` or `run1`:

```scheme
(let* ((x (logic-var)))
  (findall (lambda () (deref-lvar x)) (membero x '(a b c))))
; => (a b c)
```

When `x` is already bound, failing branches are silently skipped, leaving only
the matching element(s).  Analogue of Prolog's `member/2`.

---

## Usage Patterns

### Pattern 1 — Fact Database with `findall`

Build one branch thunk per fact.  The thunk attempts to unify query variables
against the fact's fields.  This inline form is the foundation; for a reusable,
bidirectional version see [**`examples/logic.eta`**](../examples/logic.eta) — the
*relational* approach wraps this into a named `parento` function that works as a
forward lookup, backward lookup, or full enumeration without modification.

```scheme
(defun db-branches (pv cv db)
  (map* (lambda (fact)
           (lambda ()
             (and (== pv (car fact))
                  (== cv (cadr fact)))))
         db))

;; All parent→child pairs
(let* ((p (logic-var)) (c (logic-var))
       (sols (findall (lambda () (cons (deref-lvar p) (deref-lvar c)))
                      (db-branches p c parent-db))))
  (println sols))
; => ((tom . bob) (tom . liz) (bob . ann) ...)
```

### Pattern 2 — Derived Relations (Manual Join)

Compose two `findall` calls to implement a join.  The inner `findall` materialises
intermediate values with a clean trail; the outer loop builds per-intermediate
branches.  See [`examples/logic.eta`](../examples/logic.eta) for the full
`grandparento` derivation with annotated output.

```scheme
;; grandparent(GP, GC) :- parent(GP, Mid), parent(Mid, GC)
(defun solve-grandparent (db)
  (let* ((gp  (logic-var))
         (mid (logic-var))
         (gp-mid (findall (lambda () (cons (deref-lvar gp) (deref-lvar mid)))
                          (db-branches gp mid db))))
    ;; For each (gp . mid) pair, find all children of mid
    (foldr
      (lambda (pair acc)
        (let* ((gp-val  (car pair))
               (mid-val (cdr pair))
               (gc      (logic-var))
               (gc-sols (findall (lambda () (deref-lvar gc))
                                 (db-branches mid-val gc db))))
          (append (map* (lambda (gc-val) (cons gp-val gc-val)) gc-sols)
                  acc)))
      '()
      gp-mid)))
```

### Pattern 3 — Pattern Matching via Unification

```scheme
;; Decode a tagged message against several shape patterns
(defun dispatch (msg)
  (let* ((x    (logic-var))
         (y    (logic-var))
         (m    (trail-mark))
         (ok   (unify msg (cons 'add (cons x (cons y '()))))))
    (if ok
        (begin (println (+ (deref-lvar x) (deref-lvar y)))
               (unwind-trail m))
        (begin
          (unwind-trail m)
          ;; try next pattern …
          ))))

(dispatch '(add 3 4))    ; => 7
```

### Pattern 4 — Parameterised Rules with `copy-term`

When you represent a rule's head as a term template (rather than as a
closure), `copy-term` lets you instantiate it freshly for each invocation:

```scheme
(define rule-head (cons (logic-var) (logic-var)))  ; (?X . ?Y)

(defun try-rule (a b)
  (let* ((head (copy-term rule-head))
         (ok   (and (== a (car head))
                    (== b (cdr head)))))
    ok))

;; Both calls use independent copies of ?X and ?Y
(try-rule 1 2)   ; binds copy1.X=1, copy1.Y=2
(try-rule 3 4)   ; binds copy2.X=3, copy2.Y=4
```

---

## Relation to Prolog

| Prolog Feature | Eta Equivalent | Notes |
|----------------|----------------|-------|
| Unification `X = Y` | `(unify x y)` or `(== x y)` | Robinson's algorithm with occurs check |
| Logic variable `_X` | `(logic-var)` | Heap-allocated; GC-managed |
| Dereferencing | `(deref-lvar x)` | Explicit; no implicit deref |
| Backtracking (choice point) | `(trail-mark)` / `(unwind-trail m)` | Manual; composable |
| `findall/3` | `(findall thunk branches)` | Branch list explicit |
| Negation as failure `\+` | `(naf thunk)` | Always unwinds |
| `copy_term/2` | `(copy-term t)` | Functional memo-table implementation |
| `ground/1` | `(ground? t)` | VM builtin, walks heap graph |
| `assert/retract` | *(not built-in)* | Use mutable list + `set!` |
| Cut `!` | *(not built-in)* | `run1` covers the most common use |
| DCG | *(not built-in)* | Encodeable as difference lists |

**What Eta adds that Prolog lacks:**

- Full Scheme semantics — closures, TCO, `call/cc`, `dynamic-wind`,
  `define-syntax`, module system — all coexist with unification.
- Bindings are first-class NaN-boxed values, so logic variables can
  hold any Eta value including closures, ports, and vectors.
- The trail is per-VM and cooperates with the mark-sweep GC.

---

## What Is Not Supported (Yet)

| Feature | Status |
|---------|--------|
| `copy-term` as a VM opcode | Implemented in Eta; a future `CopyTerm` opcode would eliminate per-pair allocation overhead |
| Attributed variables | Not supported |
| Tabling / memoisation | Not supported; would require a WAM-style call stack |

---

## Source Locations

| Component | File |
|-----------|------|
| `LogicVar` heap type | [`logic_var.h`](../eta/core/src/eta/runtime/types/logic_var.h) |
| `ObjectKind::LogicVar` | [`heap.h`](../eta/core/src/eta/runtime/memory/heap.h) |
| `MakeLogicVar` … `UnwindTrail` opcodes | [`bytecode.h`](../eta/core/src/eta/runtime/vm/bytecode.h) |
| `VM::unify`, `deref`, `occurs_check` | [`vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp) |
| `trail_stack` GC rooting | [`vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp) — `collect_garbage()` |
| `logic-var?`, `ground?` builtins | [`core_primitives.h`](../eta/core/src/eta/runtime/core_primitives.h) |
| Expander handlers | [`expander.cpp`](../eta/core/src/eta/reader/expander.cpp) |
| Core IR nodes | [`core_ir.h`](../eta/core/src/eta/semantics/core_ir.h) |
| Emitter | [`emitter.cpp`](../eta/core/src/eta/semantics/emitter.cpp) |
| `std.logic` library (`==`, `findall`, `membero`, …) | [`stdlib/std/logic.eta`](../stdlib/std/logic.eta) |
| Low-level example (unification, backtracking, `db-branches`) | [`examples/unification.eta`](../examples/unification.eta) |
| Relational example (`parento`, `grandparento`, `membero`) | [`examples/logic.eta`](../examples/logic.eta) |

