# Adjoint Algorithmic Differentiation (AAD)

[Back to README](../README.md) | [Examples](examples.md) | [Modules](modules.md) | [ADR 0001](adr/0001-aad-taperef-and-error-tags.md)

---

## Contents

- [Overview](#overview)
- [Core API](#core-api)
- [Safety Model](#safety-model)
- [Nested Tape Contract](#nested-tape-contract)
- [Parallel Contract](#parallel-contract)
- [Non-Differentiability Policy](#non-differentiability-policy)
- [Primitive Coverage and Domain Rules](#primitive-coverage-and-domain-rules)
- [Stdlib Helpers](#stdlib-helpers)
- [AD Error Tags](#ad-error-tags)
- [Examples](#examples)

---

## Overview

Eta provides VM-native reverse-mode automatic differentiation using a tape
(Wengert list). When an operation sees a `TapeRef`, the VM records the forward
operation and later computes adjoints in a reverse sweep.

Eta uses standardized safety semantics for ownership, stale references,
cross-VM boundaries, non-differentiable branch policy, and domain failures.

---

## Core API

### Tape lifecycle and values

| Primitive | Arity | Purpose |
|---|---:|---|
| `tape-new` | 0 | Create a tape |
| `tape-start!` | 1 | Push tape onto active-tape stack |
| `tape-stop!` | 0 | Pop active tape |
| `tape-clear!` | 1 | Clear entries and bump generation |
| `tape-var` | 2 | Create an independent variable reference |
| `tape-backward!` | 2 | Reverse sweep from output ref |
| `tape-adjoint` | 2 | Read adjoint for a ref |
| `tape-primal` | 2 | Read primal for a ref |
| `tape-size` | 1 | Number of tape entries |
| `tape-ref?` | 1 | Predicate |
| `tape-ref-index` | 1 | Encoded node index field |
| `tape-ref-value-of` | 2 | Explicit primal extraction with tape argument |
| `tape-ref-value` | 1 | Active-tape primal extraction (strict for TapeRefs) |

### AAD policy controls

| Primitive | Arity | Purpose |
|---|---:|---|
| `set-aad-nondiff-policy!` | 1 | Set `strict` or `zero-subgrad` |
| `aad-nondiff-policy` | 0 | Get current policy symbol |

---

## Safety Model

`TapeRef` is a packed immediate with three fields:

- `tape-id`
- `generation`
- `node-index`

At every taped operation and lookup, runtime validation enforces:

- Ownership (`tape-id` must match)
- Lifecycle (`generation` must match current tape generation)
- Bounds (`node-index` must be valid)

This produces deterministic, catchable AD errors instead of silent misuse.

---

## Nested Tape Contract

`active_tape` is stack-based:

1. `tape-start!` pushes.
2. `tape-stop!` pops.
3. Nested tapes are supported.

If control exits through `raise`/`catch`, the VM unwinds tape stack depth with
the catch boundary, so outer tape context remains consistent.

Cross-tape `TapeRef` use is rejected deterministically.

---

## Parallel Contract

AAD values are VM-local:

- `spawn-thread` / `spawn-thread-with`: fresh in-process VM per actor thread.
- `spawn` / `worker-pool`: separate process VM per worker.
- `Tape` and `TapeRef` are not transferable across VM boundaries.

Serializer checks reject `Tape` and `TapeRef` with tag `:ad/cross-vm-ref`.
Callers must extract plain numeric primals before send.

See also: [Network and Message-Passing Parallelism](network-message-passing.md).

---

## Non-Differentiability Policy

Supported modes:

- `strict` (default): reject kinks with `:ad/nondiff-strict`
- `zero-subgrad`: use deterministic zero subgradient at ties/kinks

Kink definitions:

- `abs(x)`: `x == 0`
- `max(a, b)` / `min(a, b)`: `a == b`
- `relu(x)` (via helper): `x == 0`
- `clamp(x, lo, hi)` (via helper): `x == lo` or `x == hi`

Comparison semantics on taped values:

- `strict`: comparison on taped operands raises `:ad/nondiff-strict`
- `zero-subgrad`: taped operands are compared by validated primals

---

## Primitive Coverage and Domain Rules

Taped primitives include:

- Arithmetic: `+`, `-`, `*`, `/`
- Unary math: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `log`, `sqrt`
- Piecewise numerics: `abs`, `min`, `max`
- Binary math: `pow`

Domain behavior for taped primitives:

- `log(x)`: `x > 0`, else `:ad/domain`
- `sqrt(x)`: `x >= 0`, else `:ad/domain`
- `asin(x)`, `acos(x)`: `-1 <= x <= 1`, else `:ad/domain`
- `pow(negative, non-integer exponent)`: `:ad/domain`
- `pow(0, negative)`: `:ad/domain`
- `pow(0, 0)`:
  - `strict`: `:ad/domain`
  - `zero-subgrad`: value `1`, zero subgrad
- `pow(0, positive < 1)`:
  - `strict`: `:ad/domain`
  - `zero-subgrad`: finite forward value with zero subgrad at singular base derivative

The current API remains scalar-focused (no tensor-aware tape extension here).

---

## Stdlib Helpers

`std.aad` provides:

- Piecewise wrappers: `ad-abs`, `ad-max`, `ad-min`, `ad-relu`, `ad-clamp`
- Smooth alternatives: `softplus`, `smooth-abs`, `smooth-clamp`
- Gradient tools: `grad`, `check-grad`, `check-grad-report`
- Checkpoint API: `with-checkpoint` (MVP API surface)

Gradient checker defaults:

- `rtol = 1e-5`
- `atol = 1e-7`
- Central difference step: `h = sqrt(eps) * max(1, |x|)` (scaled by optional `step-scale`)

Tolerance test:

`|aad - fd| <= atol + rtol * |aad|`

---

## AD Error Tags

| Tag | Meaning |
|---|---|
| `:ad/mixed-tape` | refs from different tapes |
| `:ad/stale-ref` | old generation or invalid index |
| `:ad/no-active-tape` | ambient tape API used with no active tape |
| `:ad/nondiff-strict` | strict-mode kink/comparison rejection |
| `:ad/cross-vm-ref` | Tape/TapeRef attempted across VM boundary |
| `:ad/domain` | taped primitive domain violation |

Errors are emitted as `runtime.error` payloads with structured field rows.
Tests should assert tag identity and payload keys, not message text.

---

## Examples

- [examples/aad.eta](../examples/aad.eta) — basic AAD walkthrough
- [European option pricing](european.md) ([source](../examples/european.eta))
- [SABR model](sabr.md) ([source](../examples/sabr.eta))
- [XVA](xva.md) ([source](../examples/xva.eta))
