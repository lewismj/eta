# AAD Next Steps: Pain Points and Implementation Tasks

[Back to README](../README.md) - [AAD](aad.md) - [Bytecode and VM](bytecode-vm.md) - [Project Status](next-steps.md)

---

## Scope

This document captures the most important engineering pain points in the
current tape-based AAD implementation, and a concrete task list for the next
implementation cycle.

---

## Current Pain Points

### 1) `TapeRef` does not encode ownership or lifecycle generation

`TapeRef` currently stores only a node index. It does not encode which tape
owns the node, and cannot distinguish a fresh tape from a reset/reused one.

Impact:

- Cross-tape misuse is hard to detect.
- A `TapeRef` can be interpreted against the wrong tape context.
- Stale references can survive resets and fail later in non-obvious ways.

Primary locations:

- `eta/core/src/eta/runtime/nanbox.h`
- `eta/core/src/eta/runtime/vm/vm.cpp`
- `eta/core/src/eta/runtime/core_primitives.h`

### 2) `tape-ref-value` is coupled to ambient active-tape state

`tape-ref-value` resolves through `vm->active_tape()` rather than an explicit
tape argument.

Impact:

- In nested or mixed-tape workflows, primal extraction depends on ambient VM state.
- Piecewise logic can read from the wrong tape if the active-tape stack differs from caller intent.

### 3) Nested and higher-order tape semantics are implicit

The runtime supports stack-based `tape-start!`/`tape-stop!`, but the contract
for cross-tape values and higher-order workflows is not explicit.

Impact:

- Behavior at nested boundaries is easy to misinterpret.
- Higher-order patterns are harder to reason about and test.

### 4) Parallel semantics are unclear (`spawn-thread` vs process parallelism)

Eta has multiple parallel mechanisms (`spawn-thread`/`spawn-thread-with`,
`spawn`, `worker-pool`), but the AAD contract across those boundaries is not
documented. (Resolution: see Task 1.4.)

Impact:

- It is unclear when `active_tape` state is shared vs isolated.
- Code may accidentally rely on transporting runtime-local AD values.
- Determinism is harder to guarantee in mixed concurrency code.

### 5) Non-differentiable numeric functions are not AD-safe by default

Many builtins (`<`, `>`, `<=`, `>=`, `abs`, `min`, `max`, predicates, etc.)
are not tape-aware and reject `TapeRef`.

Impact:

- User code must manually unwrap with `tape-ref-value`.
- Piecewise functions are possible, but ergonomics are poor and easy to misuse.

### 6) No explicit policy at kinks / non-differentiable points

For functions like `abs`, `max`, and ReLU-like branches, derivatives at kink
points are ambiguous. There is no standardized runtime policy.

Impact:

- Optimization behavior can be unstable or inconsistent across user code.
- Results at boundary points are not standardized.

### 7) AD primitive surface is narrower than math surface

Tape recording currently covers `+`, `-`, `*`, `/`, `sin`, `cos`, `exp`,
`log`, and `sqrt`. Several math functions in the runtime are not taped.

Impact:

- Users rewrite formulas into supported primitives.
- Model code complexity rises, increasing maintenance and correctness risk.

### 8) Edge-case behavior lacks an explicit AAD contract

Domain and singularity cases (`log(x<=0)`, `sqrt(x<0)`, divide-by-zero,
near-zero denominators in backward formulas) are not documented as an AAD
contract.

Impact:

- Behavior may depend on IEEE floating-point propagation.
- Production code lacks clear guidance on guard patterns.

### 9) Test coverage is strong on happy paths, thin on failure semantics

There are many positive-path tape tests, but limited coverage for:

- Cross-tape misuse detection
- Wrong-active-tape `tape-ref-value` behavior
- Stale-reference generation failures
- Kink-point policy behavior
- Concurrency and parallel-boundary semantics

Impact:

- Regressions in safety semantics may pass unnoticed.

---

## Implementation Tasks (Prioritized)

## Phase 0: Design Lock-In

### Task 0.1: ADR for `TapeRef` representation, generation, and error vocabulary

Before implementation starts, publish a short ADR that decides:

- `TapeRef` representation (`nanbox` payload extension vs heap object)
- ownership encoding fields (`tape_id`, `generation`, `node_index`)
- stale-ref validation behavior when tapes reset/clear
- equality/hash semantics for `TapeRef` (refs are `equal?` only when
  `tape_id`, `generation`, and `node_index` all match; refs from a prior
  generation are never `equal?` to current refs)
