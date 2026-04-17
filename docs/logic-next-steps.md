# Logic & CLP — Next Steps

A phased roadmap bringing Eta's logic / CLP subsystem from its current
"library + forward-checker" design toward a full **WAM-class** engine:
native choice-points, attributed variables, AC-3 propagation, CLP(B)/CLP(R),
first-argument indexing, and finally a dedicated WAM-style bytecode layer
integrated into the existing Eta VM.

Each phase is self-contained, extends `stdlib/tests/logic.test.eta`-style
tests, and cross-references the C++ runtime modules already in place.

---

## Guiding Principles (Holistic, Single-VM)

These principles apply to **every** phase below and override any local
decision that would contradict them:

1. **One VM, one heap, one GC, one bytecode stream.** All logic / CLP /
   WAM features are *additive* to the existing Eta VM (`eta/core/src/eta/runtime/vm`).
   No satellite interpreter, no second dispatch loop, no parallel value
   representation. WAM opcodes live in the *same* opcode table
   (`runtime/vm/bytecode.h`) and are dispatched by the *same* `VM::run`
   loop as ordinary Scheme/Eta opcodes.
2. **Reuse existing heap types before inventing new ones.** Where an
   existing type already fits the job it is preferred over a
   logic-specific clone. Concretely:
   - **Clause database = `FactTable`** (see Phase 7). Ground facts
     *are* rows; per-column hash indexes *are* first-argument indexing.
   - Continuation / frame stack reused for `LogicFrame` (Phase 8),
     not a sibling stack.
   - Trail lives on the existing GC root set, not on a private
     allocator.
3. **Logic values = Eta values.** Logic vars, compound terms, attributed
   vars are ordinary heap objects (`ObjectKind::…`) visible to the GC,
   nanboxing, serializer, DAP, LSP, and value-formatter without special
   cases. The same `LispVal` flows through Scheme functions and Prolog
   goals.
4. **Shared infrastructure for constraints.** One `PropagationQueue`,
   one attributed-variable mechanism, one trail — reused by CLP(FD),
   CLP(B), CLP(R/Q), `dif`, `freeze`, and tabling. No per-solver
   bespoke plumbing.
5. **No forked toolchains.** Disassembler, serializer, DAP, LSP, fuzzer,
   benchmark harness, and REPL all learn the new opcodes in-place. A
   feature that cannot be debugged / traced / serialized through the
   existing toolchain is not done.

---

## Current State Assessment

### Shipped (C++ VM-level)

- `LogicVar` heap kind (`eta/core/src/eta/runtime/types/logic_var.h`) + 6
  opcodes: `MakeLogicVar`, `Unify`, `DerefLogicVar`, `TrailMark`,
  `UnwindTrail`, `CopyTerm` (in `eta/core/src/eta/runtime/vm/bytecode.h`,
  dispatched in `vm.cpp`).
- Robinson unification with **configurable occurs-check** (`'always` /
  `'never` / `'error`, Phase 1); trail stack as GC root; per-VM trail
  is now a `std::vector<TrailEntry>` with a tagged `Kind` (`Bind`,
  `Attr` reserved for Phase 3).
- **`CompoundTerm` heap kind** (Phase 1) with `(term 'f a1 … aN)` /
  `compound?` / `functor` / `arity` / `arg`; `VM::unify` / `occurs_check`
  / `copy_term` / `ground?` all recurse through it.
- `ConstraintStore` with `ZDomain` / `FDDomain`
  (`eta/core/src/eta/runtime/clp/`); forward-checker hook inside
  `VM::unify`; packed trail mark (23 + 23 bits).
- Primitives: `%clp-domain-z!`, `%clp-domain-fd!`, `%clp-get-domain`,
  `logic-var?`, `logic-var/named`, `var-name`, `ground?`,
  `set-occurs-check!`, `occurs-check-mode`, `term`, `compound?`,
  `functor`, `arity`, `arg`.

### Shipped (Eta-level)

- `stdlib/std/logic.eta`: `==`, `copy-term*`, `naf`, `succeeds?`,
  `findall`, `run1`, `membero`.
- `stdlib/std/clp.eta`: `clp:domain`, `clp:in-fd`, `clp:=`, `clp:+`,
  `clp:<=`, `clp:>=`, `clp:<>`, `clp:all-different` (global list,
  not trailed), `clp:solve` (DFS labelling).

### Gaps vs a WAM

- **No native choice-point stack** — disjunction is simulated by
  `findall` walking a list of thunks; no incremental/lazy enumeration,
  no `call/cc`-free cut.
- **No cut (`!`)**, no if-then-else (`->`), no `catch`/`throw` for
  logic failure.
- **No attributed variables** → CLP wakeup on var–var unification is
  impossible; propagators only fire at grounding.
- **No propagation queue / AC-3**; `clp:all-different` is a flat global
  list (lost on backtrack — `*clp-adiff-groups*` is a plain `set!`).
- **No compound-term heap object** — ~~terms are built out of
  `Cons`/`Vector`, forcing linear structural walks; no functor/arity
  header, no structural hash~~ **Resolved in Phase 1:** new
  `ObjectKind::CompoundTerm` with O(1) functor/arity and builtins
  `term` / `compound?` / `functor` / `arity` / `arg`.
- **Occurs check is always-on** — ~~no toggle, unlike SWI's
  `unify_with_occurs_check`~~ **Resolved in Phase 1:** `(set-occurs-check! 'always/'never/'error)`
  + `(occurs-check-mode)`.
- **No clause database**, no first-argument indexing, no tabling / SLG.
- **No register machine** — no X/A/Y registers, environment frames,
  `try / retry / trust`, `allocate / deallocate`, LCO for logic calls.
- **No CLP(B)**, **no CLP(R/Q)**; integer-only; `clp:+` is the only
  arithmetic propagator.
- **No debugger/tracer** for goals (DAP knows about host frames only).

---

## Phase 1 — Solidify Core Term Model & Trail API ✅ **COMPLETE**

**Goal:** put unification on a proper WAM-ready foundation before any
advanced feature.

### Progress

