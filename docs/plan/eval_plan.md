# `eval` - Implementation Plan (Revised)

**Status:** Proposed  
**Priority:** P1  
**Effort:** ~6 days  
**Revision:** 4 (strict tag preservation, dedicated eval API, lexical-environment execution)

---

## Motivation

Eta is homoiconic (`quote`/`quasiquote` produce code data), so `eval` is required for the reverse direction.

This revision is driven by three hard requirements:

1. Preserve original error tags across `eval` (no `eval-error` umbrella tag).
2. Do not route through REPL wrapping helpers.
3. Evaluate in the current environment, including lexical bindings:

```scheme
(let ((x 10))
  (eval '(+ x 5)))   ;; => 15
```

---

## Public API

```scheme
(eval expr)
```

`expr` is any Eta value (typically a quoted list).

### Behavior

| `expr` | Result |
|---|---|
| Self-evaluating value (number, string, bool, etc.) | Returned unchanged |
| Symbol | Resolved in current lexical environment first, then globals/builtins |
| List / pair | Evaluated in current lexical environment |
| Other values | Returned unchanged |

Examples:

```scheme
(eval '(+ 1 2))                         ;; => 3
(let ((x 10)) (eval '(+ x 5)))         ;; => 15
(let ((x 10)) (let ((f (lambda (y) (eval '(+ x y))))) (f 7)))  ;; => 17
```

---

## Error Semantics (Strict)

`eval` must **not** normalize tags.

- User raises keep their original tag:

```scheme
(catch 'my-tag
  (eval '(raise 'my-tag 7)))           ;; catches 'my-tag
```

- VM/runtime errors keep their existing runtime tags (`runtime.type-error`, `runtime.invalid-arity`, etc.).
- Parse/expand/link/semantic failures from eval compilation are surfaced as normal runtime user errors (`runtime.user-error`) with diagnostic text; no eval-specific tag is introduced.

There is no `'eval-error` in this design.

---

## Architecture

### 1) Reentrant `execute()` remains required

`Driver` compilation still executes module init functions via `vm_.execute(...)`.  
Calling this from inside a running primitive still requires VM execution-state snapshot/restore.

We keep the `ExecutionSnapshot` + `saved_executions_` design, with one correction:

- Reentrancy guard is based on explicit execution depth, not only `current_func_ != nullptr`.
  - Add `execute_depth_` in `VM`.
  - Increment/decrement via RAII at `execute()` entry/exit.
  - `is_executing()` becomes `execute_depth_ > 0`.

This avoids false negatives in windows where `current_func_` is null but `execute()` has not fully exited yet.

### 2) Dedicated Driver eval API (no REPL wrapping)

Add a direct API in `Driver` for eval compilation/invocation, separate from `eval_string` and `wrap_repl_submission`.

Proposed internal types:

```cpp
struct EvalBinding {
    std::string name;
    runtime::nanbox::LispVal value;
};
```

```cpp
// Compile EXPR source into a callable closure with lexical params.
std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
compile_eval_lambda(std::string_view expr_source,
                    std::span<const EvalBinding> lexical_bindings);

// Invoke compiled eval closure with binding values.
std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
invoke_eval_lambda(runtime::nanbox::LispVal closure,
                   std::span<const EvalBinding> lexical_bindings);
```

`compile_eval_lambda` uses `run_source_impl` directly (not REPL wrapper).

### 3) Lexical environment capture

At eval call time, capture visible lexical names/values from the current VM frame:

- `vm.get_locals(0)` (highest precedence)
- `vm.get_upvalues(0)` (next precedence)

Filtering rules:

- Drop empty names.
- Drop synthetic placeholders (`%<n>`, `&<n>`).
- Deduplicate by name, keeping first (locals shadow upvalues).

### 4) How lexical evaluation is implemented

Given `expr_source` (from write-format of `expr`) and captured bindings:

1. Generate a temporary module and function:

```scheme
(module __eta.eval.N
  (define __eta_eval_fn_N
    (lambda (x y z ...)
      <expr_source>)))
```

