# AAD Next Steps: Pain Points and Implementation Tasks

[Back to README](../README.md) · [AAD](aad.md) · [Bytecode and VM](bytecode-vm.md) · [Project Status](next-steps.md)

---

## Scope

This document captures the most important engineering pain points in the
current tape-based AAD implementation, and a concrete task list for the
next implementation cycle.

---

## Current Pain Points

### 1) `TapeRef` has no tape ownership

`TapeRef` currently stores only a node index. It does not encode which tape
that index belongs to.

Impact:

- Cross-tape misuse is hard to detect.
- A `TapeRef` can be interpreted against the wrong active tape.
- This can lead to wrong gradients or index errors that are non-obvious at the call site.

Primary locations:

- `eta/core/src/eta/runtime/nanbox.h`
- `eta/core/src/eta/runtime/vm/vm.cpp`
- `eta/core/src/eta/runtime/core_primitives.h`

### 2) `tape-ref-value` is coupled to the active tape

`tape-ref-value` resolves via `vm->active_tape()` rather than an explicit tape argument.

Impact:

- In nested or mixed-tape workflows, primal extraction depends on ambient VM state.
- Piecewise logic can read from the wrong tape if the active-tape stack is not what the caller expects.

### 3) Non-differentiable numeric functions are not AD-safe by default

Many builtins (`<`, `>`, `<=`, `>=`, `abs`, `min`, `max`, predicates, etc.)
are not tape-aware and reject `TapeRef`.

Impact:

- User code must manually unwrap with `tape-ref-value`.
- Piecewise functions are possible, but ergonomics are poor and easy to misuse.

### 4) No explicit policy at kinks / non-differentiable points

For functions like `abs`, `max`, ReLU-like branches, derivatives at kink points
are ambiguous. There is no runtime policy (error vs subgradient convention).

Impact:

- Optimization behavior can be unstable or inconsistent across user code.
- Results at boundary points are not standardized.

### 5) AD primitive surface is narrower than math surface

Tape recording currently covers `+`, `-`, `*`, `/`, `sin`, `cos`, `exp`,
`log`, `sqrt`. Several math functions in the runtime are not taped.

Impact:

- Users rewrite formulas into supported primitives.
- Increased model code complexity and risk of mistakes.

### 6) Edge-case behavior lacks explicit contract

Domain and singularity cases (`log(x<=0)`, `sqrt(x<0)`, divide-by-zero,
near-zero denominators in backward formulas) are not documented as an AAD
contract.

Impact:

- Behavior may depend on IEEE floating-point propagation.
- Production code lacks clear guidance on guard patterns.

### 7) Test coverage is strong for core paths but thin for failure semantics

There are many positive-path tape tests, but limited coverage for:

- Cross-tape misuse detection
- Wrong-active-tape `tape-ref-value` behavior
- Kink-point policy behavior
- Nondiff helper conventions

Impact:

- Regressions in safety semantics may pass unnoticed.

---

## Implementation Tasks (Prioritized)

## Phase 1: Safety and Correctness (highest priority)

### Task 1.1: Encode tape ownership in references

Options:

- Add tape identity to `TapeRef` payload encoding.
- Or move AD refs to a small heap object (`{tape_id, node_index}`).

Required behavior:

- Any op involving refs from different tapes raises a deterministic runtime error.
- Any ref lookup validates both ownership and index bounds.

Acceptance criteria:

- Cross-tape arithmetic fails with a clear error.
- Existing single-tape examples continue to pass unchanged.

### Task 1.2: Add explicit tape-based primal lookup

Add a new primitive:

- `(tape-ref-value-of tape ref)` -> number

Keep `tape-ref-value` for compatibility (short-term), but document it as
context-dependent and deprecate in examples.

Acceptance criteria:

- All piecewise examples can be rewritten without reliance on active tape state.
- Nested tape tests verify deterministic behavior.

### Task 1.3: Standardize AD runtime errors

Introduce clear error messages/codes for:

- mixed-tape refs
- stale/invalid refs
- no active tape for active-tape-dependent APIs
- nondiff op on taped value (when strict mode is enabled)

Acceptance criteria:

- Errors are stable and asserted in tests.

---

## Phase 2: Non-Differentiability Strategy

### Task 2.1: Define kink policy modes

Add a runtime or stdlib-level policy for non-differentiable points:

- `strict` (raise at kink)
- `zero-subgrad`
- `left-subgrad`
- `right-subgrad`

Default recommendation:

- `strict` in tests/debug
- explicit policy selection in production model code

Acceptance criteria:

- Policy is documented and test-covered.
- Boundary-point behavior is deterministic.

### Task 2.2: Provide AD-safe piecewise helpers in stdlib

Add helpers in `std.math` or new `std.aad`:

- `ad-abs`
- `ad-max`
- `ad-min`
- `ad-relu`
- `ad-clamp`

Helpers should:

- branch using primal extraction
- apply chosen kink policy
- preserve tape tracking through returned expressions

Acceptance criteria:

- Users can build piecewise models without raw `tape-ref-value` plumbing.

### Task 2.3: Add smooth alternatives for optimization workflows

Add optional smooth approximations:

- `softplus` (smooth ReLU/max(0,x))
- `smooth-abs` (epsilon-based)
- optional smooth clamp

Acceptance criteria:

- Documented trade-off: bias vs optimization stability.

---

## Phase 3: Primitive Coverage Expansion

### Task 3.1: Extend tape-aware unary ops

Evaluate and add derivatives for:

- `tan`, `asin`, `acos`, `atan`

Acceptance criteria:

- Forward and backward formulas validated against finite differences.
- New primitives included in docs and examples.

### Task 3.2: Consider first-class `pow`

Current patterns emulate power via `exp(b * log(a))`.
A dedicated `pow` can improve readability and boundary handling control.

Acceptance criteria:

- Decision documented (add or intentionally defer).

---

## Phase 4: Testing and Validation

### Task 4.1: Add safety regression tests

Add functional tests for:

- mixed-tape arithmetic and unary calls
- wrong-active-tape primal extraction
- stale ref index failures

### Task 4.2: Add nondiff/kink tests

For each policy and helper:

- `abs` at negative/positive/zero
- `max` at tie and non-tie
- piecewise branch stability across nested tapes

### Task 4.3: Add gradient checker harness

Create a small numeric checker comparing AAD gradients to finite-difference
approximations for representative functions.

Acceptance criteria:

- CI gate catches obvious derivative regressions.

---

## Phase 5: Documentation and Migration

### Task 5.1: Update AAD docs

Update `docs/aad.md` with:

- explicit nondiff guidance
- piecewise helper usage
- kink policy semantics
- failure modes and error expectations

### Task 5.2: Update examples

Refactor `examples/european.eta`, `examples/sabr.eta`, and related docs to use
explicit tape-aware helper patterns where relevant.

Acceptance criteria:

- Examples remain readable and deterministic under nesting.

---

## Recommended Delivery Order

1. Phase 1 (ownership + explicit primal lookup + errors)
2. Phase 4.1 (safety tests immediately after Phase 1)
3. Phase 2 (kink policy + helpers)
4. Phase 3 (new taped primitives)
5. Phase 5 (docs + example refresh)

This order reduces correctness risk first, then improves modeling ergonomics.
