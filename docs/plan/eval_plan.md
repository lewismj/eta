# `eval` — Implementation Plan

**Status:** Proposed · **Priority:** P1 · **Effort:** ~5 days
**Revision:** 3 — reentrant from day one (no v1/v2 split). Addresses VM
state-snapshot, GC reachability, error tagging, registration, and tests.

---

## Motivation

Eta is a homoiconic Lisp.  `quote` and `quasiquote` let programs treat code
as data; without `eval` the reverse direction is missing.

- `do:simplify` returns an estimand AST.  With `eval`:
  ```scheme
  (eval (do:simplify (id g '(y) '(x))))
  ```
- Symbolic differentiator output (`causal_demo.eta`) becomes directly
  executable.
- Programmatic code generation in Jupyter notebooks becomes natural.
- `define-dag`-style macros can emit and run code at runtime.

---

## Design

### API (v1, full)

```scheme
(eval expr)
```

`expr` is any Eta value (typically a quoted list).  Behaviour:

| `expr`                 | Result                               |
|------------------------|--------------------------------------|
| Self-evaluating value  | Returned unchanged                   |
| Quoted list / pair     | Compiled and executed; result returned |
| Symbol                 | Treated as a variable reference; resolved in current session |
| Anything else          | Returned unchanged                   |

Examples:

```scheme
(eval '(+ 1 2))                       ;; => 3
(eval (list '* 3 4))                  ;; => 12
(eval `(define x ,(+ 1 2)))           ;; defines x = 3 in session
(eval 42)                             ;; => 42
(eval "hello")                        ;; => "hello"