2. Compile+execute that module through the dedicated Driver API.
3. Retrieve `__eta_eval_fn_N` as a closure.
4. Invoke closure with captured binding values using `vm_.call_value`.

This avoids serializing runtime values into source text. Lexical values are passed as real runtime values.

### 5) Error propagation path

- Compilation-stage failures in `compile_eval_lambda`:
  - convert diagnostics to `RuntimeError{VMError{RuntimeErrorCode::UserError, ...}}`
  - no `tag_override`.
- Runtime failures from `vm_.execute` and `vm_.call_value`:
  - propagate unchanged (`return std::unexpected(err);`)
  - preserves original runtime/user tags.

### 6) GC reachability for saved executions

Saved snapshots remain in `vm.saved_executions()` and are marked during GC root scan.

When walking snapshot state, mark the actual VM fields:

- `stack`, `temp_roots`, `pending_unwind_thunks`
- `frames`: `closure` and `extra`
- `catch_stack`: `tag` and `closure`
- `winding_stack`: `before`, `body`, `after`
- `pending_exception_transfer`: `closure`, `payload`
- `current_closure` in snapshot (if non-nil)

Factor live and snapshot root marking through shared helpers to avoid divergence.

### 7) Registration strategy

Keep existing patch-mode registration pattern:

1. Add `r("eval", 1, false);` at end of core section in `builtin_names.h`.
2. Add `eval` stub in `core_primitives.h` (must never run in practice).
3. Add `BuiltinEnvironment::overwrite_func(name, func)`.
4. In `Driver` constructor, after `verify_all_patched()`, overwrite `eval` with closure capturing `this`.

---

## Implementation Steps

### Step 1 - VM execution snapshot support

In `vm.h`/`vm.cpp`:

- Add `ExecutionSnapshot`.
- Add `saved_executions_`.
- Add `save_execution_state()` / `restore_execution_state()`.
- Add `execute_depth_` and update `is_executing()`.
- Add RAII `ExecutionScope` helper for compile-time reentrant `execute()` calls.

### Step 2 - Extend GC root walk

In VM GC root enumeration (`VM::collect_garbage` root callback):

- Mark all `saved_executions_` contents as described above.
- Refactor with helper lambdas for live/snapshot traversal parity.

### Step 3 - Builtin overwrite hook

In `builtin_env.h`:

```cpp
void overwrite_func(std::string_view name, PrimitiveFunc func);
```

Validate name exists; replace function pointer in-place.

### Step 4 - Register `eval` name and runtime stub

- `builtin_names.h`: append `r("eval", 1, false);` in the core block.
- `core_primitives.h`: add unreachable stub returning internal error text.

### Step 5 - Add Driver lexical-binding capture helper

In `driver.h`:

- Add helper to collect lexical bindings from `vm_.get_locals(0)` and `vm_.get_upvalues(0)`.
- Deduplicate and filter placeholders.

### Step 6 - Add dedicated Driver eval APIs

In `driver.h`:

- `compile_eval_lambda(...)`:
  - render temporary module source
  - call `run_source_impl(...)` directly
  - retrieve generated function binding as closure
  - on diagnostics failure, return `RuntimeErrorCode::UserError` with diagnostics text
- `invoke_eval_lambda(...)`:
  - collect argument values from `EvalBinding`s
  - call `vm_.call_value(...)`

No REPL wrapping helpers are used.

### Step 7 - Install real `eval` in Driver

After `verify_all_patched()`:

```cpp
builtins_.overwrite_func("eval", [this](std::span<const LispVal> args) -> std::expected<LispVal, RuntimeError> { ... });
```

Primitive flow:

1. Arity check.
2. Fast path self-evaluating values.
3. Capture lexical bindings.
4. Render expr to write-source.
5. Enter `ExecutionScope` and call `compile_eval_lambda`.
6. Exit scope (outer VM state restored).
7. Call `invoke_eval_lambda`.
8. Return value or propagate error unchanged.