- canonical AD error tag vocabulary used by Task 1.5 and the test suite:
  - `:ad/mixed-tape`
  - `:ad/stale-ref`
  - `:ad/no-active-tape`
  - `:ad/nondiff-strict`
  - `:ad/cross-vm-ref`
  - `:ad/domain` (for Task 3.2 / Pain Point 8)

Acceptance criteria:

- ADR merged and linked from this document.
- Chosen representation has a migration plan and test implications listed.
- Tag vocabulary is fixed before Phase 1 implementation begins.

---

## Phase 1: Safety and Correctness (highest priority)

### Task 1.1: Encode tape ownership and generation in references

Required behavior:

- Any op involving refs from different tapes raises a deterministic AD error.
- Any ref lookup validates ownership, generation, and index bounds.
- Stale refs from prior tape generations fail deterministically.

"Deterministic AD error" means: a catchable runtime exception whose tag is
drawn from the Task 0.1 / Task 1.5 vocabulary, with a structured payload
(tape id, generation expected vs actual, node index). Tests assert on tag
identity, not message text. AD errors are never silent and never VM traps.

Acceptance criteria:

- Cross-tape arithmetic fails with `:ad/mixed-tape`.
- Stale refs after tape reset/clear fail with `:ad/stale-ref`.
- Errors are catchable with the standard exception mechanism.
- Existing single-tape examples continue to pass unchanged.

### Task 1.2: Add explicit tape lookup and harden implicit lookup

Add a new primitive:

- `(tape-ref-value-of tape ref)` -> number

Keep `tape-ref-value` for compatibility, but harden it:

- Non-`TapeRef` arguments remain pass-through (unchanged).
- `TapeRef` argument requires an active tape; otherwise raise
  `:ad/no-active-tape`.
- The ref must belong to the current active tape (`tape_id` and
  `generation` match); otherwise raise `:ad/mixed-tape` or `:ad/stale-ref`.
- The previous behavior of silently reading from whichever tape happens to
  be active is removed.

Back-compat and deprecation:

- Hardened `tape-ref-value` is shipped in the same release as
  `tape-ref-value-of`.
- `tape-ref-value` is marked deprecated in docs immediately; examples
  migrate in Task 5.2.
- A removal release is not scheduled in this cycle; revisit after Phase 5.

Acceptance criteria:

- Piecewise examples can be rewritten without ambient active-tape dependence.
- `tape-ref-value` has deterministic failure semantics on context mismatch.
- Deprecation note is present in `docs/aad.md`.

### Task 1.3: Specify nested tape contract

Document and enforce:

- `active_tape` is stack-based and nestable.
- Cross-tape `TapeRef` use is explicit and deterministic (either defined behavior or deterministic error).
- Expected behavior for higher-order workflows is specified in `docs/aad.md`.

Acceptance criteria:

- Nested tape behavior is no longer implicit.
- Functional tests cover supported nested patterns and unsupported cross-tape paths.

### Task 1.4: Specify parallel contract (`spawn-thread` vs process workers)

Define AAD behavior per parallel model:

- `spawn-thread` / `spawn-thread-with`: each actor runs in a fresh in-process VM with its own active-tape stack.
- `spawn` / `worker-pool`: each worker runs in a separate process VM with isolated tape state.
- Tape runtime handles (`Tape`, `TapeRef`) are VM-local and not part of cross-actor data contracts.
- In parallel code, prefer explicit-tape APIs (`tape-ref-value-of`, `tape-primal`, `tape-adjoint`) over ambient lookup.

Enforcement:

- The message serializer rejects `Tape` and `TapeRef` values at send time
  with the `:ad/cross-vm-ref` tag. Silent stripping or primal-only encoding
  is explicitly not done — the user must extract a primal scalar before send.

Acceptance criteria:

- Parallel semantics are documented in AAD docs and cross-referenced from networking docs.
- Tests verify deterministic behavior for both thread actors and process workers.
- Sending a `TapeRef` across an actor/worker boundary raises `:ad/cross-vm-ref`.

### Task 1.5: Standardize AD runtime error tags and payload shape

Implement the canonical tag vocabulary fixed in Task 0.1:

