# Eigen — Linear Algebra Backend

[← Back to Language Guide](../../language_guide.md)

[Eigen](https://eigen.tuxfamily.org/) is the C++ template library that
powers Eta's dense linear algebra. It is a **backend**, not a
user-facing module: there is no `(import std.eigen)` form. Instead,
the runtime uses Eigen internally for the numerics in:

| Module                       | What Eigen does                                  |
| :--------------------------- | :----------------------------------------------- |
| [`std.stats`](./stats.md)             | OLS / GLS regression, PCA, covariance, descriptive statistics |
| [`std.torch`](./torch.md)             | Light-weight tensor ops on the CPU path          |
| [`std.clpr`](./clpr.md)      | LP / QP solver workspace                         |
| [`std.aad`](./aad.md)                 | Vector primal / adjoint storage                  |

This page documents what that means in practice, what guarantees the
backend gives, and how to read the relevant performance trade-offs.

---

## Why a backend, not a module

Eta values are NaN-boxed; double-precision matrices are not. Surfacing
Eigen's `Matrix<double>` directly would force every cell access through
a boxing/unboxing trampoline. Instead the runtime keeps Eigen objects
behind opaque handles (`stats:matrix`, `torch:tensor`) and exposes a
high-level operation surface that batches work on the C++ side.

Result: the user-facing modules feel like Eta values, while the heavy
lifting (BLAS-level loops, vectorised intrinsics, multi-threading)
happens in compiled C++.

---

## Where Eigen shows up

### `std.stats`

Regression, decompositions, and projections all return Eta values
(numbers, lists, vectors) but compute on Eigen matrices internally:

```scheme
(import std.stats)
(stats:ols y X)            ; least squares: returns coefficients + diagnostics
(stats:pca X 'k 3)         ; top-3 PCA via Eigen's SVD
```

See [`stats.md`](./stats.md) for the full surface.

### `std.torch`

CPU tensor operations route through Eigen for small / medium tensors;
larger ones hand off to libtorch's kernels. The choice is internal —
user code is unchanged.

### `std.clpr`

The LP / QP oracle uses Eigen for its inner-loop linear algebra
(pivoting, projection, KKT solves). See [CLP(R)](./clpr.md).

### `std.aad`

Reverse-mode AD stores primal and adjoint vectors as
`Eigen::VectorXd` so the tape's BLAS-1 sweeps can be vectorised.

---

## Build configuration

Eigen is fetched via CMake at configure time
(see [`cmake/FetchEigen.cmake`](../../../cmake/FetchEigen.cmake)).
There is no Eta-side configuration: the same binary uses Eigen for all
subsystems.

Optional knobs surfaced through CMake:

| Variable                 | Default | Meaning                                  |
| :----------------------- | :------ | :--------------------------------------- |
| `EIGEN_USE_THREADS`      | OFF     | Enable OpenMP threading inside Eigen     |
| `EIGEN_USE_BLAS`         | OFF     | Defer to a vendor BLAS (MKL, OpenBLAS)   |

For the standard release builds, neither is enabled — the workloads
addressed by `std.stats` and `std.clpr` are typically too small to
benefit from a vendor BLAS.

---

## Performance notes

- **Allocation** dominates small-matrix work. Where possible the
  bindings reuse a thread-local Eigen scratch buffer rather than
  allocating per call.
- **SIMD width** is whatever Eigen detects at compile time
  (SSE2/AVX/AVX2 on x86; NEON on ARM).
- **Determinism**: Eigen's deterministic mode is on by default — repeat
  runs of `(stats:ols y X)` produce identical bits.

---

## When to drop into libtorch instead

Use [`std.torch`](./torch.md) when you need:

- GPU acceleration,
- gradient tracking on tensor-shaped objects,
- the `nn` layer / optimiser ecosystem.

Stay with the Eigen-backed modules (`std.stats`, `std.clpr`) when:

- the working set fits in CPU cache,
- the operation is statistical / linear-algebraic rather than learned,
- you want the Eta integers / floats round-trip without GPU sync.

---

## Related

- [`stats.md`](./stats.md), [`torch.md`](./torch.md)
- [CLP(R)](./clpr.md), [`aad.md`](./aad.md)
- [`architecture.md`](../../architecture.md)