### Step 8 - Eta tests (`stdlib/tests/eval.test.eta`)

Add/replace tests to cover:

- self-evaluating literals
- quoted-list evaluation
- lexical local lookup:
  - `(let ((x 10)) (eval '(+ x 5))) => 15`
- upvalue lookup:
  - eval inside lambda closes over outer `x`
- local-over-upvalue precedence
- nested eval
- strict tag preservation:
  - runtime tag preserved (e.g. `runtime.type-error`)
  - user tag preserved (`my-tag`)
- compile-path failure is `runtime.user-error` (no `eval-error`)
- reentrant recursion stress
- GC stress while reentrant compile path runs

### Step 9 - C++ unit tests

Add focused tests for:

- `save_execution_state`/`restore_execution_state` round-trip.
- snapshot LIFO nesting.
- `execute_depth_` behavior around `execute()`.
- GC roots include saved snapshots.
- Driver eval API compiles via direct path (not REPL wrappers).

### Step 10 - Docs and tooling

- `hover_at` docstring for `eval` must not mention `eval-error`.
- Update reference docs to describe strict tag preservation and lexical-environment behavior.
- Add editor snippet for `(eval expr)` if desired.

---

## File Change Summary

| File | Change |
|---|---|
| `eta/core/src/eta/runtime/vm/vm.h` | `ExecutionSnapshot`, `saved_executions_`, `execute_depth_`, save/restore API |
| `eta/core/src/eta/runtime/vm/vm.cpp` | save/restore implementation, `is_executing` depth semantics, GC root walk for snapshots |
| `eta/core/src/eta/runtime/builtin_env.h` | `overwrite_func(name, func)` |
| `eta/core/src/eta/runtime/builtin_names.h` | add `eval` builtin name metadata |
| `eta/core/src/eta/runtime/core_primitives.h` | add eval stub placeholder |
| `eta/session/src/eta/session/driver.h` | lexical binding capture, `compile_eval_lambda`, `invoke_eval_lambda`, real eval install |
| `stdlib/tests/eval.test.eta` | revised tests for lexical env + strict tag preservation |
| `eta/test/src/*` | C++ unit tests for snapshot/depth/eval API behavior |
| `docs/guide/reference/eval.md` | reference page |
| `docs/guide/reference/README.md` | index entry |
| `editors/vscode/snippets/eta.json` | optional eval snippet |

---

## Acceptance Criteria

1. `(let ((x 10)) (eval '(+ x 5)))` returns `15`.
2. Eval inside lambda sees upvalues from lexical scope.
3. Runtime errors inside eval preserve existing runtime tags.
4. `(raise 'my-tag ...)` inside eval is catchable as `'my-tag` (unchanged).
5. No code path emits or relies on `'eval-error`.
6. Eval compilation path does not use REPL wrapping helpers.
7. Reentrant eval from inside running VM frames is safe (no frame/stack corruption).
8. GC during reentrant compile/eval does not free snapshot-reachable values.
9. `.etac` builtin-count mismatch behavior remains correct after adding `eval`.
10. Hover/docs match implemented semantics.

---

## Non-goals (This Iteration)

- `(eval expr env)` API with first-class environment objects.
- Sandboxed eval variants.
- Global performance optimizations for repeated eval compilation (cache/JIT).
- Full write-through mutation of outer lexical slots via nested `set!` inside eval.
  - v1 guarantees lexical name resolution for reads.

---

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Snapshot roots missed -> use-after-free | Explicit snapshot root tests + shared live/snapshot marking helpers |
| Reentrancy guard misses `execute` window | `execute_depth_` instead of `current_func_`-only checks |
| Lexical placeholder names leak into params | filter `%<n>` / `&<n>` and empty names before lambda generation |
| Compilation diagnostics lose context | propagate full `diagnostics_to_string()` in `runtime.user-error` |
| Repeated eval compilation overhead | acceptable for v1; profile and add cache as follow-up if needed |