| Tag                    | Raised when                                                       |
|------------------------|-------------------------------------------------------------------|
| `:ad/mixed-tape`       | An op receives `TapeRef`s from different tapes                    |
| `:ad/stale-ref`        | A `TapeRef` references a prior generation of its tape             |
| `:ad/no-active-tape`   | Ambient-tape API called with no active tape on the stack          |
| `:ad/nondiff-strict`   | Strict-mode kink reached (Task 2.1)                               |
| `:ad/cross-vm-ref`     | `Tape`/`TapeRef` sent across a VM boundary (Task 1.4)             |
| `:ad/domain`           | Domain violation in a taped primitive (Task 3.2, Pain Point 8)    |

Each error carries a structured payload (e.g. `{tape-id, expected-gen, actual-gen, node-index, op}`).

Acceptance criteria:

- Tests assert on tag identity and payload fields, not only message text.
- The vocabulary above is exhaustive for AD errors raised by the runtime in
  this cycle; new tags require an ADR amendment.

---

## Phase 2: Non-Differentiability Strategy

### Task 2.1: Ship only `strict` and `zero-subgrad` initially

Initial policy modes:

- `strict` (raise `:ad/nondiff-strict` at kink)
- `zero-subgrad`

Deferred modes:

- `left-subgrad`
- `right-subgrad`

Operational definition of "kink" (per op):

- `abs(x)`: kink iff `x == 0.0` (exact bitwise equality, IEEE).
- `max(a, b)` / `min(a, b)`: kink iff `a == b` (exact equality).
- `relu(x)`: kink iff `x == 0.0`.
- `clamp(x, lo, hi)`: kink iff `x == lo` or `x == hi`.

Tolerance-based "near-kink" detection is explicitly out of scope; users who
need it must use the smooth helpers in Task 2.3.

Acceptance criteria:

- Policy default is documented (`strict` in tests/debug, explicit selection
  in production).
- Boundary-point behavior is deterministic and test-covered for each op above.

### Task 2.2: Provide AD-safe piecewise helpers and comparison contract

Add helpers in `std.math` or `std.aad`:

- `ad-abs`
- `ad-max`
- `ad-min`
- `ad-relu`
- `ad-clamp`

Define comparison behavior on taped values:

- Default mode auto-extracts primals and returns booleans.
- Strict mode may reject taped comparisons to surface unsafe branching.
- Mixed case (one taped, one plain number): primal is extracted from the
  taped side and the comparison proceeds normally; no error in default mode.

Acceptance criteria:

- Users can build piecewise models without raw `tape-ref-value` plumbing.
- Comparison semantics are documented and tested.

### Task 2.3: Add smooth alternatives for optimization workflows

Add optional smooth approximations:

- `softplus` (smooth ReLU / `max(0,x)`)
- `smooth-abs` with required `epsilon`
- optional smooth clamp

Acceptance criteria:

- Trade-off (bias vs optimization stability) is documented.
- Helper APIs avoid implicit epsilon defaults.

---

## Phase 3: Primitive Coverage and Scale

### Task 3.1: Extend tape-aware unary ops

Add and validate derivatives for:

- `tan`, `asin`, `acos`, `atan`

Acceptance criteria:

- Forward/backward formulas validated against finite differences.
- New primitives added to docs and examples.

### Task 3.2: Add first-class `pow` with domain contract

Add dedicated taped `pow` support and document domain behavior. Required
named cases:

- `pow(0, 0)`: return value and derivative behavior must be documented
  (recommended: value `1`, derivative raises `:ad/domain` in strict mode,
  zero in non-strict).
- `pow(negative, non-integer-exponent)`: raise `:ad/domain`.
- `pow(0, positive)`: value `0`; derivative w.r.t. base raises `:ad/domain`
  if the exponent is `< 1` (singular), otherwise zero.
- `pow(negative, integer)`: defined; derivative defined.

Acceptance criteria:

- `pow` value and derivative behavior is explicitly documented for each
  named case above.
- Regression tests cover each named edge-domain input.

### Task 3.3: Tape-memory scaling — minimum viable checkpointing

Long Monte Carlo paths (`monte-carlo.eta`, `sabr.eta`) grow tape memory
linearly in `path-length × paths`. Scalar AD is the primary AD workload in
this cycle (Task 3.4), so this is shipped, not stubbed.

