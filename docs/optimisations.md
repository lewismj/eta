# Eta Optimisation Plan

This plan inventories existing IR-level optimisation in Eta and lists the
highest-ROI improvements available in the compiler, the bytecode emitter,
and the VM. Items are ordered by expected payoff per unit of work.

## Current state (April 2026)

Three passes exist under
[`eta/core/src/eta/semantics/passes/`](../eta/core/src/eta/semantics/passes):

- **`ConstantFolding`** - folds binary `+ - * /` only when both operands
  are `Const` numeric literals and the callee resolves to a global named
  `+`/`-`/`*`/`/`. No identity simplification, no n-ary, no fixpoint, no
  comparisons or boolean ops.
- **`DeadCodeElimination`** - removes pure (`Const`/`Var`/`Quote`)
  non-tail expressions inside a `Begin`; collapses single-element
  `Begin`s.
- **`PrimitiveSpecialisation`** - lowers eligible builtin calls to
  dedicated primitive IR nodes that emit VM opcodes
  (`Add/Sub/Mul/Div/Eq/Cons/Car/Cdr`).

The optimisation pipeline is wired and active under `etac -O`:

- `main_etac.cpp` registers `ConstantFolding`,
  `PrimitiveSpecialisation`, and `DeadCodeElimination` when
  `-O` / `--optimize` is set.
- `Driver::run_source_impl` executes
  `optimization_pipeline_.run_all(sem_mods)` between semantic analysis and
  bytecode emission.
- With `-O0` (or default), no passes are registered.

Important correctness caveat: current `ConstantFolding` identifies
arithmetic by global binding name only. If user code rebinds
`+`/`-`/`*`/`/`, `-O` can fold using builtin semantics even when runtime
call target differs.

For calls proven to target immutable builtins with supported arity,
`etac -O` now emits dedicated opcodes (`Add/Sub/Mul/Div/Eq/Cons/Car/Cdr`)
instead of generic `LoadGlobal` + `Call`.

Calls that are not proven safe (shadowed names, unsupported arity, or
non-builtin/global targets) still take the generic call path.

## Shortlist (best ROI first)

1. Self-recursive tail call -> backward `Jump` in the emitter.
2. Drop the `shared_lock` on `BytecodeFunctionRegistry::get` and pass
   `std::span<LispVal>` to primitives (zero-copy).
3. Harden and extend `ConstantFolding` (builtin-identity checks, n-ary,
   comparisons, identities, fixpoint loop).
4. Constant + copy propagation through `let`-bindings.
5. Extend primitive specialisation coverage (more builtins/arity forms)
   where semantic proof is straightforward.

Items 1-2 alone should produce a multi-x speedup on arithmetic-heavy
workloads (AAD primal, Monte Carlo path generation in pure Eta) without
introducing any new opcodes or new IR.

---

## Detailed items

### 1. Primitive call specialisation status and next extensions

**Where**: implemented in
`eta/core/src/eta/semantics/passes/primitive_specialisation.h`,
consumed by the existing emitter.

[`bytecode.h`](../eta/core/src/eta/runtime/vm/bytecode.h) defines
dedicated opcodes `Add`, `Sub`, `Mul`, `Div`, `Eq`, `Cons`, `Car`,
`Cdr`. The pass currently lowers calls when:

- the callee is a `Var` with `Address::Global`,
- the target global slot is a known immutable builtin,
- and the arity matches,

to `PrimitiveCall`, which the emitter lowers to the matching opcode.

Remaining work in this area:

- extend coverage to additional builtins with fixed arity where opcode
  equivalents exist or can be added cheaply,
- evaluate whether selected variadic builtin forms should lower to
  specialised opcodes after argument normalization.

### 2. Harden and extend constant folding

- Make folding semantic-safe under shadowing:
  - fold only when callee identity is a known immutable builtin, not
    name-match alone.
  - add regression tests that redefine `+`/`-`/`*`/`/` and verify `-O`
    does not fold incorrectly.
- Extend `ConstantFolding` to:
  - n-ary `+ - * /` (runtime `+` is variadic; folder currently handles 2 args).
  - comparisons (`= < > <= >=`) and `not` (and/or are lowered to `if`).
  - algebraic identities: `(* x 1)`, `(+ x 0)`, `(- x 0)`,
    `(* x 0) -> 0` only when `x` is pure, `(if #t a b) -> a`,
    `(if #f a b) -> b`.
  - iterate to a fixpoint, since folding can expose new opportunities.

### 3. Constant / copy propagation through `let` (i.e. `Begin` + `Set{Local}`)

`Let` is desugared into a `Begin` that starts with `Set{Local}`
initialisers (see letrec pattern detection in `emit_begin`).
For a non-mutated local whose initialiser is a `Const`, `Var`, or
`Quote`, replace each `LoadLocal slot` with the initialiser, then DCE
drops the `Set`. Combined with item 2 this enables folding through
`(let ((x 2) (y 3)) (* x y))` style code that macro expansion produces
everywhere. `BindingInfo::mutable_flag` already records mutability.

