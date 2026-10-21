# Numerics Performance Plan

## Goal

Make the existing `Add`/`Sub`/`Mul`/`Div` opcodes the default path for
arithmetic. Add a single `NumCmp` opcode for numeric comparison. Move all
numeric semantics into one shared kernel, so the VM opcodes and the builtin
primitives can't drift apart.

## Background

- `Add`/`Sub`/`Mul`/`Div`/`Eq` opcodes exist (`core/src/eta/runtime/vm/bytecode.h`),
  but only `PrimitiveSpecialisation` emits them. The only non-test code that
  registers that pass is `tools/compiler/src/eta/compiler/main_etac.cpp`, and only
  under `-O`. As a result, `etai`, the REPL, Jupyter and `etac` without `-O` run all
  arithmetic as generic primitive calls.
- A generic primitive call (`VM::dispatch_callee`) costs:
  - 3 `concurrent_flat_map` lookups: Closure, then Continuation, then Primitive
  - 6 string comparisons in `builtin_is_blocking`
  - a profiler scope
  - a `std::function` call
  - `classify_numeric` per argument
- Binary `=` is lowered to `OpCode::Eq` (`values_eqv`), which gives wrong results:
  - `(= 1 1.0)` returns `#f`
  - `(= 0.0 -0.0)` returns `#f`
  - `(= nan nan)` returns `#t`
  - `(= 'a 'a)` returns `#t` with no type error
- The VM and the primitives each have their own copy of numeric semantics.
  The `=` bug is what happens when those copies diverge.

## Phase 0: Baseline (you run this)

- Three micro-benchmarks:
  - recursive `fib 30`
  - an integer loop summing to 10M with `(< i n)` as the condition
  - the same loop over flonums
- Run each with `etai` and with `etac -O`, to get four numbers to compare later:
  unoptimised before/after and `-O` before/after.

## Phase 1: Cheaper primitive dispatch (no behaviour change)

- **`types/primitive.h`:** add `bool is_blocking{}` to `Primitive`.
- **`builtin_env.h:148` and `extension_env.h:67`:** where `debug_name` is set, also set
  `is_blocking = builtin_is_blocking(spec.name)`.
- **`vm/vm.cpp:739`:** replace `builtin_is_blocking(prim->debug_name)` with `prim->is_blocking`.
- **`VM::dispatch_callee`:** do one heap lookup for the callee, then `switch` on its kind
  (Closure, Continuation, Primitive), instead of three `try_get_as` calls.

## Phase 2: One numeric kernel

- **New `runtime/numeric_ops.h`** (inline, header-only) containing:
  - `enum class ArithOp { Add, Sub, Mul, Div }`
  - `enum class CmpOp : uint8_t { NumEq, Lt, Gt, Le, Ge }`
  - `arith(Heap&, ArithOp, LispVal a, LispVal b) -> std::expected<LispVal, RuntimeError>`,
    with fast paths first:
    - Two immediate fixnums: Add and Sub can't overflow `int64`, so compute,
      range-check, and fall back to a heap fixnum. Mul uses `mul_overflow`.
    - Two raw doubles: compute directly, then `make_flonum`, which canonicalises NaN.
    - Anything else: the general path (`classify_numeric`, promote on overflow,
      exact or inexact `Div`), moved unchanged out of `do_binary_arithmetic`.
      It also adds a guard for `INT64_MIN / -1`.
  - `compare(Heap&, CmpOp, LispVal a, LispVal b) -> std::expected<bool, RuntimeError>`:
    - fast paths for fixnum/fixnum and double/double, then a mixed general path
    - IEEE rules: NaN compares false, and `0.0 == -0.0`
    - a non-number gives a `TypeError`
  - A fast inline type check that runs before the visitor-based `classify_numeric`.
    The visitor stays for its other callers.
- **`VM::do_binary_arithmetic`:** keep the TapeRef block as it is, and replace
  everything after it with a call to `numeric_ops::arith`.
- **Primitives `+ - * /`** (`core_primitives_arithmetic.cpp`): keep the TapeRef branches.
  The plain-number path becomes a left fold using `numeric_ops::arith`. The
  results are the same, including when overflow promotes to a double.
- **`make_comparison_primitive`** (`core_primitives_arithmetic_helpers.h`): keep the
  TapeRef branch. The plain-number path calls `numeric_ops::compare`.
- **Move the TapeRef comparison logic** (strict-policy error, comparing primal
  values) into a helper that both the primitive and the new opcode can call.

## Phase 3: `NumCmp` opcode, and the `=` fix

- **`vm/bytecode.h`:** add `NumCmp` at the **end** of `OpCode`, so existing opcode
  numbers don't move. `arg` holds the `CmpOp`. Add `"NumCmp"` to the name table.
