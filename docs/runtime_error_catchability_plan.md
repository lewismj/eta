# Runtime-Error Catchability Plan (Builtin Errors)

This document defines a concrete plan to make builtin/runtime errors catchable from Eta code.

It is intentionally scoped to **runtime errors returned by primitive/builtin calls** first, because that is the most painful gap and unlocks testability (`assert-throws` style behavior) in userland.

---

## 1. Problem Statement

Today, Eta has two separate failure channels:

1. `raise`/`catch` (VM `Throw`/`SetupCatch`) for user-level tagged exceptions.
2. `RuntimeError` return path for builtin/VM faults (e.g. `TypeError`, `UserError`).

Only channel (1) is catchable in Eta code.

Current behavior evidence:

- `dispatch_callee` primitive path returns `std::unexpected(RuntimeError)` directly on builtin failure (`eta/core/src/eta/runtime/vm/vm.cpp`).
- `catch` handlers are only entered via `do_throw(...)` (`vm.cpp`), i.e. explicit `raise`.
- `stdlib/std/test.eta` explicitly notes failing assertions cannot be trapped yet.

Impact:

- Eta code cannot recover from builtin errors.
- Eta tests cannot assert error behavior in-language.
- Libraries must push error assertions down to C++ tests, which is a layering mismatch.

---

## 2. Goals and Non-Goals

## 2.1 Goals

- Make builtin/runtime errors catchable from Eta.
- Preserve current uncaught-error diagnostics and host behavior.
- Avoid new bytecode opcodes for V1.
- Keep `raise`/`catch` source syntax stable.
- Provide deterministic, machine-parseable error payloads for tests.

## 2.2 Non-Goals (V1)

- No full condition-system redesign.
- No new typed runtime error heap object.
- No changes to parser/expander semantics unless needed for compatibility.
- No broad rewrite of all VM-internal faults in V1 (focus on builtin error path first).

---

## 3. Proposed Runtime Semantics

## 3.1 Reserved runtime-error tags

Runtime errors are surfaced as tagged catchable events with reserved symbol tags:

- super-tag: `runtime.error`
- specific tags:
  - `runtime.type-error`
  - `runtime.invalid-arity`
  - `runtime.user-error`
  - `runtime.undefined-global`
  - `runtime.internal-error`
  - `runtime.nanbox-error`
  - `runtime.heap-error`
  - `runtime.intern-error`

## 3.2 Matching behavior

On builtin/runtime failure:

- `catch 'runtime.error ...` catches all runtime errors.
- `catch 'runtime.type-error ...` catches only that subtype.
- `catch 'tag ...` for non-runtime tag does not catch runtime errors.

Compatibility rule for V1 (updated):

- `(catch body)` (tagless catch-all) catches both explicit `raise` and runtime errors.

## 3.3 Caught payload shape

When a runtime error is caught, the catch result is:

`(runtime-error <tag-symbol> <message-string> <span-record> <stack-trace>)`

Where:

- `<span-record>` is `(span file-id start-line start-column end-line end-column)`
- `<stack-trace>` is `((frame function-name <span-record>) ...)`

Examples:

- `(catch 'runtime.error (car 42))`
  => `(runtime-error runtime.type-error "car: argument must be a pair")`
- `(catch 'runtime.invalid-arity (car 1 2))`
  => `(runtime-error runtime.invalid-arity "...")`

Uncaught behavior remains unchanged: execution aborts with `RuntimeError` diagnostics.

---

## 4. Implementation Plan

## 4.1 VM plumbing (no opcode changes)

Primary touchpoints:

- `eta/core/src/eta/runtime/vm/vm.h`
- `eta/core/src/eta/runtime/vm/vm.cpp`

Add VM helpers:

- `runtime_error_tag(RuntimeError&) -> symbol LispVal`
- `runtime_error_message(RuntimeError&) -> std::string`
- `build_runtime_error_payload(...) -> LispVal` (list as defined above)
- `do_runtime_error(RuntimeError, Span)`:
  - scans `catch_stack_` for matching runtime tag (`runtime.error` or specific)
  - restores frame/stack/wind/tape state like `do_throw`
  - pushes payload and resumes execution
  - returns uncaught error unchanged if no matching handler

Patch primitive dispatch:

- In `dispatch_callee` primitive branch, when `prim->func(args)` fails:
  - call `do_runtime_error(err, span-at-callsite)`
  - if handled: continue execution (same frame action as a handled throw)
  - if unhandled: preserve current `std::unexpected(err)` path

Note:

- Include best-effort instruction span and stack trace frames in the payload.
- If payload allocation fails while constructing caught value, fall back to uncaught original error.

## 4.2 No compiler/emitter opcode work in V1

No change required to:

- `semantic_analyzer.cpp` (`raise`/`catch` forms stay as-is)
- `emitter.cpp`
- `bytecode.h`

This keeps rollout low-risk and focused.

## 4.3 Optional stdlib helper (post-V1)

After VM behavior lands, optionally add helper wrappers in stdlib:

- `(runtime-error? v)`
- `(runtime-error-tag v)`
- `(runtime-error-message v)`

This is convenience only, not required for core capability.

---

## 5. Testing Plan

## 5.1 C++ tests (authoritative for failure-path behavior)

Add VM-level tests in `eta/test/src/vm_tests.cpp` (or dedicated file):

1. `catch 'runtime.error` catches builtin `TypeError` (`car` on non-pair).
2. Specific subtype catch works (`runtime.invalid-arity`).
3. Mismatched runtime subtype does not catch (error propagates).
4. Tagless `(catch body)` catches runtime errors.
5. Explicit `raise` behavior unchanged.
6. Nested catches: inner matching runtime catch wins.
7. Uncaught runtime errors still return `RuntimeError` with original code/message.

## 5.2 Eta tests (language-level contract)

Add `stdlib/tests/runtime_error_catchability.test.eta`:

- successful catches produce `(runtime-error tag msg)` payload
- both super-tag and subtype catches work
- caught payload is introspectable via list ops

Because uncaught errors abort current test execution, mismatch/unhandled paths stay in C++ tests.

## 5.3 Regression targets

- Existing exception examples/docs still pass.
- `stdlib/std/logic.eta` `logic-catch` behavior unchanged.
- Existing stdlib and core test suites remain green.

---

## 6. Rollout Sequence

1. Land VM plumbing + C++ tests.
2. Land Eta-level test file for caught runtime payloads.
3. Update docs (`docs/examples.md`, `docs/logic.md`, `docs/runtime.md`) with new catchable runtime-error contract.
4. Optional follow-up: add `std.test` helper `assert-raises` built on the new capability.

---

## 7. Open Decisions

1. Should `(catch body)` catch runtime errors too?
   - Recommendation for V1: **Yes**.
2. Should payload include source span/module now?
   - Recommendation: **Yes**, include span and stack trace in V1 payload.
3. Should VM-internal faults (not from builtin calls) use the same channel?
   - Recommendation: V1 no; evaluate once builtin path is stable.

---

## 8. Done Criteria

This plan is complete when all are true:

- builtin runtime errors are catchable with `catch 'runtime.*`.
- `(catch body)` catches runtime errors as catch-all.
- uncaught runtime errors keep current host-visible behavior.
- no new opcode was introduced.
- C++ and Eta tests for the new contract pass.
- docs reflect the new semantics and payload contract.