| Item | Status | Notes |
|---|---|---|
| Occurs-check toggle — `(set-occurs-check! 'always / 'never / 'error)` + `(occurs-check-mode)` | ✅ **Done** | `VM::OccursCheckMode` enum on the VM; `'error` mode surfaces a `RuntimeError` from the `Unify` opcode. |
| Named logic variables — `(logic-var/named 'x)` / `(var-name v)` | ✅ **Done** | Optional `std::string name` on `LogicVar`; value-formatter prints `_alphaG1234`; zero impact on unification semantics. |
| `CompoundTerm` heap kind + `(term 'f a1 … aN)` / `(compound? t)` / `(functor t)` / `(arity t)` / `(arg i t)` | ✅ **Done** | New `ObjectKind::CompoundTerm`, new `types/compound.h`, GC visitor, factory, value-formatter case (`f(1, 2)`). `VM::unify`, `occurs_check`, `copy_term` and `ground?` all recurse through compound args; `copy_term` preserves sharing of repeated vars. Implemented as a **runtime builtin** (`term`, arity 1 + rest) rather than a special form — no new opcode, no bytecode / serializer / disassembler churn, works identically with existing `LoadConst` + `Call` dispatch. |
| Structured `TrailEntry { Kind { Bind, Attr } … }` | ✅ **Done** | Per-VM `trail_stack_` refactored from `std::vector<LispVal>` to `std::vector<TrailEntry>` with a tagged `Kind` (`Bind` live; `Attr` reserved for Phase 3 attributed-variable writes — `prev_value` slot in place). GC roots, `UnwindTrail` dispatch, unify write sites, and the DAP `enumerate_gc_roots` label all updated. The packed binding-trail / constraint-trail `TrailMark` fixnum layout is unchanged. |
| Phase 1 tests | ✅ **Done** | `stdlib/tests/logic.test.eta` groups `named-logic-vars` (6 tests), `occurs-check-toggle` (5 tests), `compound-terms` (19 tests). End-to-end smoke: `stdlib/tests/logic_phase1_smoke.eta`, `stdlib/tests/compound_smoke.eta`. |

### Success criteria (met)

- ✅ All existing tests green — **9/9 stdlib test files pass, Boost `eta_core_test` = *No errors detected***.
- ✅ `(unify (term 'point x 1) (term 'point 2 y))` binds `x=2, y=1`.
- ✅ Toggling occurs-check changes `(unify z (term 'f z))` behaviour —
  `'always` → `#f`, `'never` → `#t` (cyclic), `'error` → runtime error.
- ✅ Trail unwind still atomically restores bindings and domains; the
  packed mark survives both kinds of trail write.

### Deliberate scope trims

- **No new `MakeCompound` opcode.** `(term 'f …)` is a runtime builtin
  with arity 1 + rest, registered via `builtin_names.h` / `core_primitives.h`.
  Rationale: zero parser/expander/emitter/serializer/disassembler churn,
  yet identical observable semantics. A dedicated opcode can be added in
  Phase 8 if WAM-style register allocation justifies it.
- **No `TrailEntry::Kind::Domain`.** CLP domain changes continue to flow
  through `ConstraintStore`'s private trail and the packed 23-bit
  constraint-trail half of the `TrailMark` fixnum — Phase 1's refactor
  only generalises the *binding* trail. Domain-kind trail entries can be
  added later if Phase 4 merges the two stores.

---

## Phase 2 — Native Choice-Points, Cut, If-Then-Else, Catch/Throw

**Goal:** replace list-of-thunks disjunction with a real backtracking
primitive inside the VM so cut and LCO become possible.

| Work item | Touches |
|---|---|
| `ChoicePoint` struct on a new `VM::cp_stack_`: `{ pc_alt, sp, trail_mark, constraint_mark, env_ptr, cut_barrier }`. GC-root it. | `vm.h`, `vm.cpp` |
| New opcodes `TryMeElse <alt>`, `RetryMeElse <alt>`, `TrustMe`, `CutTo <cp_mark>`, `Throw`, `Catch`. | `runtime/vm/bytecode.h`, `disassembler.h`, emitter |
| New special form `(conde (g1 …) (g2 …) …)` — a real disjunction that installs a choice point and re-enters on failure; `(if-> cond then else)`; `(!)` cut; `(throw tag val)`, `(catch tag body handler)`. | `expander.cpp`, `semantic_analyzer.cpp`, `core_ir.h`, `emitter.cpp` |
| Rewrite `findall`, `run1` in `stdlib/std/logic.eta` to sit on top of `conde` + an internal `enumerate` primitive returning a lazy stream of solutions; keep old list-of-thunks API as a thin compat shim. | `stdlib/std/logic.eta`, `vm.cpp` |
| New goal combinators in `std.logic`: `conde`, `conda` (soft-cut), `condu` (committed choice), `fresh`, `disj2`, `conj2`. | `stdlib/std/logic.eta` |
| Tests: `choice-points`, `cut`, `if-then-else`, `throw-catch`, lazy `run*` returning N-th solution without materialising the rest. | `stdlib/tests/logic.test.eta`, new `stdlib/tests/conde.test.eta` |

**Success criteria:** 8-queens expressible via `conde` without
list-of-thunks scaffolding; `(run1 …)` does not evaluate later
branches; benchmark `unify/backtrack` throughput ≥ 3× current
`findall`-based loop.

---

## Phase 3 — Attributed Variables + Wakeup Hooks ✅ **MVP COMPLETE**

**Goal:** give constraint libraries a first-class hook into
unification — the foundation for real CLP and for tabling.

### Progress

