# `eval` — Implementation Plan

**Status:** Proposed · **Priority:** P1 · **Effort:** ~3 days

---

## Motivation

Eta is a homoiconic Lisp.  `quote` and `quasiquote` let programs treat code as
data; without `eval` the reverse direction is missing.  Concretely:

- `do:simplify` (causal M4) returns an estimand AST.  To *execute* it you
  currently have to hand-interpret the tree.  With `eval`:
  ```scheme
  (eval (do:simplify (id g '(y) '(x))))
  ```
- The symbolic differentiator in `causal_demo.eta` builds an expression tree
  that `eval` can immediately run.
- Programmatic code generation in Jupyter notebooks (macro-expanded forms,
  generate-and-run patterns) becomes natural.
- `define-dag` (M11) and similar definition macros could emit code and `eval` it
  at runtime without a source-string round trip.

---

## Design

### v1 — single-argument form

```scheme
(eval expr)
```

`expr` is any Eta value (typically a quoted list).  It is converted to its
`write`-representation string, then compiled and executed through the full
pipeline in the current session's environment (same module state, same globals,
same accumulated forms).  Returns the result of the last expression.

```scheme
(eval '(+ 1 2))                             ;; => 3
(eval (list '* 3 4))                        ;; => 12
(eval `(define x ,(+ 1 2)))                 ;; defines x = 3 in the session
(eval (list 'if #t ''yes ''no))             ;; => yes
```

Errors in the compiled/executed expression propagate as normal Eta exceptions:

```scheme
(catch 'eval-error
  (eval '(/ 1 0)))
```

### v2 — environment argument (deferred)

R7RS §6.12 specifies `(eval expr environment)`.  Defer until environment
objects are first-class.  Placeholder API:

```scheme
(eval expr (interaction-environment))   ;; current session — same as v1 today
(eval expr (the-environment))           ;; capture environment at call site
(eval expr (make-environment 'std.causal 'std.io))  ;; fresh env with imports
```

### `compile` (deferred, paired with v2)

```scheme
(compile '(lambda (x) (* x x)))         ;; => closure, no re-compilation later
```

Useful for hot loops that construct closures at runtime.  Depends on
exposing the compilation pipeline as a first-class value.

---

## Architecture

### Why it's almost free

`Driver::eval_string` (line 408 of `driver.h`) already does exactly what
`eval` needs: it splits top-level forms, wraps them into an internal module,
runs `run_source`, and returns a formatted string.  The existing REPL and
Jupyter kernel both call this function on every cell submission.

The primitive needs to:
1. Convert its `LispVal` argument to a source string via `format_value(..., FormatMode::Write)`.
2. Call `Driver::run_source(src, &result)` with a result out-parameter.
3. Box and return the result.

### Why `eval` cannot be registered through `all_primitives.h`

Standard primitives receive `(heap, intern_table, vm)` but **not** a pointer
to the `Driver`.  `eval` needs the full compilation pipeline:
`Lexer → Parser → Expander → ModuleLinker → SemanticAnalyzer →
OptimizationPipeline → Emitter → VM::execute`.  This all lives on `Driver`.

The correct pattern — already used for NNG primitives that need `ProcessManager`,
`registry_`, and `vm_.globals()` — is to **register `eval` directly inside the
`Driver` constructor after `builtins_.verify_all_patched()`**, capturing `this`:

```cpp
// In Driver constructor, after verify_all_patched():

builtins_.register_extra("eval", 1, false,
    [this](std::span<const runtime::nanbox::LispVal> args)
        -> std::expected<runtime::nanbox::LispVal,
                         runtime::error::RuntimeError>
    {
        using runtime::nanbox::LispVal, runtime::nanbox::Nil;
        if (args.size() != 1)
            return std::unexpected(runtime::error::make_arity_error("eval", 1, args.size()));

        // Convert the LispVal to its write representation.
        std::string src = format_value(args[0], runtime::FormatMode::Write);

        // Feed through the full compile+execute pipeline.
        LispVal result = Nil;
        if (!run_source(src, &result)) {
            // Diagnostics were emitted; surface as a tagged runtime error.
            return std::unexpected(
                runtime::error::make_user_error("eval-error",
                    runtime::error::format_diagnostics(diag_engine_,
                                                       file_resolver())));
        }
        return result;
    });
```

`register_extra` is a new method on `BuiltinEnvironment` (see §Implementation
steps below) that appends *after* the pre-registered block so the global slot
numbering stays consistent.

Alternatively — the simpler approach that requires **zero changes to
`BuiltinEnvironment`** — register `eval` via the same `builtins_.patch()`
path as every other primitive, by:

1. Adding `r("eval", 1, false);` at the end of `builtin_names.h`.
2. Adding a registration call at the end of `register_all_primitives`, but
   passing a `Driver*` pointer.

This requires adding `Driver*` to `register_all_primitives`'s signature, which
is a larger change.  The `register_extra` approach is preferable.

### Thread safety

Each `spawn-thread` worker runs inside its own child `Driver` instance (see
`install_actor_worker_factories` in `driver.h`).  `eval` called from a worker
thread compiles and executes against *that child's* environment, not the
parent's.  This is correct: worker threads have isolated state by design.
Document clearly to avoid confusion.

---

## Implementation steps

### Step 1 — `BuiltinEnvironment::register_extra`

Add a method that appends a builtin slot *after* `verify_all_patched()` has
been called, bypassing the strict sequential patch-cursor protocol:

```cpp
// In builtin_env.h, inside class BuiltinEnvironment:

/// Register a late-bound builtin not tracked by builtin_names.h.
/// Only valid after verify_all_patched().
/// The slot is appended; its index is builtins_.size() - 1.
void register_extra(const char* name, uint32_t arity, bool has_rest,
                    PrimitiveFunc func)
{
    BuiltinSpec spec;
    spec.name     = name;
    spec.arity    = arity;
    spec.has_rest = has_rest;
    spec.func     = std::move(func);
    specs_.push_back(std::move(spec));
}
```

This requires no change to `builtin_names.h` or the numeric slot ordering used
by the semantic analyser, because `eval` is resolved by *name* at analysis time
(it is a known symbol in the global scope, just like any other builtin).

> **Important:** The `builtin_count()` method returns `builtins_.specs().size()`
> and is embedded in `.etac` files for mismatch detection.  Adding `eval` via
> `register_extra` increments this count between compiler and interpreter
> versions.  Include `eval` in `builtin_names.h` from day one to keep
> `builtin_count` stable.  Use the following approach instead of
> `register_extra`:

**Preferred approach:** Add `r("eval", 1, false);` to `builtin_names.h` (at
the end of the core-primitives block), add a corresponding forwarding entry to
`register_all_primitives` that plants a stub, then **overwrite the slot** after
the Driver captures `this`:

```cpp
// builtin_names.h — at end of core section:
r("eval", 1, false);

// core_primitives.h — stub (never called; overwritten by Driver):
r("eval", [](auto) -> ... { return Err(runtime_error("eval not installed")); });

// Driver constructor, after verify_all_patched():
auto eval_slot = builtins_.lookup("eval");
assert(eval_slot.has_value());
builtins_.specs()[*eval_slot].func = [this](std::span<const LispVal> args) { ... };
```

This keeps `builtin_count` deterministic and the `.etac` format stable.

### Step 2 — `BuiltinEnvironment::overwrite_func`

Expose the slot-overwrite pattern as an explicit method:

```cpp
// In builtin_env.h:
void overwrite_func(std::string_view name, PrimitiveFunc func) {
    auto idx = lookup(name);
    assert(idx && "overwrite_func: unknown builtin");
    specs_[*idx].func = std::move(func);
}
```

### Step 3 — `driver.h` wiring

After `builtins_.verify_all_patched()`:

```cpp
builtins_.overwrite_func("eval",
    [this](std::span<const runtime::nanbox::LispVal> args)
        -> std::expected<runtime::nanbox::LispVal,
                         runtime::error::RuntimeError>
    {
        using namespace runtime::nanbox;
        if (args.size() != 1)
            return std::unexpected(
                runtime::error::make_arity_error("eval", 1, args.size()));

        std::string src = format_value(args[0], runtime::FormatMode::Write);

        LispVal result = Nil;
        if (!run_source(src, &result)) {
            std::ostringstream oss;
            diag_engine_.print_all(oss, /*use_color=*/false, file_resolver());
            return std::unexpected(
                runtime::error::UserError{"eval-error", oss.str()});
        }
        return result;
    });
```

### Step 4 — stdlib wrapper in `std.core`

`eval` is already a known identifier in the completion list
(`completions_at`).  Export it from `std.core` so `(import std.core)` makes
it available:

```scheme
;; stdlib/std/core.eta — no wrapper needed; eval is a builtin.
;; Just add it to the exports list + docstring:
;;   (eval expr) — compile and evaluate EXPR in the current session environment.
;;                 EXPR is typically a quoted list or any self-evaluating value.
;;                 Returns the result of the last expression in EXPR.
;;                 Raises 'eval-error on compile or runtime failure.
```

### Step 5 — tests (`stdlib/tests/eval.test.eta`)

```scheme
(module eval-tests
  (import std.core)
  (import std.test)
  (begin

    (test "eval: self-evaluating literal"
      (assert-equal (eval 42) 42))

    (test "eval: quoted list → arithmetic"
      (assert-equal (eval '(+ 1 2)) 3))

    (test "eval: constructed call"
      (assert-equal (eval (list '* 3 4)) 12))

    (test "eval: quasiquoted expression"
      (let ((n 7))
        (assert-equal (eval `(* ,n ,n)) 49)))

    (test "eval: define persists in session"
      (eval '(define %eval-test-x 99))
      (assert-equal %eval-test-x 99))

    (test "eval: nested eval"
      (assert-equal (eval '(eval '(+ 10 5))) 15))

    (test "eval: string is not treated as source — it evals to itself"
      (assert-equal (eval "hello") "hello"))

    (test "eval: error in compiled expression raises eval-error"
      (let ((caught #f))
        (catch 'eval-error
          (eval '(error 'deliberate "test")))
        (set! caught #t)
        (assert caught)))

    (test "eval: causal estimand AST round-trip"
      ;; Build a trivial estimand AST by hand and eval it.
      (let* ((ast '(begin (define %et-result (* 6 7)) %et-result))
             (r   (eval ast)))
        (assert-equal r 42)))

  ))
```

### Step 6 — docs (`docs/guide/reference/eval.md`)

New reference page covering:
- One-line description and signature.
- The environment model (current REPL/program session).
- Performance note (full compiler pipeline per call; use `compile` for hot loops — deferred).
- Thread-safety note (child actors evaluate in their own environment).
- Error handling (`catch 'eval-error`).
- Worked examples: code generation, estimand execution, metaprogramming.
- v2 roadmap (environment argument, `compile`).

### Step 7 — completions and hover

`eval` already appears in the keyword list in `completions_at`
(`driver.h` line ~479).  Add it to `keyword_docs` in `hover_at` with a
proper docstring:

```cpp
{"eval", "**eval**  -  Compile and evaluate an expression in the current environment.\n\n"
         "`(eval expr)`\n\n"
         "Returns the result of executing EXPR.  EXPR is typically a quoted list.\n"
         "Raises `eval-error` on compile or runtime failure."},
```

---

## File change summary

| File | Change |
|---|---|
| `eta/core/src/eta/runtime/builtin_names.h` | Add `r("eval", 1, false);` at end of core block |
| `eta/core/src/eta/runtime/core_primitives.h` | Add stub registration for `eval` (overwritten by Driver) |
| `eta/core/src/eta/runtime/builtin_env.h` | Add `overwrite_func(name, func)` method |
| `eta/interpreter/src/eta/interpreter/all_primitives.h` | No change needed |
| `eta/session/src/eta/session/driver.h` | Call `builtins_.overwrite_func("eval", ...)` after `verify_all_patched()` |
| `eta/session/src/eta/session/driver.h` | Add `"eval"` to `keyword_docs` in `hover_at` |
| `stdlib/std/core.eta` | Add `eval` to exports and docstring |
| `stdlib/tests/eval.test.eta` | New — 9 tests |
| `docs/guide/reference/eval.md` | New reference page |
| `docs/guide/reference/README.md` | Add `eval.md` row |
| `editors/vscode/snippets/eta.json` | Add `eval` snippet |

---

## Acceptance criteria

1. All 9 tests in `eval.test.eta` pass.
2. `(eval '(+ 1 2))` returns `3` in `etai` REPL, Jupyter kernel, and `etai script.eta`.
3. `(eval '(define x 42))` followed by `x` returns `42` in the same REPL session.
4. Errors inside the evaluated expression are catchable with `catch 'eval-error`.
5. `eval` from inside a `spawn-thread` worker evaluates in the child VM without
   corrupting the parent's state.
6. `.etac` files compiled before and after the change are detected as mismatched
   by the version check (because `builtin_count` changes); clear error message emitted.
7. `hover_at("eval")` returns meaningful markdown in the LSP / VS Code extension.

---

## Non-goals (v1)

- `(eval expr env)` — deferred to v2.
- `(compile expr)` — deferred; requires first-class procedure values pre-allocated
  without execution.
- `load` / `load-relative` — file loading; covered by `(import ...)` and `run_file`.
- `eval-when` — compile-time evaluation hooks; separate concern.
- Security sandbox for `eval` input — out of scope; document that `eval` has full
  access to the session environment and should not be used with untrusted input.