;; From inside a function — works (reentrant):
(define (square-via-eval n)
  (eval `(* ,n ,n)))
(square-via-eval 7)                   ;; => 49
```

Errors propagate as catchable Eta exceptions tagged `'eval-error`:

```scheme
(catch 'eval-error
  (eval '(undefined-fn 1 2)))
```

### Deferred (independent follow-ups, not blocked on this work)

- `(eval expr env)` — needs first-class environment objects.
- `(compile expr)` — needs first-class closure pre-allocation.

These are independent of reentrancy and can land later without an API break.

---

## Architecture

### The reentrancy problem and its solution

The VM's per-execution state is stored in `VM` member variables, not on the
C++ call stack:

| Field | Reason |
|---|---|
| `current_func_`, `pc_`, `fp_`, `current_closure_` | active frame |
| `stack_` | operand stack (resized at `execute()` entry per `main.stack_size`) |
| `frames_` | call frames (a sentinel `FrameKind::Sentinel` is pushed at `execute()` entry) |
| `catch_stack_` | live exception handlers |
| `winding_stack_` | dynamic-wind frames |
| `pending_unwind_thunks_` + `pending_unwind_index_` | in-flight unwind |
| `pending_exception_transfer_` | cross-frame raise |
| `temp_roots_` | GC roots for in-flight primitives |

`VM::execute` (`vm.cpp` line 327) pushes a sentinel frame, resets the active
fields, resizes `stack_`, then enters `run_loop`.  Calling `execute` again
from inside a primitive while the outer `run_loop` is still on the C++ stack
overwrites these fields and corrupts the outer execution.

**Solution.** A typed `ExecutionSnapshot` saves and restores exactly the
fields that `execute()` rewrites on entry.  Trail, constraint store, real
store, attribute store, globals, and heap are *not* snapshotted — they are
either trail-bound (and the eval'd code's writes are intentionally retained
as session state) or shared by design.

```cpp
// vm.h — public:
struct ExecutionSnapshot {
    const BytecodeFunction*               func;
    uint32_t                              pc;
    uint32_t                              fp;
    LispVal                               closure;
    std::vector<LispVal>                  stack;
    std::vector<Frame>                    frames;
    std::vector<CatchFrame>               catch_stack;
    std::vector<WindFrame>                winding_stack;
    std::vector<LispVal>                  pending_unwind_thunks;
    std::size_t                           pending_unwind_index;
    std::optional<PendingExceptionTransfer> pending_exception_transfer;
    std::vector<LispVal>                  temp_roots;
};

void save_execution_state();
void restore_execution_state();
```

`save_execution_state` *moves* the live vectors into a new snapshot pushed
onto a member stack and resets the live members to a clean baseline.
`restore_execution_state` pops and moves them back.  Per-call cost is O(1)
moves rather than O(stack-size) copies.

### GC reachability of saved state

The snapshot holds `LispVal`s that may point to heap objects.  The GC root
scan only looks at `VM` member fields, so the snapshot stack must itself be a
member:

```cpp
// vm.h — private:
std::vector<ExecutionSnapshot> saved_executions_;
```

`save_execution_state` pushes onto `saved_executions_`; `restore_execution_state`
pops.  The GC's existing root scan is extended to also walk every `LispVal`
inside every active snapshot:

```cpp
// In MarkSweepGC::mark_roots (or wherever VM roots are enumerated):
for (const auto& snap : vm.saved_executions()) {
    for (LispVal v : snap.stack)         mark_root(v);
    for (LispVal v : snap.temp_roots)    mark_root(v);
    for (LispVal v : snap.pending_unwind_thunks) mark_root(v);
    for (const Frame& f : snap.frames)   mark_frame(f);
    for (const CatchFrame& cf : snap.catch_stack) mark_root(cf.handler);
    for (const WindFrame& wf : snap.winding_stack) {
        mark_root(wf.before); mark_root(wf.after);
    }
    if (snap.pending_exception_transfer)
        mark_root(snap.pending_exception_transfer->payload);
    if (snap.closure != Nil) mark_root(snap.closure);
}
```

Factor the live-field walk into a helper that takes references to the
relevant vectors so it can be reused for snapshots — avoids two copies of
the marking logic.

### Trail handling

The trail (`trail_stack_`), constraint store, attribute store, and CLP real
store are **not** snapshotted.  Reasoning:

- Logic-var bindings made by eval'd code that are *intentional* (top-level
  `(unify x 3)`) should persist as session state — the same way they
  persist when typed at the REPL.
- Trail entries from inside the eval'd code that have *not* been committed
  by an outer choice-point are still associated with positions later than
  any pre-`eval` trail mark, so existing `unwind-trail` / `findall`
  scaffolding behaves correctly.

If a future test demonstrates breakage we can add an optional
`(eval/sandbox expr)` variant that snapshot-and-restores the trail too.

### Error tagging

Compile-time and VM errors inside `eval`'d code are normalised to the symbol
tag `'eval-error` with the diagnostic text as payload, so user code can
`catch` reliably:

```scheme
(catch 'eval-error
  (eval '(undefined-fn 1 2)))
```

User `(error 'my-tag "...")` calls inside the eval'd code propagate with
their *original* tag, not `'eval-error` — only un-tagged compile/VM errors
get the umbrella tag.  Implementation: `run_source` returns success/failure;
on failure we wrap into `'eval-error`; on success the user's own `raise`
value already carries the right tag because `RuntimeError::tag_override`
survives unwinding through `run_source`.

### Registration

`eval` needs `Driver::run_source`, which is not available from
`all_primitives.h`.  One canonical strategy:

1. Add `r("eval", 1, false);` at the **end of the core block** in
   `builtin_names.h` — preserves `builtin_count` for `.etac` version checks.
2. Add an unreachable stub in `core_primitives.h` (overwritten in step 3).
3. After `verify_all_patched()` in the `Driver` constructor, call the new
   `BuiltinEnvironment::overwrite_func("eval", ...)` to install the real
   closure that captures `this`.

Same pattern as the NNG primitives that capture `&registry_`,
`&vm_.globals()`, etc.

### Thread safety

Each `spawn-thread` worker owns its own `Driver` and `VM`.  `eval` from a
worker compiles + executes in that child's context with its own
`saved_executions_` stack.  No cross-thread synchronisation needed.

### Why not export `eval` from `std.core`

Builtins live in global slots and are not module-level bindings.  Adding
`eval` to `std.core`'s `(export ...)` list would fail at semantic analysis
with "unknown export".  Builtins are visible in every module without
`import` — same as `+`, `cons`, `error`.  No `std.core` change required.

---

## Implementation steps

### Step 1 — `ExecutionSnapshot` + `saved_executions_`

```cpp
// vm.h — public:
struct ExecutionSnapshot { /* fields as above */ };

void save_execution_state();
void restore_execution_state();

[[nodiscard]] const std::vector<ExecutionSnapshot>& saved_executions() const noexcept {
    return saved_executions_;
}
[[nodiscard]] bool is_executing() const noexcept {
    return current_func_ != nullptr;
}

// vm.h — private:
std::vector<ExecutionSnapshot> saved_executions_;
```

```cpp
// vm.cpp:
void VM::save_execution_state() {
    ExecutionSnapshot s{};
    s.func                       = current_func_;
    s.pc                         = pc_;
    s.fp                         = fp_;
    s.closure                    = current_closure_;
    s.stack                      = std::move(stack_);
    s.frames                     = std::move(frames_);
    s.catch_stack                = std::move(catch_stack_);
    s.winding_stack              = std::move(winding_stack_);
    s.pending_unwind_thunks      = std::move(pending_unwind_thunks_);
    s.pending_unwind_index       = pending_unwind_index_;
    s.pending_exception_transfer = std::move(pending_exception_transfer_);
    s.temp_roots                 = std::move(temp_roots_);

    // Reset live fields to baseline (execute() will repopulate them).
    current_func_           = nullptr;
    pc_                     = 0;
    fp_                     = 0;
    current_closure_        = Nil;
    stack_.clear();
    frames_.clear();
    catch_stack_.clear();
    winding_stack_.clear();
    pending_unwind_thunks_.clear();
    pending_unwind_index_   = 0;
    pending_exception_transfer_.reset();
    temp_roots_.clear();

    saved_executions_.push_back(std::move(s));
}

void VM::restore_execution_state() {
    assert(!saved_executions_.empty() && "restore_execution_state without save");
    auto s = std::move(saved_executions_.back());
    saved_executions_.pop_back();
    current_func_               = s.func;
    pc_                         = s.pc;
    fp_                         = s.fp;
    current_closure_            = s.closure;
    stack_                      = std::move(s.stack);
    frames_                     = std::move(s.frames);
    catch_stack_                = std::move(s.catch_stack);
    winding_stack_              = std::move(s.winding_stack);
    pending_unwind_thunks_      = std::move(s.pending_unwind_thunks);
    pending_unwind_index_       = s.pending_unwind_index;
    pending_exception_transfer_ = std::move(s.pending_exception_transfer);
    temp_roots_                 = std::move(s.temp_roots);
}
```

RAII helper used by the `eval` primitive:

```cpp
struct ExecutionScope {
    runtime::vm::VM& vm;
    bool active;
    explicit ExecutionScope(runtime::vm::VM& v)
        : vm(v), active(v.is_executing())
    { if (active) vm.save_execution_state(); }
    ~ExecutionScope() { if (active) vm.restore_execution_state(); }
};
```

### Step 2 — extend GC root enumeration

Locate the function that currently walks `vm_.stack()`, `vm_.frames()`,
`vm_.globals()`, etc., and add a loop over `vm.saved_executions()` mirroring
the live-field walk.  Factor the per-set walk into a helper.

### Step 3 — `BuiltinEnvironment::overwrite_func`

```cpp
// builtin_env.h:
void overwrite_func(std::string_view name, PrimitiveFunc func) {
    auto idx = lookup(name);
    assert(idx.has_value() && "overwrite_func: unknown builtin");
    specs_[*idx].func = std::move(func);
}
```

### Step 4 — `builtin_names.h`

Add at the end of the core-primitives block:

```cpp
r("eval", 1, false);
```

### Step 5 — `core_primitives.h` stub

```cpp
env.patch("eval", [](std::span<const LispVal>) -> std::expected<LispVal, RuntimeError> {
    return std::unexpected(RuntimeError{VMError{
        RuntimeErrorCode::InternalError,
        "eval: stub — Driver did not install the real implementation"}});
});
```

### Step 6 — `Driver` installs the real `eval`

After `builtins_.verify_all_patched()`:

```cpp
builtins_.overwrite_func("eval",
    [this](std::span<const runtime::nanbox::LispVal> args)
        -> std::expected<runtime::nanbox::LispVal,
                         runtime::error::RuntimeError>
    {
        using runtime::nanbox::LispVal, runtime::nanbox::Nil;
        using runtime::error::RuntimeError;
        using runtime::error::VMError;
        using runtime::error::RuntimeErrorCode;

        if (args.size() != 1)
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InvalidArity, "eval: expected 1 argument"}});

        // Render the argument as source text (write representation).
        std::string src = format_value(args[0], runtime::FormatMode::Write);

        // Reentrant: snapshot outer VM execution state, restore on exit.
        ExecutionScope scope(vm_);

        LispVal result = Nil;
        if (!run_source(src, &result)) {
            std::ostringstream oss;
            diag_engine_.print_all(oss, /*use_color=*/false, file_resolver());
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::UserError, oss.str(),
                /*tag_override=*/"eval-error"}});
        }
        return result;
    });
```

`Driver::run_source` already calls `vm_.execute(...)`, which now operates on
the cleared baseline.  When it returns, `ExecutionScope`'s destructor
restores the outer context — including on the unexpected/error path.

### Step 7 — Eta tests (`stdlib/tests/eval.test.eta`)

```scheme
(module eval-tests
  (import std.core)
  (import std.test)
  (begin

    (test "eval: self-evaluating literal"
      (assert-equal (eval 42) 42))

    (test "eval: quoted list arithmetic"
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

    (test "eval: string is self-evaluating"
      (assert-equal (eval "hello") "hello"))

    ;; Reentrant — called from inside a running VM frame.
    (test "eval: reentrant inside lambda"
      (let ((f (lambda (n) (eval `(* ,n ,n)))))
        (assert-equal (f 6) 36)))

    ;; Reentrant — eval inside a deep recursion.
    (test "eval: reentrant inside recursion"
      (letrec ((depth (lambda (k acc)
                        (if (= k 0) acc
                            (depth (- k 1) (eval `(+ ,acc 1)))))))
        (assert-equal (depth 100 0) 100)))

    ;; Stack of saved executions: nested eval inside a lambda.
    (test "eval: nested reentrant"
      (let ((f (lambda ()
                 (eval '(eval '(+ 2 3))))))
        (assert-equal (f) 5)))

    ;; Tag normalisation: compile error -> 'eval-error.
    (test "eval: compile error raises eval-error"
      (let ((tag #f))
        (catch (lambda (t _) (set! tag t))
          (eval '(undefined-function-xyz)))
        (assert-equal tag 'eval-error)))

    ;; User-tagged errors propagate untouched (NOT renamed to eval-error).
    (test "eval: user tag preserved"
      (let ((tag #f))
        (catch (lambda (t _) (set! tag t))
          (eval '(error 'my-tag "oops")))
        (assert-equal tag 'my-tag)))

    ;; GC stress while reentrant: the snapshot must keep stack values alive.
    (test "eval: GC during reentrant eval preserves outer stack"
      (let ((data (list 1 2 3 4 5)))
        (eval '(begin
                 (define (alloc-much) (make-vector 10000 0))
                 (alloc-much) (alloc-much) (alloc-much)))
        (assert-equal data '(1 2 3 4 5))))

    ;; Causal estimand AST round-trip.
    (test "eval: causal-style AST"
      (let* ((ast '(begin (define %et-result (* 6 7)) %et-result)))
        (assert-equal (eval ast) 42)))

  ))
```

### Step 8 — C++ unit test (`eta/test/src/vm_eval_tests.cpp`)

Focused gtest that exercises the snapshot mechanism in isolation of the
Driver:
- `save_execution_state` clears live members.
- `restore_execution_state` round-trips identical bytes.
- Two concurrent snapshots stack correctly (LIFO).
- `saved_executions()` count grows / shrinks as expected.
- GC root walk visits values inside an active snapshot — allocate a unique
  heap object, snapshot with it pinned in the outer `stack_`, force a GC,
  restore, assert the object is still alive and its bit-pattern intact.

### Step 9 — `hover_at` docstring

```cpp
{"eval",
 "**eval**  -  Compile and evaluate an expression in the current session "
 "environment.\n\n"
 "`(eval expr)`\n\n"
 "EXPR is typically a quoted list.  Self-evaluating values are returned "
 "unchanged.  Reentrant: safe to call from inside any function.  "
 "Compile / VM errors are caught with `(catch 'eval-error ...)`; user "
 "`raise`/`error` calls inside the evaluated code propagate with their "
 "original tag.\n\n"
 "**Note:** `eval` is a builtin — available without `(import ...)`."},
```

---

## File change summary

| File | Change |
|---|---|
| `eta/core/src/eta/runtime/vm/vm.h` | `ExecutionSnapshot`, `save/restore_execution_state`, `saved_executions_`, `is_executing()` |
| `eta/core/src/eta/runtime/vm/vm.cpp` | Implementations (no change to `execute()` itself; it already resets cleanly) |
| `eta/core/src/eta/runtime/memory/gc/mark_sweep.cpp` (or wherever GC roots are enumerated) | Walk `vm.saved_executions()` during root marking |
| `eta/core/src/eta/runtime/builtin_env.h` | `overwrite_func(name, func)` |
| `eta/core/src/eta/runtime/builtin_names.h` | `r("eval", 1, false);` at end of core block |
| `eta/core/src/eta/runtime/core_primitives.h` | Unreachable stub for `eval` |
| `eta/session/src/eta/session/driver.h` | `ExecutionScope` RAII; install reentrant `eval` after `verify_all_patched()`; add `"eval"` to `keyword_docs` |
| `stdlib/tests/eval.test.eta` | New — 13 tests including reentrancy, nesting, tag preservation, GC stress |
| `eta/test/src/vm_eval_tests.cpp` | New — C++ unit test for snapshot mechanism |
| `docs/guide/reference/eval.md` | New reference page |
| `docs/guide/reference/README.md` | Add `eval.md` row |
| `editors/vscode/snippets/eta.json` | Add `eval` snippet |

*No change to `stdlib/std/core.eta` — builtins are globally visible without export.*

---

## Acceptance criteria

1. All 13 tests in `eval.test.eta` pass, including all reentrancy and
   nesting scenarios.
2. C++ unit test passes: snapshot/restore round-trip, LIFO nesting, GC root
   walk into snapshots.
3. `(eval '(+ 1 2))` returns `3` in `etai` REPL, Jupyter cell, script
   entry-point, **and** from inside any function/lambda.
4. `(define f (lambda () (eval '(* 6 7))))` followed by `(f)` returns `42`.
5. Compile errors in eval'd code raise `'eval-error`.
6. User `(error 'my-tag "...")` inside eval'd code raises `'my-tag` (not
   renamed).
7. GC during reentrant `eval` does not free objects reachable only from the
   outer snapshot's stack/frames.
8. `eval` from a `spawn-thread` worker evaluates in the child VM without
   touching the parent.
9. `.etac` files compiled before this change are detected as mismatched
   (`builtin_count` changed); clear error message emitted.
10. `hover_at("eval")` returns the documented markdown in LSP / VS Code.

---

## Non-goals (this iteration)

- `(eval expr env)` — needs first-class environment objects.
- `(compile expr)` — needs callable closure pre-allocation.
- `load` / `load-relative` — covered by `(import ...)` and
  `Driver::run_file`.
- `eval-when` — compile-time evaluation hooks; separate concern.
- Sandboxed `eval` (no session mutation) — defer until a concrete need
  emerges; would be a separate `eval/sandbox` builtin that additionally
  snapshots and rolls back trail/globals/constraint store.
- Security boundary for untrusted input — `eval` has full session access;
  document and leave to callers.

---

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| GC misses values inside a snapshot → use-after-free | C++ unit test explicitly forces GC with reachable-only-via-snapshot objects |
| Snapshot vector copy is O(stack-size) per `eval` call | Snapshots use `std::move`; only baseline-clear costs are paid (vector `clear()` is O(0) for trivially-destructible elements; `Frame`/`LispVal` are trivial) |
| Trail not snapshotted → eval'd `unify` leaks into outer search | Documented; if a real test fails we add `eval/sandbox` |
| `pending_exception_transfer_` left in a bad state by failed eval | RAII `ExecutionScope` destructor restores it unconditionally |
| `frames_` sentinel pushed by inner `execute()` not popped on error | Not an issue: snapshot replaces `frames_` wholesale on restore |
| `BytecodeFunction*` in snapshot dangles if its module is hot-reloaded mid-eval | Out of scope; modules are not hot-reloaded mid-`eval` in current Driver |
| `process_pending_finalizers()` in inner `execute()` runs Eta finalizers that themselves call `eval` | Already supported — finalizers run via `call_value`, which sees `current_func_ == nullptr` after the inner `execute()` returns; `ExecutionScope` is reentrant |

