# Torch Improvements Plan

[<- Back to Plan Index](./scenario_plan.md) | [Torch Reference](../guide/reference/torch.md) | [Fact Table Reference](../guide/reference/fact-table.md)

---

## 1. Goal

Add three torch/fact-table improvements needed by the NOTEARS-style workflow:

1. `torch/matrix-exp` availability in `std.torch` (P2 follow-up).
2. `fact-table -> tensor` bridge helper `(fact-table->tensor ft col-idxs)`.
3. `torch:column-l2-norm` helper (axis-aware).

Immediate delivery should unblock DAG-learning without waiting on `matrix-exp` binding by using Yu et al. (2019) truncated acyclicity approximation:

\[
\tilde h(W) = \mathrm{tr}\left(\left(I + \frac{1}{d} (W \circ W)\right)^d\right) - d
\]

---

## 2. Scope and Decisions

### In scope

- Add thin runtime and stdlib APIs for the three items above.
- Keep default tensor dtype as `float64` to match existing torch primitives.
- Add tests in both C++ (`eta/test/src`) and Eta stdlib tests (`stdlib/tests`).

### Out of scope

- Broader refactors of torch primitive architecture.
- CUDA-specific behavior changes.
- End-to-end NOTEARS training tests in this plan.

---

## 3. Workstreams

### WS1 -- `torch/matrix-exp` binding (P2 follow-up)

#### Summary

Expose libtorch `matrix_exp` as `torch/matrix-exp`, then alias it from `std.torch`.

#### Implementation steps

1. **Runtime primitive registration**
   - File: `eta/torch/src/eta/torch/torch_primitives.h`
   - Add builtin:
     - Name: `torch/matrix-exp`
     - Arity: `1`
     - Contract: tensor input, return tensor output
     - Backend call: `::torch::linalg_matrix_exp(t->tensor)` (or `::torch::matrix_exp` if preferred by current libtorch version in repo).
   - Error shape:
     - `"torch/matrix-exp: tensor required"`
     - Catch `c10::Error` and wrap with `"torch/matrix-exp: ..."`

2. **Builtin names parity**
   - File: `eta/core/src/eta/runtime/builtin_names.h`
   - Add `r("torch/matrix-exp", 1, false);` in torch primitive section in slot order alignment with runtime registration.

3. **Stdlib alias**
   - File: `stdlib/std/torch.eta`
   - Export `matrix-exp`.
   - Define `(define matrix-exp torch/matrix-exp)`.

4. **Docs touch-up**
   - File: `docs/guide/reference/torch.md`
   - Add one-line entry for matrix exponential in tensor ops list.

#### Tests

**C++ tests (`eta/test/src/torch_tests.cpp`)**

- `prim_torch_matrix_exp_via_env`
  - Construct a small 2x2 tensor via libtorch.
  - Call builtin through env lookup.
  - Assert output shape is `(2,2)`.
  - Numeric sanity: `matrix_exp(zeros)` is identity.

- `builtin_name_matrix_exp_registered`
  - Ensure `env.lookup("torch/matrix-exp")` succeeds after primitive registration.
  - Verify name/arity parity check continues passing.

**Eta tests (`stdlib/tests/torch.test.eta`)**

- `matrix-exp of zero matrix is identity`
  - Build `2x2` zero matrix.
  - `(matrix-exp z)` should have diagonal near `1.0`, off-diagonal near `0.0`.

- `matrix-exp preserves shape`
  - Input shape `(n n)` => output shape `(n n)`.

---

### WS2 -- `fact-table->tensor` bridge

#### Summary

Add `(fact-table->tensor ft col-idxs)` producing a `(rows x k)` `float64` tensor where `k = (length col-idxs)`.

#### API behavior

- Accepts:
  - `ft`: fact-table
  - `col-idxs`: proper list of non-negative integer column indices
- Returns:
  - torch tensor shaped `(fact-table-row-count ft, length(col-idxs))`
- Numeric policy:
  - Numeric cells accepted (fixnum/flonum)
  - Non-numeric cell => error
- Row policy:
  - Use live rows (matches `%fact-table-row-count` / std.fact_table behavior)

#### Implementation option

   - File: `eta/torch/src/eta/torch/torch_primitives.h`
   - Add builtin `%`-style or direct public builtin (recommended public: `torch/fact-table->tensor`).
   - Access fact-table columns directly and fill contiguous `torch::Tensor` in row-major order.
   - Register alias in `std.torch` as `fact-table->tensor`.
   - This avoids per-cell VM overhead.

#### Implementation steps 
   - `torch/matrix-exp` (when P2 lands)
3. Builtin name/arity parity tests continue passing after each primitive addition.
4. `torch_improvements.md` implementation steps are reflected in touched files and test suites.

---

## 4. Implementation Status (2026-05-01)

- WS1 implemented:
  - `torch/matrix-exp` runtime primitive registered.
  - builtin name parity updated.
  - `std.torch` exports and aliases `matrix-exp`.
  - torch reference docs updated.
  - C++ and Eta tests added for identity and shape behavior.

- WS2 implemented:
  - `torch/fact-table->tensor` runtime primitive registered.
  - `std.torch` exports and aliases `fact-table->tensor`.
  - torch reference docs updated for fact-table bridge helper.
  - C++ and Eta tests added for live-row handling and column order.

- WS3 implemented:
  - `torch/column-l2-norm` runtime primitive registered (axis-aware).
  - `std.torch` exports both `column-l2-norm` and `torch:column-l2-norm`.
  - torch reference docs updated for axis-aware L2 norm helper.
  - C++ and Eta tests added for numeric correctness and alias parity.

- Validation gate used after each workstream and at the end:
  - Build: `cmake --build C:\Users\lewis\develop\eta\out\msvc-release --target eta_all -j 14`
  - Tests: `ctest --test-dir C:\Users\lewis\develop\eta\out\msvc-release --output-on-failure`
  - Result: all tests passed (`eta_core_test`, `eta_stdlib_tests`) at each gate.
