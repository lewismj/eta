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

## Phase 3 — Attributed Variables + Wakeup Hooks

**Goal:** give constraint libraries a first-class hook into
unification — the foundation for real CLP and for tabling.

| Work item | Touches |
|---|---|
| New heap kind `AttrVar { optional<LispVal> binding; flat_map<SymbolId, LispVal> attrs; }` (extends `LogicVar`, or adds sibling kind). | new `types/attr_var.h`, `memory/heap.h` |
| Primitives: `(put-attr v module val)`, `(get-attr v module)`, `(del-attr v module)`, `(attr-var? v)`. Registered per-module hook `attr-unify-hook` invoked by `VM::unify` when an attributed var is bound. | `core_primitives.h`, `vm.cpp` |
| Wakeup queue on VM: goals scheduled by hooks execute before `unify` returns; overflow → fail. Trailed as a new `TrailEntry::Kind::Attr` from Phase 1. | `vm.h`, `vm.cpp` |
| Port `ConstraintStore` Z/FD domains to live as attributes on attributed vars (instead of external `ObjectId` map) — makes domains survive var–var aliasing. | `clp/constraint_store.h`, `stdlib/std/clp.eta` |
| Eta-level `(freeze v goal)` — delayed goal until `v` bound; built on attributes. Also `(dif x y)` for structural disequality. | new `stdlib/std/freeze.eta` |
| Tests: `attr-vars`, `freeze`, `dif`, unification of two constrained vars intersects domains. | new `stdlib/tests/attrvar.test.eta` |

**Success criteria:** unifying `x ∈ [1,10]` with `y ∈ [5,20]` yields a
single var with domain `[5,10]`; `freeze` fires exactly once; no
regressions in CLP tests.

---

## Phase 4 — CLP(FD) with AC-3 Propagation & Labeling Strategies

**Goal:** real finite-domain solver built on Phase 3 attributes.

| Work item | Touches |
|---|---|
| `PropagationQueue` in VM: FIFO of `(propagator_id, var_id)`; run to fixpoint after each domain narrowing. | new `clp/propagator.h`, `vm.h`, `vm.cpp` |
| Native propagators (C++): `fd_eq`, `fd_neq`, `fd_lt`, `fd_le`, `fd_plus`, `fd_times`, `fd_abs`, `fd_min`, `fd_max`, `fd_all_different` (Régin-lite via value-graph matching), `fd_element`, `fd_sum`, `fd_scalar_product`. | new `clp/propagators.cpp` |
| Bit-set FD domain representation (`roaring` or `std::bitset` chunks) replacing `std::vector<int64_t>` for compact finite sets; interval+holes hybrid. | `clp/domain.h` |
| New Eta API in `stdlib/std/clp.eta`: `clp:!=`, `clp:*`, `clp:abs`, `clp:sum`, `clp:scalar-product`, `clp:element`, `clp:all-different/2` (with `first-fail` / `ff` / `ffc` / `most-constrained` variable-selection strategies), `clp:minimize`, `clp:maximize`, `clp:labeling` accepting option list. | `stdlib/std/clp.eta` |
| Branch-and-bound optimizer: `(clp:minimize cost goal)` using incumbent + `clp:<=` on cost. | `stdlib/std/clp.eta`, `vm.cpp` |
| Tests: AC-3 narrowing trace, N-queens @ N=12 under 1s, `send+more=money`, job-shop micro-benchmark, branch-and-bound correctness. | new `stdlib/tests/clpfd.test.eta`, `examples/send-more-money.eta` |

**Success criteria:** N-queens(20) ≤ 2 s on reference hw;
`send+more=money` solved with a single answer; propagation idempotent
and monotone (property test).

---

## Phase 5 — CLP(B) Boolean Constraints

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

