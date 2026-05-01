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

#### Implementation options

1. **Phase A (fast to ship): stdlib thin helper**
   - File: `stdlib/std/torch.eta` (or `stdlib/std/fact_table.eta`)
   - Implement nested loops over row/column using `%fact-table-ref`.
   - Build row-major list and call `from-list`, then `reshape`.
   - Lowest implementation risk, slower for large tables.

2. **Phase B (preferred performance): C++ primitive**
   - File: `eta/torch/src/eta/torch/torch_primitives.h`
   - Add builtin `%`-style or direct public builtin (recommended public: `torch/fact-table->tensor`).
   - Access fact-table columns directly and fill contiguous `torch::Tensor` in row-major order.
   - Register alias in `std.torch` as `fact-table->tensor`.
   - This avoids per-cell VM overhead.

#### Implementation steps (Phase B preferred)

1. Add helper to decode list of column indices in C++ primitive.
2. Validate `ft` type and each column index range.
3. Allocate tensor:
   - `auto out = ::torch::empty({rows, k}, ::torch::kFloat64);`
4. Fill tensor:
   - Iterate chosen columns.
   - For each live row, numeric decode and write to `out[row_i][col_j]`.
5. Return wrapped tensor via `factory::make_tensor`.
6. Register builtin name in `builtin_names.h`.
7. Add `std.torch` export/alias:
   - `fact-table->tensor`.

#### Tests

**C++ tests (`eta/test/src/torch_tests.cpp`)**

- `prim_torch_fact_table_to_tensor_via_env_shape`
  - Build fact-table in runtime.
  - Insert a few rows.
  - Convert selected columns.
  - Assert shape `(rows, selected-cols)`.

- `prim_torch_fact_table_to_tensor_values`
  - Validate expected row-major numeric values in output tensor.

- `prim_torch_fact_table_to_tensor_rejects_non_numeric`
  - Insert non-numeric value in selected column and assert error.

- `prim_torch_fact_table_to_tensor_rejects_bad_col_index`
  - Out-of-range index should error cleanly.

**Eta tests**

- File: `stdlib/tests/torch.test.eta` (or new `stdlib/tests/torch_fact_table_bridge.test.eta`)

- `fact-table->tensor basic conversion`
  - Build fact-table with 3 rows x 3 cols.
  - Convert `(0 2)` and assert shape `(3 2)`.
  - Assert flattened value order matches row-major expectation.

- `fact-table->tensor respects live row count`
  - Tombstone one row via `%fact-table-delete-row!`.
  - Assert output row count equals `fact-table-row-count`.

- `fact-table->tensor fails on non-numeric column`
  - Ensure clear runtime error.

---

### WS3 -- `torch:column-l2-norm` helper (axis-aware)

#### Summary

Expose L2 norm reduction along a specified axis.

#### API

- In `std.torch`: `(column-l2-norm t axis)`
- Semantics:
  - Return `sqrt(sum(t * t, axis))`
  - For matrix + `axis=0`: column-wise norms
  - For matrix + `axis=1`: row-wise norms

#### Implementation choices

1. **Pure wrapper only** (blocked by current runtime):
   - Current `torch/sum` has arity `1` only; no axis argument.
2. **Minimal runtime extension (recommended)**:
   - Add `torch/sum-dim` primitive (tensor, dim).
   - Implement `column-l2-norm` in `std.torch` from `t*`, `torch/sum-dim`, `tsqrt`.

#### Implementation steps

1. Runtime:
   - File: `eta/torch/src/eta/torch/torch_primitives.h`
   - Add `torch/sum-dim` arity `2`.
   - Validate tensor + integer dim.
2. Names:
   - File: `eta/core/src/eta/runtime/builtin_names.h`
   - Add `r("torch/sum-dim", 2, false);`
3. Stdlib:
   - File: `stdlib/std/torch.eta`
   - Export `column-l2-norm`.
   - Define:
     - `(defun column-l2-norm (t axis) (tsqrt (torch/sum-dim (t* t t) axis)))`

#### Tests

**C++ tests (`eta/test/src/torch_tests.cpp`)**

- `prim_torch_sum_dim_via_env`
  - 2x3 tensor with known values.
  - `sum-dim` over axis 0 and 1, assert expected values.

- `prim_torch_sum_dim_bad_dim`
  - Non-integer or out-of-range dimension returns error.

**Eta tests (`stdlib/tests/torch.test.eta`)**

- `column-l2-norm axis=0 on 2x2`
  - Input `[[3,4],[0,0]]`, axis `0` => `(3,4)`.

- `column-l2-norm axis=1 on 2x2`
  - Same input, axis `1` => `(5,0)`.

---

## 4. Delivery Phases

### Phase 1 (unblock now)

- Use Yu et al. truncated acyclicity approximation in NOTEARS path.
- Implement `fact-table->tensor` (Phase A helper or Phase B primitive).
- Implement axis-aware `column-l2-norm` via `torch/sum-dim`.

### Phase 2 (P2 follow-up)

- Add `torch/matrix-exp` binding and stdlib alias.
- Keep approximation path available for reproducibility and performance comparisons.

---

## 5. Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Builtin registration order drift vs `builtin_names.h` | Runtime/name parity failures | Add/update parity tests in `torch_tests.cpp`; keep additions in matching order. |
| Bridge performance regressions on large tables | Slower DAG training input pipeline | Prefer Phase B C++ primitive for columnar copy; keep helper only as fallback. |
| Axis semantics confusion | Wrong norm vectors in model code | Add explicit axis tests (`0` and `1`) in Eta and C++. |
| Numeric type surprises in fact-table | Runtime errors in training | Enforce numeric validation and explicit error messages. |

---

## 6. Acceptance Criteria

1. `std.torch` exposes `fact-table->tensor` and `column-l2-norm` with passing Eta tests.
2. C++ primitive tests pass for:
   - `torch/sum-dim`
   - fact-table bridge primitive (if Phase B implemented)
   - `torch/matrix-exp` (when P2 lands)
3. Builtin name/arity parity tests continue passing after each primitive addition.
4. `torch_improvements.md` implementation steps are reflected in touched files and test suites.