- **`vm/bytecode_serializer.h`:** bump `FORMAT_VERSION` from 6 to 7, and keep loading v6 files.
- **Verifier** (`bytecode_serializer.cpp`, `verify_function`): reject any `NumCmp`
  whose `arg` is above 4.
- **`vm/disassembler.h`:** print `NumCmp <` and so on.
- **`vm/vm.cpp`:**
  - `stack_operands`: `NumCmp` takes 2 operands.
  - New dispatch case: pop `b`, then `a`. If either is a TapeRef, use the shared
    tape-compare helper. Otherwise call `numeric_ops::compare` and push `True` or `False`.
- **`semantics/core_ir.h`:** add `NumEq`, `Lt`, `Gt`, `Le`, `Ge` to `PrimitiveKind`.
- **`semantics/emitter.cpp`:** map those kinds to `NumCmp` with the right `arg`.
  `Add`, `Sub`, `Mul`, `Div`, `Eq`, `Cons`, `Car` and `Cdr` stay as they are.
- **`passes/primitive_specialisation.h`:**
  - `"="` now lowers to `NumEq`, not `Eq`.
  - Add `"<"`, `">"`, `"<="` and `">="`.
  - The `Eq` opcode stays, used only for `eqv?` semantics.
- **Added during implementation:**
  - **Catchable opcode errors:** errors from `Add`/`Sub`/`Mul`/`Div`/`NumCmp` now go through
    `do_runtime_error`, so `catch` handlers see them, just as they see errors from the
    equivalent primitive calls. Before, a type error in a specialised opcode aborted
    execution. The messages now match the primitives' (`+: argument is not a number`,
    `/: division by zero`). This was needed before Phase 4 turns specialisation on everywhere.
  - **`ConstantFolding`:** it used its own unchecked arithmetic (signed-overflow UB, no
    promotion to double). It now folds through `numeric_ops::arith`, so folded and
    runtime results agree.
  - **Format versions:** `FORMAT_VERSION_V6` was added for the extension-environment
    header field, so v6 files still load after `FORMAT_VERSION` moved to 7.

## Phase 4: Use the existing opcodes by default

- **`PrimitiveSpecialisation`:** lower `+ - * /` calls with 3 or more arguments to a
  left-associated chain of the existing binary opcodes. Calls with 0 or 1 argument
  (`(+)`, `(- x)`, `(/ x)`) and comparisons with 3 or more arguments stay as
  primitive calls. Chaining those comparisons would need a temporary to avoid
  evaluating `b` twice.
- **`session/driver.cpp`:** the `Driver` constructor always adds `PrimitiveSpecialisation`
  to `optimization_pipeline_`. That makes `etai`, the REPL, Jupyter and `etac`
  without `-O` all emit `Add`/`Sub`/`Mul`/`Div`/`NumCmp`.
- **`main_etac.cpp:99-104`:** `-O` adds only `ConstantFolding` and `DeadCodeElimination`,
  so the specialisation pass isn't registered twice. Pass order needs checking:
  today it runs ConstantFolding → PrimitiveSpecialisation → DCE.
- **Side effect:** specialised `+` and `<` no longer show up as primitive calls in the
  profiler. That's expected; note it in the commit message.

## Phase 5: Small correctness fixes (separate commit)

- `remainder` on flonums: switch from `std::remainder` to `std::fmod`.
- `modulo` on flonums: use `fmod`, then fix the sign to match the divisor.
- Unary `-` and `abs` on `INT64_MIN`: promote to a double instead of hitting
  undefined behaviour.

## Phase 6: Tests

- **`=` semantics, through both the opcode and the primitive:**
  - `(= 1 1.0)` returns `#t`
  - `(= 0.0 -0.0)` returns `#t`
  - `(= nan nan)` returns `#f`
  - `(= 'a 'a)` gives a `TypeError`
- **Comparisons:** `< > <= >=` with mixed fixnums and flonums, and with heap fixnums.
- **Arithmetic:**
  - overflow promotes to a double
  - `INT64_MIN / -1`
  - a 3-argument `(+ a b c)` lowers to a chain and gives the same result as the primitive
- **TapeRef:** comparisons through `NumCmp`, including the strict-policy error.
- **Bytecode:** a serializer round trip for `NumCmp`, the verifier rejecting a bad
  `arg`, and disassembler output.
- **Existing tests to update:** `optimization_tests.cpp`, for any that expect `=` → `Eq`
  or that expect no specialisation without `-O`.

## Deferred (not in this plan)

- A `CallBuiltin <slot> argc` instruction, so calls to any builtin skip the heap lookup.
- Reworking how every heap object access goes through a locked hash-map lookup.