| Item | Status | Notes |
|---|---|---|
| Attributed-variable storage | ✅ **Done** | Extended the existing `LogicVar` (`eta/core/src/eta/runtime/types/logic_var.h`) with a `std::unordered_map<InternId, LispVal> attrs;` field rather than forking a sibling `AttrVar` heap kind. Zero ABI churn — every existing switch on `ObjectKind::LogicVar` keeps working unchanged. `attr-var?` is true iff the var is unbound and `!attrs.empty()`. |
| Primitives `put-attr / get-attr / del-attr / attr-var?` | ✅ **Done** | Registered in `core_primitives.h` alongside `logic-var?`. Keys are intern-id symbols; values are arbitrary `LispVal`. |
| Trailed attribute writes | ✅ **Done** | `TrailEntry` (`vm.h`) extended with `module_key: InternId` and `had_prev: bool`. `put-attr` and `del-attr` snapshot the prior slot state onto `trail_stack_`; the existing `TrailEntry::Kind::Attr` slot reserved in Phase 1 now unwinds correctly (restore previous value, or erase if the slot was absent). GC roots walk `prev_value` for `Attr` entries. |
| Per-module `attr-unify-hook` registry | ✅ **Done** | `(register-attr-hook! 'module proc)` installs a hook on the VM (`attr_unify_hooks_` map, GC-rooted). `VM::unify` fires every applicable hook after committing a binding, via `call_value`; a hook returning `#f` causes unify to fail (caller's trail-mark unwinds). |
| Eta-level `freeze` + `dif` | ✅ **Done** | `stdlib/std/freeze.eta` exports `(freeze v goal-thunk)` (delayed goal list, fires once when `v` binds) and `(dif x y)` (per-variable disequality witness list, re-probed via trail-mark / unify / unwind on every binding). |
| Phase 3 smoke test | ✅ **Done** | `stdlib/tests/attrvar_smoke.eta` — 30+ assertions covering put/get/del round-trip, trail unwind of set / overwrite / delete, hook fires on unify, failing hook aborts unify + leaves var unbound, `freeze` / `dif` ground + unbound paths. |

### Success criteria (met for MVP)

- ✅ `put-attr` / `del-attr` are trailed — any backtrack via
  `unwind-trail` restores pre-write state.
- ✅ A registered `attr-unify-hook` fires exactly on bindings that
  touch an attributed variable; a `#f` return aborts the unify and
  the would-be binding is unwound.
- ✅ `(freeze v (lambda () body))` fires exactly once when `v` is
  later bound, including trail-aware cases.
- ✅ `(dif x y)` rejects subsequent unifies that would equate its
  arguments, with clean binding rollback on rejection.

### Deliberate scope trims (for follow-up work)

- **Var–var attribute / domain intersection** — ✅ **Resolved in Phase 4b.**
  `VM::unify` (`vm.cpp`) now intersects CLP domains BEFORE binding two
  unbound logic vars: `domain_intersect` (added to `clp/domain.h`,
  handles every Z/FD cross-kind combination) is invoked, the result is
  installed on the surviving var via the trailed `ConstraintStore::set_domain`,
  and an empty intersection fails the unify cleanly.  Both trails are
  snapshotted before the intersect / bind / hook chain so a hook
  rejection rolls back atomically.  Smoke test
  `stdlib/tests/clp_varvar_merge_smoke.eta` covers Z∩Z, FD∩FD, FD∩Z,
  asymmetric (only one side has a domain), disjoint→fail, and trail
  restoration.  *Module-level attribute merging* (e.g. `freeze` goal
  list concatenation when both vars have `'freeze` attrs) is still the
  responsibility of each module's registered `attr-unify-hook` — the
  hook fires with `(var=A, bound-val=B, attr-val=A's value)` and can
  read `(get-attr B key)` to merge as needed (matches SWI semantics).
- **No wakeup queue / overflow cap.** ✅ **Resolved in Phase 4b.**  A
  VM-level FIFO `PropagationQueue` with closure-identity dedup now
  drains at the outer-`unify` boundary; `freeze` / `dif` keep their
  inline call_value path (they never recursively schedule), while
  domain propagators register their attribute key via the new
  `register-prop-attr!` builtin and are queued.
- **No CLP port yet.** `ConstraintStore` still owns FD/Z domains and
  still trails them through its private trail mark (23-bit slot).
  Migrating domains to live as `'clp.fd` attributes on the variable
  is a Phase 4 prerequisite; leaving it in place keeps existing CLP
  tests green while Phase 3 lands.

---

## Phase 4 — CLP(FD) with AC-3 Propagation & Labeling Strategies ✅ **MVP COMPLETE (Eta-level)**

**Goal:** real finite-domain solver that *actually propagates* on every
binding, not just at grounding.  Phase 4 MVP delivers this entirely in
Eta atop the Phase 3 attribute substrate — **zero C++ churn** — leaving
native C++ propagators, bit-set domains and Régin all-different for a
later Phase 4b once the API has stabilised.

### Progress

| Item | Status | Notes |
|---|---|---|
| All-different via trailed attribute groups | ✅ **Done** | Each participating variable carries a `'clp.adiff` attribute whose value is the list of siblings.  Attribute writes are trailed (Phase 3), so backtracking cleanly undoes group membership — replaces the non-trailed `*clp-adiff-groups*` set!-global. |
| AC-3 wakeup inside `unify` | ✅ **Done** | `%clp-adiff-hook`, registered via `register-attr-hook!`, fires on every binding that touches a var carrying `'clp.adiff`.  It prunes the bound value from every sibling's FD domain; singletons trigger a recursive `unify`, so propagation runs to fixpoint *inside* the original unify call (no explicit queue needed for MVP scope). |
| `clp:!=` / `clp:<>` | ✅ **Done** | Now both implemented as 2-var all-different groups, reusing the AC-3 hook. |
| First-fail labelling | ✅ **Done** | `(clp:labeling vars 'ff)` picks the var with the smallest current domain at every step; ties broken by list order. `(clp:labeling vars 'leftmost)` preserves existing behaviour.  `clp:solve` is a back-compat shim for `'leftmost`. |
| Branch-and-bound optimisation | ✅ **Done** | `clp:minimize cost thunk` / `clp:maximize cost thunk` iteratively tighten the Z-domain bound on `cost` outside per-attempt trail-marks, returning the optimum (or `#f` if no solution exists). |
| Phase 4 smoke test | ✅ **Done** | `stdlib/tests/clp_phase4_smoke.eta` — AC-3 prunes siblings on bind, singleton cascade grounds, unsat detection, backtracking restores both domain *and* attribute state, `clp:!=`, first-fail labelling, pigeonhole (4-in-3 UNSAT), 4-in-4 Latin row, and `clp:minimize` correctness. |

### Success criteria (met for MVP)

- ✅ Binding `X = v` where `X` is in an all-different group with `Y`
  immediately narrows `Y`'s FD domain — no wait for `clp:solve`.
- ✅ Singleton domains propagate transitively: a prune that reduces
  `Y` to `{k}` recursively unifies `Y = k`, firing `Y`'s hooks.
- ✅ Backtracking undoes attribute additions AND domain narrowings.
- ✅ Pigeonhole detected infeasible without enumerating any labelling.
- ✅ `clp:minimize` converges to the true optimum.

### Deliberate scope trims (deferred to Phase 4b / 8)

- **Native C++ propagators.** `fd_plus`, `fd_times`, `fd_abs`,
  `fd_element`, `fd_sum`, `fd_scalar_product`, `fd_all_different`
  (Régin-style value-graph matching) all remain un-implemented.  The
  current Eta-level AC-3 is O(n²) per bind on group size — fine for
  toy problems, insufficient for serious CSP workloads.
- **Explicit `PropagationQueue`.** ✅ **Resolved in Phase 4b.**  Propagation
  now drains via a VM-level FIFO with idempotence on closure identity;
  the outer `VM::unify` call drains exactly once before returning, so
  cascades are bounded-stack and fair.  See the Phase 4b row for details.
- **Bit-set FD domain representation.** Domains remain `std::vector<int64_t>`
  lists.  Roaring-bitmap / bitset-chunk representations are a
  Phase 4b task coupled to the Régin propagator rewrite.
- **`clp:labeling` option list.** Current dispatch is a positional
  second argument taking a single symbol (`'leftmost` / `'ff`).  The
  full option-list API (`#:strategy ff #:variable-ordering …`) will
  land alongside the option set that actually matters to the C++
  propagators.
- **N-queens diagonals / send+more=money.** Both require arithmetic
  propagators (`fd_plus` with non-variable offsets) that are in the
  Phase 4b scope.  The Phase 4 MVP test therefore substitutes a
  pigeonhole-UNSAT + 4-in-4 Latin-row + `clp:minimize` trio as the
  "solver actually works" signal.

---

## Phase 4b — Native Propagators & Search API 🚧 **IN PROGRESS**

**Goal:** retire every Phase 4 scope-trim.  Native C++ bounds-consistency
propagators replace the Eta-level arithmetic; Régin matching replaces
the pairwise attribute-hook all-different; labeling grows a proper
keyword-option API; bitset FD domains and an explicit VM-level
propagation queue round out the substrate.

### Progress

| Item | Status | Notes |
|---|---|---|
| `%clp-fd-plus!` / `%clp-fd-plus-offset!` / `%clp-fd-abs!` | ✅ **Done** | Bounds-consistency propagators in `core_primitives.h`, mixed Z/FD/ground operands, trailed via the unified `VM::trail_set_domain`.  Eta wrappers `clp:+`, `clp:plus-offset`, `clp:abs` in `stdlib/std/clp.eta` install a re-propagator thunk per participating var under the `'clp.prop` attribute (now an *async-thunk attribute* — see queue row below); each binding enqueues every thunk for FIFO drain at the outer-`unify` boundary, so propagators re-narrow to fixpoint without recursive C++ stack growth and without duplicate work when one bind fires the same propagator from many siblings. |
| `%clp-fd-times!` / `%clp-fd-sum!` / `%clp-fd-scalar-product!` / `%clp-fd-element!` | ✅ **Done** | Interval-multiplication with floor/ceil quotient back-propagation (skipped when divisor straddles zero); sum forward+back; signed-coefficient scalar-product; 1-based element constraint with both ground and FD-domained index. Eta wrappers `clp:*`, `clp:sum`, `clp:scalar-product`, `clp:element` attach `'clp.prop` thunks.  Smoke test `stdlib/tests/clp_phase4b_arith.eta` includes a full **SEND+MORE=MONEY** end-to-end solve. |
| `clp:labeling` option-list API | ✅ **Done** | Plist-style keyword args — `(clp:labeling vars 'strategy 'ff 'value-ordering 'down 'solutions 'all 'on-solution proc)`.  Positional back-compat `(clp:labeling vars 'ff)` detected by single-symbol rest arg.  `'solutions 'all` / integer N enumerate with an outer trail mark so vars are left unbound after return.  `'on-solution` callback may return `#f` to short-circuit.  Regression test `stdlib/tests/clp_labeling_options.eta`. |
| N-queens + SEND+MORE=MONEY examples | ✅ **Done** | `examples/nqueens.eta` (8-queens demo + `solve-nqueens n`), `examples/send-more-money.eta` (verified 9567+1085=10652).  Regression test `stdlib/tests/clp_nqueens.eta` asserts known solution counts for N ∈ {4, 5, 6}. |
| Régin-style `fd_all_different` (value-graph matching + SCC elim) | ✅ **Done** | `eta/core/src/eta/runtime/clp/alldiff_regin.h` implements the textbook Régin algorithm: augmenting-path max-matching on the bipartite value graph → iterative Tarjan SCCs of the directed orientation → BFS reachability from free values → every non-vital edge becomes a domain prune.  Exposed as `%clp-fd-all-different!`; `clp:all-different` is now a thin wrapper that posts the native propagator and attaches a re-firing `'clp.prop` thunk.  **Deletes** the pairwise `'clp.adiff` attribute-hook plumbing entirely — no shim, no dead code.  Smoke test `stdlib/tests/clp_alldiff_regin_smoke.eta` verifies the canonical "X,Y∈{1,2}, Z∈{1,2,3} → Z=3" pruning that pairwise AC-3 cannot discover, plus pigeonhole UNSAT at post-time, ground-value propagation, re-firing on bind, trail restoration, and a 4×4 Latin square. |
| Explicit VM-level `PropagationQueue` | ✅ **Done** | New `prop_queue_` (FIFO `std::deque<LispVal>`) + `prop_queued_set_` dedup on closure `ObjectId`, plus a `unify_depth_` counter on `VM` (`vm.h`/`vm.cpp`).  `VM::unify` is now an outer wrapper around `unify_internal`: nested calls (compound recursion, sync-hook re-entry, propagator-driven cascades) return straight through; the *outer* call snapshots the trail, runs the inner step, then drains the queue once before returning.  A drained thunk returning `#f` triggers the existing atomic rollback (binding + attr + Domain trail entries unwound) and clears any still-pending entries.  Hook routing: synchronous hooks (registered via `register-attr-hook!`, used by `freeze` / `dif`) keep their inline call_value path; *async-thunk attributes* registered via the new `register-prop-attr!` builtin (currently `'clp.prop`) instead enqueue every thunk in the attribute's list — replacing the old recursive `%clp-prop-hook` walker entirely.  GC-rooted via the queue itself; `enumerate_gc_roots` exposes a `Propagation Queue` category for DAP.  `(%clp-prop-queue-size)` introspection primitive added.  Smoke test `stdlib/tests/clp_prop_queue_smoke.eta` covers re-firing on bind, atomic rollback on drain failure, idempotent FIFO under all-different broadcast, queue quiescence at the user boundary, and Domain-trail unwind after both var-var intersection and propagator narrowing. |
| Bit-set FD domain representation | ✅ **Done** | `clp::FDDomain` (`eta/core/src/eta/runtime/clp/domain.h`) is now a chunked 64-bit bit-set `{ int64_t base; std::vector<uint64_t> bits; int64_t count; }` instead of a sorted `std::vector<int64_t>`.  `empty` / `size` / `contains` are O(1); `min` / `max` are one `countr_zero`/`countl_zero` per non-empty word; iteration streams bits via `for_each` in ascending order; `intersect` walks the smaller operand and probes the larger, `intersect_z` masks against a Z interval.  `shrink_to_fit` trims leading / trailing zero words after every narrowing so the stored chunks stay tight.  Builders `from_sorted_unique` / `from_unsorted` / `from_range` / `singleton` cover every construction site.  All 9 call sites in `core_primitives.h` (`%clp-domain-fd!`, `%clp-get-domain`, `extract_bounds`, `narrow_var`, `%clp-fd-element!`, `%clp-fd-all-different!` domain materialisation + narrow callback) migrated to the new API; `FDDomain::values` is gone.  Unified-trail `TrailEntry::Kind::Domain` snapshots the bit-set chunks directly — no extra allocation per trailed write.  Existing CLP smoke tests (`clp.test.eta`, `clp_phase4_smoke.eta`, `clp_phase4b_arith.eta`, `clp_alldiff_regin_smoke.eta`, `clp_labeling_options.eta`, `clp_nqueens.eta`, `clp_prop_queue_smoke.eta`, `clp_varvar_merge_smoke.eta`) remain the behavioural oracle — iteration order, domain-values list shape, and `clp:domain-values` output are all preserved. |
| Phase 3 var–var attribute merge path | ✅ **Done** | `VM::unify` now intersects CLP domains on var–var alias via the new `domain_intersect` helper in `clp/domain.h`; the surviving var receives the intersected domain through trailed `VM::trail_set_domain`, empty intersection fails the unify, and the outer-unify wrapper rolls back atomically (Bind + Attr + Domain trail entries) on hook rejection.  Smoke test `stdlib/tests/clp_varvar_merge_smoke.eta`. |
| Phase 1 `TrailEntry::Kind::Domain` (unified domain trail) | ✅ **Done** | `ConstraintStore` no longer keeps a private trail — domain mutations are recorded as `TrailEntry::Kind::Domain` entries on the shared `VM::trail_stack_` via `VM::trail_set_domain` / `trail_erase_domain`, snapshotting the prior `std::optional<clp::Domain>`.  `OpCode::TrailMark` is now a single fixnum (binding-trail size); `OpCode::UnwindTrail` restores Bind / Attr / Domain entries through one switch.  Eliminates the packed 23+23-bit mark and makes the binding trail the single source of truth for backtracking, paving the way for bitset domains (which can now snapshot the bitset chunk into the same trail entry). |

---

## Phase 5 — CLP(B) Boolean Constraints ✅ **MVP COMPLETE (propagation-only)**

**Goal:** Boolean constraint solver, sharing the propagation infra
from Phase 4.

Phase 5 delivers Option A from the recommendation table — pure
support-based propagation, no BDD backend.  Boolean propagators are
peers of the CLP(FD) propagators: same `'clp.prop` queue, same
unified domain trail, same labelling story (Boolean vars carry a
`Z[0,1]` domain so `clp:labeling` from `std.clp` already does the
right thing).  Option B (BDD-backed `sat-count` / `taut?`) remains
deferred behind a feature flag for a follow-up phase.

### Progress

| Item | Status | Notes |
|---|---|---|
| Boolean domain on existing FD/Z substrate | ✅ **Done** | A "Boolean" is just an integer drawn from {0, 1}: declared via `(clp:boolean v)` which installs `ZDomain{0,1}`.  No new `BDomain` heap kind — `bool_view` (`core_primitives.h`) folds the current domain into a 2-bit `mask` (bit 0 = may be 0, bit 1 = may be 1).  `narrow_bool` writes back via the unified trail (`VM::trail_set_domain`), preserving the FD-vs-Z kind of the prior domain so cross-domain narrowings stay consistent. |
| Native Boolean propagators (`%clp-bool-and!` / `or!` / `xor!` / `imp!` / `eq!` / `not!`) | ✅ **Done** | Built on a generic `propagate_ternary` / `propagate_binary` truth-table walker — for each row of the constraint's 4-row truth table, mark the row alive iff every operand's mask permits its value, then narrow each operand to the OR of bits over alive rows.  Domain-consistent on 2-value vars and unifies all 5 ternary connectives behind one helper.  `narrow_bool` is **value-captured** (not by-reference) into the helpers — these helpers are then captured into the registered builtin closures which outlive `register_core_primitives()`; an earlier `[&narrow_bool]` capture caused a dangling-reference crash on first call (Phase 5 commit notes record the lifetime contract). |
| Cardinality propagator `%clp-bool-card!` | ✅ **Done** | Standard 2-bound cardinality: count `forced_1` (mask = {1}) and `possible_1` (mask ∩ {1} ≠ ∅); fail if `forced_1 > k_hi` or `possible_1 < k_lo`; force every open var (mask = {0,1}) to 0 when `forced_1 == k_hi`, to 1 when `possible_1 == k_lo`.  Trailed through `narrow_bool`. |
| Eta-level wrappers in `std.clpb` | ✅ **Done** | New module `stdlib/std/clpb.eta` exports `clp:boolean`, `clp:and`, `clp:or`, `clp:xor`, `clp:imp`, `clp:eq`, `clp:not`, `clp:card`, `clp:labeling-b`, `clp:sat?`, `clp:taut?`.  Each posting helper runs the native propagator once, then attaches a re-firing thunk under the same shared `'clp.prop` queue attribute used by CLP(FD) — so Boolean and FD constraints participate in the same FIFO drain, no parallel queue.  `clp:sat?` / `clp:taut?` wrap `clp:labeling` in a trail-mark / unwind so the vars are left unbound on return. |
| Builtin registration order | ✅ **Done** | New `%clp-bool-*` names registered in `core_primitives.h` immediately after `%clp-fd-all-different!`, mirrored in `builtin_names.h` (analysis-only LSP env) in the exact same order — keeps the LSP slot indices aligned with the runtime registry per the file's invariant header. |
| Phase 5 smoke test | ✅ **Done** | `stdlib/tests/clpb_smoke.eta` covers (1–6) every native propagator's forward & back-propagation paths, (7) cardinality saturation, (8) trailed Boolean-domain unwind, (9) `sat?` / `taut?` leaving vars unbound, (10) tautology detection of `(x ∨ ¬x)` via `clp:taut?`, (11) **pigeonhole 4 → 3 UNSAT** detected by labelling + propagation, (12) majority-of-3 enumerated to exactly 4 models.  Test 10 documents the propagator-queue semantics that `'clp.prop` re-firing happens on bindings (not on domain narrowings) — matching CLP(FD); singleton narrowings cascade through labelling rather than the unify queue. |

### Success criteria (met for MVP)

- ✅ `(clp:boolean v)` installs a `Z[0,1]` domain visible to all FD machinery.
- ✅ Every Boolean connective enforces domain-consistent pruning on
  initial post AND re-fires on later bindings via the shared `'clp.prop`
  queue.
- ✅ Cardinality saturates: `(clp:card xs k k)` pins every open var
  once `forced_1` or `possible_1` reaches the bound.
- ✅ Backtracking restores Boolean domains atomically through the
  unified trail.
- ✅ Pigeonhole 4 → 3 detected UNSAT by `clp:taut?` (labelling +
  cardinality propagation, no manual hint).
- ✅ Tautology / unsatisfiability of `¬(x ∨ ¬x)` detected by
  `clp:taut?` after a single `unify` posts the negation.

### Deliberate scope trims (deferred)

- **BDD / ROBDD backend.**  No `clp:sat-count`, no `clp:taut?` via
  variable-elimination — `clp:taut?` here is "labelling finds no
  satisfying assignment", which is correct but exponential in the
  worst case.  The roadmap's Option B (CUDD / `clp/bdd.h`) behind a
  feature flag is the natural follow-up; pigeonhole(8 → 7) is the
  benchmark gate before that lands.
- **Tseitin / SAT-bridge.**  Option C (MiniSat) is not in scope.
- **`labeling-b` keyword options.**  `clp:labeling-b` is a thin alias
  for `clp:labeling` (which already sees `(0 1)` from `clp:domain-values`
  on a Z domain) — Boolean-specific value/variable orderings can be
  added when they have a measurable win on a real CLP(B) workload.

### Cross-cutting

- The roadmap principle "one VM, one queue, one trail" holds: Boolean
  propagators reuse every piece of the Phase 3 / Phase 4 / Phase 4b
  substrate.  No new opcode, no new `ObjectKind`, no new trail entry,
  no new attribute key.
- The capture-lifetime trap (`[&narrow_bool]` vs `[narrow_bool]`)
  caught here is now documented inline at the helper definition so
  future propagator additions don't regress.

---

## Phase 5 — Original Plan (preserved for reference)

**Goal:** Boolean constraint solver, sharing the propagation infra
from Phase 4.

| Work item | Touches |
|---|---|
| `BDomain` (domain over `{0,1}`) as a specialization of FD bitset. | `clp/domain.h` |
| Native propagators: `bool_and`, `bool_or`, `bool_xor`, `bool_not`, `bool_imp`, `bool_eq`, `bool_card` (cardinality `k-of-n`). | `clp/propagators.cpp` |
| Optional BDD / ROBDD backend (`cudd`-style) behind a feature flag for `clp:sat-count`, `clp:taut?`. | new `clp/bdd.h`, `cmake/FetchCudd.cmake` |
| New Eta API: `clp:boolean`, `clp:and`, `clp:or`, `clp:not`, `clp:xor`, `clp:sat?`, `clp:taut?`, `clp:labeling-b`. | new `stdlib/std/clpb.eta` |
| Tests: pigeon-hole UNSAT, tautology detection, majority circuit. | new `stdlib/tests/clpb.test.eta` |

**Success criteria:** pigeonhole(8 → 7) detected UNSAT within 1 s via
propagation + labeling; `sat-count` on 20-var formula via BDD
< 100 ms.

---

## Phase 6 — CLP(R/Q) Linear Arithmetic

> **Implementation breakdown:** see [`phase6_implementation.md`](phase6_implementation.md)
> for the seven-stage delivery plan (6.1 – 6.7) with per-stage smoke
> tests, success criteria, and dependency arrows.

**Goal:** linear constraint solving over reals (and rationals), so
quant-finance / causal code can express continuous constraints.

| Work item | Touches |
|---|---|
| `RDomain { double lo, hi; bool lo_open, hi_open; }` (+ `QDomain` via `boost::multiprecision`). | `clp/domain.h` |
| Incremental simplex tableau (Dutertre–de-Moura style) for equalities and strict/non-strict inequalities; attached as VM-level solver with trailing snapshots. | new `clp/simplex.h`, `clp/simplex.cpp` |
| Fallback Fourier-Motzkin for small problems / tests. | `clp/fm.cpp` |
| New Eta API: `clp:real`, `clp:r=`, `clp:r<=`, `clp:r+`, `clp:r*scalar`, `clp:r-minimize`. | new `stdlib/std/clpr.eta` |
| Integration with `CompoundTerm` linear expressions (`(+ (* 2 x) y)`). | `vm.cpp`, `stdlib/std/clpr.eta` |
| Tests: linear feasibility, LP minimisation, mixed CLP(FD)+CLP(R) via attribute co-existence. | new `stdlib/tests/clpr.test.eta`, `examples/portfolio-lp.eta` |

**Success criteria:** 50-variable LP solved in ms; trail unwinding
correctly restores tableau state; portfolio example terminates.

---

## Phase 7 — Clause Database (on `FactTable`), First-Argument Indexing, Tabling

**Goal:** real Prolog-style predicate database — prerequisite for
Phase 8 WAM bytecode — **reusing the existing `FactTable` heap type
rather than introducing a parallel clause store.**

### Why `FactTable` is the right substrate

Eta already ships a GC-managed columnar store with per-column hash
indexes (`eta/core/src/eta/runtime/types/fact_table.h`, see
[`docs/fact-table.md`](fact-table.md)). That is *exactly* what a Prolog
clause database needs:

| Prolog concept | `FactTable` mapping |
|---|---|
| Predicate `p/N` | One `FactTable` with `N` columns (per arity; or a single table keyed by `(functor, arity)` — see below) |
| Ground fact `p(a₁,…,aₙ)` | One row; each `aᵢ` goes into `columns[i]` |
| First-argument indexing | `fact-table-build-index! ft 0` on column 0 |
| Multi-argument indexing | `build-index!` on any/all columns — already supported |
| Fact lookup by key | `fact-table-query ft col key` → row-id list (O(1) amortised) |
| GC of fact terms | Already handled by `visit_fact_table` |
| Serialization / DAP view | Already handled by existing FactTable formatter |

Non-ground clauses (rules, or facts containing logic vars / compound
terms) live in an auxiliary **`rule_column`** attached to the same
table: an extra column holding a compiled-clause `LispVal` (a closure
or a WAM code pointer from Phase 8). A boolean "ground?" column lets
the engine take the fast path (pure FactTable query) for the ground
majority and fall back to the rule column only when needed.

### Work items

| Work item | Touches |
|---|---|
| Extend `FactTable` with (a) an optional `rule_column` of `LispVal` (compiled clause body), (b) a `ground_mask` bitset marking purely-ground rows, (c) a small header `{ functor: SymbolId, arity: uint8_t }` so a table self-identifies as a predicate. Keeps the existing stats / causal usage unchanged. | `types/fact_table.h`, `memory/mark_sweep_gc.h` (already visits columns — extend to `rule_column`) |
| New module `std.db`: `(defrel name (args …) body …)` macro. Each `defrel` registers (or reuses) one `FactTable` per `name/arity` in the current module's predicate registry. A ground clause `(defrel (edge 'a 'b))` becomes `fact-table-insert!`; a rule clause `(defrel (parent ?x ?y) (edge ?x ?y))` inserts a row with the compiled body in the rule column and `ground_mask[row] = 0`. | new `stdlib/std/db.eta`, new `core_primitives.h` entries |
| `(assert clause)`, `(retract clause)`, `(retract-all pattern)`, `(call-rel name . args)`. Implemented on top of the existing `%fact-table-insert!`, `%fact-table-query`, plus a new `%fact-table-delete-row!` (logical-delete via tombstone column, or compact on GC). | `stdlib/std/db.eta`, `types/fact_table.h`, `core_primitives.h` |
| Native opcode `CallRel <predicate-id>`: (1) deref + classify first argument, (2) if ground → `FactTable::query(0, key)` → install a choice point over the returned row-id list, (3) if var → iterate all rows (or use `var_bucket` fallback), (4) for each candidate row, unify remaining columns and, if non-ground, invoke rule-column body. Reuses the Phase-2 choice-point stack. | `runtime/vm/bytecode.h`, `vm.cpp` |
| Multi-argument / composite indexing: `(index-rel! name col-spec)` wraps `%fact-table-build-index!`. Compiler heuristic: auto-index arg 0 of every `defrel`. | `stdlib/std/db.eta`, `core_primitives.h` |
| Variant-based tabling: `(tabled rel)` annotation caches call patterns in a sibling `FactTable` — same substrate, different predicate id — keyed by the variant of the call. SLG-lite (no delaying) for ground/stratified rels; fallback to plain resolution otherwise. Tabled answer store *is literally a FactTable*, reusing its indexes. | new `vm/tabling.h`, `vm.cpp`, `stdlib/std/db.eta` |
| Term-hash primitive `(term-hash t depth)` used by multi-arg indexing and by `VariantTable` keys. | `core_primitives.h` |
| Tests: genealogy rel with first-arg indexing (measure index-miss vs full-scan), same-generation with tabling (terminates where plain Prolog loops), mixed ground/rule predicate. | new `stdlib/tests/db.test.eta`, new `examples/tabling.eta` |

**Success criteria:**
- 10 k-fact DB lookup ≥ 50× faster than linear `findall`-over-thunks —
  i.e. matches raw `fact-table-query` throughput, confirming no
  overhead vs the existing columnar path.
- Left-recursive `ancestor` rel terminates under `tabled`.
- A `defrel` predicate and a `make-fact-table` of the same shape are
  interchangeable via `fact-table-query` (holistic invariant).

---

## Phase 8 — WAM-Style Bytecode Layer (Inside the Existing VM)

**Goal:** dedicated register-machine opcodes for logic, **added to the
existing Eta bytecode table and dispatched by the existing `VM::run`
loop** — not a second VM. Enables tail-call / last-call optimization
for logic goals with zero impact on ordinary Eta code paths.

**Non-goals (explicit):**
- ❌ No separate WAM interpreter, process, thread, or dispatch loop.
- ❌ No second value representation, second heap, second GC, second
  serializer, second DAP.
- ❌ No "logic mode" flag that changes VM semantics globally. WAM
  opcodes are just more opcodes; Scheme opcodes keep running unchanged
  in the same function if they appear there.

| Work item | Touches |
|---|---|
| Add register file to the existing `VM`: `X[0..N]` argument/temp regs and `Y[]` environment-local regs as plain fields on `VM`. A WAM stack frame is just a variant of the existing activation record (`LogicFrame` = tagged case of the current frame union), sharing the continuation stack and GC roots. | `vm.h`, `vm.cpp` |
| New opcodes **appended to the existing `Opcode` enum** (group `0x80–0xBF`, reserving space in the same table): `GetVar`, `GetVal`, `GetConst`, `GetStruct`, `GetList`, `PutVar`, `PutVal`, `PutConst`, `PutStruct`, `PutList`, `UnifyVar`, `UnifyVal`, `UnifyConst`, `UnifyVoid`, `Allocate`, `Deallocate`, `Call`, `Execute` (LCO), `Proceed`, `TryMeElse / RetryMeElse / TrustMe` (reuse Phase 2), `SwitchOnTerm`, `SwitchOnConst`, `SwitchOnStruct` (indexing — drive directly off the `FactTable` indexes from Phase 7). | `runtime/vm/bytecode.h`, `runtime/vm/vm.cpp` (new `case` arms in the existing dispatch switch), `disassembler.h`, `bytecode_serializer.cpp` |
| Compiler pass `wam_compile_pass`: lowers `defrel` clauses into WAM instruction streams with register allocation + first-arg index tables. Emitted into the **same** `.etac` bytecode file as ordinary code; linker/loader unchanged. | new `semantics/passes/wam_compile.cpp`, `emitter.cpp`, `semantic_analyzer.cpp` |
| Calling convention bridge: a Scheme function may call a WAM-compiled predicate and vice versa via the ordinary `Call` / `Execute` opcodes — the callee prologue decides whether to read args from the Scheme value stack or from `X[]` registers, based on a bit in the function header. No tag-check on the hot path for same-kind calls. | `vm.cpp`, `semantic/emitter.cpp` |
| DAP / disassembler / LSP updates: show WAM stream with register names + resolved const table in the same views as regular Eta bytecode; no separate debugger panel. | `eta/dap/src/…`, `eta/lsp/src/…`, `disassembler.h` |
| Fuzz corpus: add logic/CLP fuzz targets to the *existing* fuzz harness (random clauses, random queries, invariant: `findall` ≡ `tabled findall` on ground queries; invariant: WAM-compiled predicate ≡ interpreted `defrel`). | `eta/fuzz/src/`, `eta/fuzz/corpus/` |
| Tests: direct WAM asm fixtures, LCO of `append`, benchmark suite entries `wam-append`, `wam-nrev`. | new `stdlib/tests/wam.test.eta`, new `bench/logic/` |

**Success criteria:**
- `nrev/1000` within 2× SWI-Prolog baseline on reference hw.
- Clause dispatch via first-arg index O(1) **using the same
  `FactTable` index** a pure-data query would use.
- LCO verified by unbounded `repeat/0`-style driver staying in
  constant stack.
- Zero regression on the existing Eta/Scheme benchmark suite — proving
  the WAM additions are truly additive.

---

## Cross-Cutting Concerns

| Concern | Tasks |
|---|---|
| **Docs** | Rewrite `docs/logic.md` per phase; split CLP into `docs/clp-fd.md` / `clp-b.md` / `clp-r.md`; new `docs/wam.md`. Update `docs/next-steps.md` checklist (§4). |
| **Examples** | Per phase: `examples/conde.eta`, `examples/dif-freeze.eta`, `examples/send-more-money.eta`, `examples/pigeonhole.eta`, `examples/portfolio-lp.eta`, `examples/tabling.eta`, `examples/wam-nrev.eta`. |
| **Benchmarks** | Add `bench/logic/{unify, nrev, queens, zebra, tabled-ancestor}` tracked in CI; extend §3.1 benchmark table. |
| **Debugger / Tracer** | Add `(trace-goal rel)` + DAP event `logicStep` showing choice-point stack & bindings; update `docs/aad.md`-style trace diagrams. |
| **LSP** | Extend `eta/lsp/` with hover on `defrel` showing indexed arg; completion for `clp:*` predicates. |
| **Fuzzing** | New `eta/fuzz/src/logic_fuzz.cpp`, `clp_fuzz.cpp`; invariant oracles (unify commutativity, trail round-trip, propagation idempotence). |
| **Performance metrics** | Add counters (choice-points created/restored, propagations fired, index hits/misses); expose via `(vm-stats)`. |

---

## Further Considerations

1. **CLP(B) backend** — pure bit-propagation vs full BDD.
   - Option A: propagation-only (fast, no counting).
   - Option B: optional BDD via cudd (counting + tautology).
   - Option C: SAT-bridge via MiniSat — heaviest, best completeness.
   - **Recommend A + B behind a feature flag.**
2. **Tabling depth** — full SLG with delaying / answer-subsumption is
   ~6 months of work.
   - Option A: variant tabling only (Phase 7 scope).
   - Option B: add subgoal abstraction later.
   - **Recommend A; revisit after WAM.**
3. **Register machine interop** — keep Scheme & WAM in one VM (shared
   dispatch) vs a satellite WAM VM invoked via FFI.
   - **Recommend shared dispatch** — reuses GC, nanboxing, DAP.
4. **Occurs-check default** — ISO Prolog defaults off. Eta currently
   defaults on.
   - Option A: keep on (safer).
   - Option B: flip to off with opt-in `unify-oc`.
   - **Recommend A until WAM; reconsider for perf parity in Phase 8.**
5. **Phase ordering flexibility** — Phase 5 (CLP(B)) and Phase 6
   (CLP(R)) are independent of each other and can swap or parallelise
   once Phase 3 lands.

---

*Draft plan — phases, APIs, and success criteria are open for review.
Update this document as each phase lands.*