### 4. Inlining of small lambdas / direct-recursive calls

Two cheap forms:

- **Beta-reduce known-callee, single-use lambdas**:
  `((lambda (x y) body) a b)` -> substitute. Eta uses this pattern
  extensively for `let`/`when`/macro expansions.
- **Self-recursive tail calls**: when the callee resolves to the
  enclosing `Lambda` and the call is in tail position, emit a backward
  `Jump` instead of `TailCall`. Today `TailCall` still does full
  dispatch and frame movement work each iteration.

### 5. Global resolution caching ("ICs lite")

`LoadGlobal` is already O(1). But every global call still does callee
object checks and function resolution from the registry, currently with a
`shared_lock` on the read path.

Options:

- Eliminate lock on the read path. Functions are append-only; slots are
  stable with `std::deque`.
- Add a fused call opcode (`CallGlobal` / `CallDirect`) that resolves once.

### 6. Stop reallocating `args` per primitive call

`dispatch_callee` builds a `std::vector<LispVal> args` for every
primitive call and primitive code usually iterates once. Either:

- pass `std::span<LispVal>` from the VM stack (zero-copy), or
- pre-allocate a thread-local scratch buffer and reuse it.

This is a runtime change, not an IR pass, but it is low-hanging VM work.

### 7. Bytecode peephole

After emission, a quick scan can collapse:

- `LoadConst Nil; Pop` (from `set!`) when result is unused.
- `Jump x; Jump y` chains.
- dead jump patterns around `if` / `begin`.
- trailing structural `Pop` patterns before terminal `LoadConst Nil; Return`.

### 8. Closure / upvalue elimination for non-escaping lambdas

If a `Lambda` is only used as the immediate callee of a `Call` (no
escape), it does not need `MakeClosure` / heap allocation - its body can
be inlined or compiled as a direct sibling function with shared frame
access.

### 9. AAD / numerical hot-path specific

Given the XVA plan and existing AD tape integration:

- **Fixnum/flonum-specialised arithmetic opcodes**: when both operands
  are statically flonum, bypass generic numeric classification and
  overflow ladders.
- **Tape-record fast path**: avoid per-op tape tag tests when no tape is active.

### 10. Dead-store / dead-local elimination

DCE today only touches pure expressions inside `Begin`. It does not
detect:

- `Set{Local}` where local is never loaded again (dead store),
- local slots written but never read (dead binding; shrinks frame size).

A liveness sweep over IR can implement both.

### 11. Constant lifting and quote interning at emit time

`emit_const` already caches string constants per function. `Quote` data
and numeric constants are not interned across the constant pool, so
repeated quoted literals still duplicate work and storage.

A hashed datum pool keyed by value would shrink bytecode and improve
constant-pool locality.

### 12. Type-driven specialisation (longer term)

There is no static type system, but a flow-sensitive pass could prove
"this var is always closure arity N" or "always fixnum" along a path and
emit specialised opcodes.

---

## Suggested rollout order

1. Self-recursive tail-call -> jump (emitter only, no new opcode). [done]
2. Drop the `shared_lock` on `BytecodeFunctionRegistry::get`; pass
   `span<const LispVal>` to primitives. [done]
3. Harden `ConstantFolding` correctness under builtin shadowing.
4. Extend folding (n-ary/comparisons/identities/fixpoint) plus
   constant/copy propagation through let-bindings.
5. Bytecode peephole.
6. Dead-store elimination + frame shrinking.
7. AAD-specific flonum fast path + active-tape flag.
8. Closure elimination for non-escaping lambdas.
9. Constant pool / quote interning.
10. Flow-sensitive type specialisation.

Each step is independently shippable and benchmarkable.

### Step 1 implementation notes

- Tail-position calls that target the current closure's self-capture upvalue
  are now lowered in the emitter to:
  - evaluate arguments left-to-right,
  - `StoreLocal` each parameter slot in reverse order,
  - emit a backward `Jump` to the lambda entry.
- This keeps self-recursive loops in constant stack space without introducing
  any new VM opcode.
- The optimisation is currently enabled for unary self recursion only; wider
  arity handling remains disabled until the multi-argument path is proven
  stable under the full stdlib workload.
- Non-self tail calls (including mutual recursion) continue to emit
  `TailCall`.

### Step 2 implementation notes

- `BytecodeFunctionRegistry::get` now uses a lock-free read path.
- Primitive dispatch now forwards arguments as `std::span<const LispVal>`
  over the VM stack instead of materialising a temporary `std::vector`.
- VM primitive dispatch checks for stack underflow before slicing arguments
  and pops arguments from the VM stack after primitive evaluation.
- No behavioural regressions were observed in `eta_core_test` and
  `eta_stdlib_tests` after this change.