Minimum viable scope:

- A `with-checkpoint` block that records only block inputs/outputs on the
  outer tape and re-records the block on backward pass.
- Naive implementation acceptable (re-execute the block; no segment graph).
- Documented memory/compute trade-off.

Acceptance criteria:

- `with-checkpoint` available in stdlib (`std.aad`).
- A Monte Carlo example demonstrates bounded tape memory under long paths.
- Gradient parity with non-checkpointed version verified by the Task 4.3 harness.

### Task 3.4: Decide scalar-only vs tensor-aware AD scope

Given ongoing Torch integration, explicitly choose:

- scalar-only AD for current cycle, or
- tensor-aware AD extension plan

Acceptance criteria:

- Decision is documented and reflected in Phase 5 docs.

---

## Phase 4: Testing and Validation

### Task 4.1: Add safety regression tests (between Task 1.1 and 1.2)

Add functional tests for:

- mixed-tape arithmetic and unary calls
- stale generation and stale index failures
- wrong-active-tape primal extraction

### Task 4.2: Add nondiff/kink tests

For each shipped policy/helper:

- `abs` at negative/positive/zero
- `max` at tie and non-tie
- comparison behavior on taped values
- nested branch stability

### Task 4.3: Add gradient checker harness and stdlib API

Create a numeric checker comparing AAD gradients to finite-difference
approximations for representative functions, and expose it as a stdlib helper
(for example `std.aad.check-grad`) in addition to CI usage.

Tolerance model (must be specified up front to avoid flaky CI):

- Combined relative + absolute tolerance: `|aad - fd| <= atol + rtol * |aad|`.
- Defaults: `rtol = 1e-5`, `atol = 1e-7` for double precision.
- Step-size selection for FD: central differences with `h = sqrt(eps) * max(1, |x|)`.
- Both tolerances and step are user-overridable.

Acceptance criteria:

- CI gate catches derivative regressions.
- Users can run gradient checks in model code/tests.
- Default tolerances pass for all primitives in Task 3.1 and Task 3.2.

### Task 4.4: Add parallelism semantics tests

Add explicit tests for:

- `spawn-thread` / `spawn-thread-with` isolation of tape context
- `spawn` / `worker-pool` process isolation
- deterministic failures on invalid cross-context AD values

---

## Phase 5: Documentation and Migration

### Task 5.1: Update AAD docs

Update `docs/aad.md` with:

- explicit nested-tape contract
- explicit parallel model contract (`spawn-thread` vs process workers)
- nondiff guidance and kink policy semantics
- failure modes and error expectations

### Task 5.2: Update examples

Refactor `examples/european.eta`, `examples/sabr.eta`, and related docs to use
explicit tape-aware helper patterns where relevant.

Acceptance criteria:

- Examples remain readable and deterministic under nesting and parallel usage.

### Task 5.3: Migration notes for existing user code

Add a "Migration" subsection to `docs/aad.md` listing user-visible changes
introduced by Phase 1 and Phase 2:

- New AD error tags (full vocabulary from Task 1.5) and what triggers each.
- `tape-ref-value` is now strict on `TapeRef` arguments; recommended
  replacement is `tape-ref-value-of`.
- `Tape` and `TapeRef` cannot cross actor/worker boundaries; extract a
  primal scalar before send.
- Comparison operators on taped values now auto-extract primals by default.
- Kink behavior on `abs`/`max`/`min`/`relu`/`clamp` follows the policy
  selected via Task 2.1.

Acceptance criteria:

- Migration section exists and is linked from the top of `docs/aad.md`.
- Each listed change references the Task number that introduced it.

---

## Recommended Delivery Order

1. Task 0.1 (ADR: representation + generation + tag vocabulary)
2. Task 1.1 -> Task 4.1 -> Task 1.2 -> Task 1.5
   - Task 1.3 (nested contract) and Task 1.4 (parallel contract) are
     specification work and may run in parallel with Task 1.2.
3. Task 2.1 -> Task 2.2 -> Task 4.2
4. Task 2.3 in parallel with step 3
5. Task 3.1 + Task 3.2, with Task 4.3 starting in parallel
6. Task 3.3 + Task 3.4
7. Phase 5 docs, examples, and migration notes

This order resolves correctness and determinism first, then expands ergonomics
and surface area with strong validation coverage.
