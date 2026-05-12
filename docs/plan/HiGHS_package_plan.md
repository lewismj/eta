# HiGHS Native Package Plan

[Back to README](../../README.md) ·
[Stdlib Reference](../stdlib.md) ·
[Architecture](../architecture.md) ·
[Packaging](../packaging.md)

> **Status.** Authoritative implementation plan for shipping the
> [HiGHS](https://highs.dev) LP / MIP / QP solver as an Eta native
> sidecar package. An implementer should be able to execute this plan
> end-to-end without any other planning document.

---

## 1) Goals and non-goals

### Goals

1. Provide a first-class, production-grade **LP / MIP / QP solver** to
   Eta programs as the package `optim.highs`, exposed under the
   `optim/` package family.
2. Ship as a **sidecar native package** (mirroring `ml.lightgbm`),
   not as a runtime built-in: the core interpreter must remain
   buildable and runnable without HiGHS installed.
3. Cover the working surface most quant / OR users need: continuous
   LP, mixed-integer LP, convex QP, basic warm-starting, presolve
   control, time/iteration limits, and structured solution readback
   (status, objective, primal, dual, basis).
4. Provide an **idiomatic Eta builder API** (`make-model`,
   `add-var!`, `add-constraint!`, `set-objective!`, `solve`) that hides
   HiGHS's stateful C API, plus a **thin pass-through API**
   (`hi/run-from-mps`, `hi/load-lp`, …) for users who want raw control.
5. Keep packaging cost low: HiGHS is MIT, pure C++17, builds via its
   own CMake — `FetchHiGHS.cmake` lives entirely inside the package
   directory and slots into the existing native-package toolchain
   used by `eta-lightgbm`.
6. Ship at least three cookbook examples that demonstrate
   **measurable** uplift over the existing `std.clpr` simplex:
   a rewritten `portfolio-lp`, a mixed-integer `scheduling`, and a
   convex `mean-variance` QP.

### Non-goals

1. **Not** replacing or removing `std.clpr` / the in-tree simplex in
   `eta/core/src/eta/runtime/clp/`. CLP(R) stays as the
   logic-language-integrated solver; HiGHS is the heavyweight numeric
   path. The two coexist; cookbook docs explain when to choose which.
2. **Not** providing nonlinear (NLP), conic (SOCP/SDP), or global
   non-convex solvers. HiGHS does not solve these; users wanting
   them get a future `optim.ipopt` / `optim.scs` package.
3. **Not** auto-importing `optim.highs` from the prelude. Native
   packages are opt-in via `(import optim.highs)`.
4. **Not** vendoring HiGHS sources into the Eta repo. Like LightGBM,
   the upstream is fetched via `FetchHiGHS.cmake` only when the
   package opts in (`-DETA_HIGHS_FETCH_UPSTREAM=ON`); otherwise the
   build looks for a system install.
5. **Not** writing a Scheme-level NLP modelling DSL in v1. The model
   builder is matrix-/coefficient-shaped, not symbolic. A symbolic
   `defmodel` macro layer can come later.

---

## 2) Where this slots in the codebase

The package follows the established `eta-lightgbm` pattern exactly.

| Concern                                  | Path                                                                      | Action |
| ---------------------------------------- | ------------------------------------------------------------------------- | ------ |
| Package root                             | `packages/numerics/native/highs/`                                         | NEW    |
| Package manifest                         | `packages/numerics/native/highs/eta.toml`                                 | NEW    |
| Package CMake driver                     | `packages/numerics/native/highs/CMakeLists.txt`                           | NEW    |
| Upstream fetch helper                    | `packages/numerics/native/highs/cmake/FetchHiGHS.cmake`                   | NEW    |
| Sidecar staging helper                   | `packages/numerics/native/highs/cmake/StageHiGHSSidecar.cmake`            | NEW    |
| Native extension entry                   | `packages/numerics/native/highs/src/eta/highs/highs_extension.cpp`        | NEW    |
| Native primitive bindings                | `packages/numerics/native/highs/src/eta/highs/highs_primitives.{h,cpp}`   | NEW    |
| Model wrapper (RAII over `Highs`)        | `packages/numerics/native/highs/src/eta/highs/highs_model.{h,cpp}`        | NEW    |
| Eta module wrapper                       | `packages/numerics/native/highs/src/optim/highs.eta`                      | NEW    |
| Pre-built artefacts                      | `packages/numerics/native/highs/libs/{amd64,arm64}/`                      | NEW    |
| Eta smoke test                           | `packages/numerics/native/highs/tests/eta/highs_smoke.test.eta`           | NEW    |
| Eta smoke test driver                    | `packages/numerics/native/highs/tests/eta/run_highs_eta_smoke.cmake`      | NEW    |
| C++ unit tests                           | `packages/numerics/native/highs/tests/unit/highs_model_tests.cpp`         | NEW    |
| Package README                           | `packages/numerics/native/highs/README.md`                                | NEW    |
| Stdlib reference doc                     | `docs/stdlib/optim-highs.md`                                              | NEW    |
| Stdlib index                             | `docs/stdlib.md`                                                          | EDIT   |
| Cookbook entry — LP rewrite              | `cookbook/numerics/portfolio-lp-highs.eta`                                | NEW    |
| Cookbook entry — MIP                     | `cookbook/numerics/scheduling-mip.eta`                                    | NEW    |
| Cookbook entry — QP                      | `cookbook/numerics/mean-variance-qp.eta`                                  | NEW    |
| Cookbook README index                    | `cookbook/numerics/README.md` (if present) or top-level cookbook index    | EDIT   |
| Top-level package list                   | `packages/README.md` (if present)                                         | EDIT   |
| Architecture doc — solvers section       | `docs/architecture.md`                                                    | EDIT (one paragraph) |
| Build doc — optional packages list       | `docs/build.md`                                                           | EDIT (one bullet) |
| Release-notes entry                      | `docs/release-notes.md`                                                   | EDIT (one bullet) |

The new top-level group `packages/numerics/` is created by this plan.
It mirrors `packages/ml/`, and a future `optim.ipopt` /
`optim.scs` would sit alongside as siblings.

---

## 3) Why HiGHS, briefly

1. **License: MIT.** Compatible with everything Eta already ships
   (Eigen, csv-parser, nng, spdlog) and with the project's
   distribution model.
2. **Pure C++17, CMake-native.** Slots into `FetchContent` cleanly;
   no Fortran, no system BLAS dependency for the LP/MIP path
   (HiGHS ships its own LU and presolve).
3. **Stable, plain C API** (`interfaces/highs_c_api.h`) — the
   sidecar binds against `Highs.h` (C++) for richer access but the
   C API is the fallback if C++-ABI portability becomes an issue.
4. **Best-in-class open LP/MIP.** Independently the leading open LP
   solver and the strongest open MIP solver; the QP path covers
   convex separable / sparse-Hessian problems, which fits portfolio
   optimisation and ridge / isotonic regression.
5. **Coexists with `std.clpr`.** The existing in-tree simplex/
   Fourier–Motzkin/QP code in `eta/core/src/eta/runtime/clp/` was
   built for tight integration with the logic engine (constraint
   posting, witness extraction, unification). HiGHS offers an
   order-of-magnitude scale upgrade for *standalone* numeric
   optimisation — the two are complementary, not competitive.

---

## 4) Upstream pin and fetch

### 4.1 Version

| Setting                | Value                          |
| ---------------------- | ------------------------------ |
| `ETA_HIGHS_VERSION`    | `1.11.0` (latest stable as of 2026-05) |
| `ETA_HIGHS_GIT_TAG`    | `v1.11.0`                      |
| Source                 | `https://github.com/ERGO-Code/HiGHS.git` |
| License                | MIT                            |
| C++ standard required  | C++17 (Eta uses C++23, fine)   |

### 4.2 `FetchHiGHS.cmake`

Mirrors `cmake/FetchEigen.cmake` and the lightgbm package's
`FetchLightGBM.cmake`:

```cmake
include(FetchContent)

set(HIGHS_BUILD_SHARED      ON  CACHE BOOL "" FORCE)
set(BUILD_TESTING           OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES          OFF CACHE BOOL "" FORCE)
set(PYTHON                  OFF CACHE BOOL "" FORCE)
set(FORTRAN                 OFF CACHE BOOL "" FORCE)
set(CSHARP                  OFF CACHE BOOL "" FORCE)
set(JULIA                   OFF CACHE BOOL "" FORCE)
set(ZLIB                    OFF CACHE BOOL "" FORCE)
set(HIGHS_NO_DEFAULT_THREADS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    highs
    GIT_REPOSITORY https://github.com/ERGO-Code/HiGHS.git
    GIT_TAG        v1.11.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)

set(_eta_prev_warn "${CMAKE_WARN_DEPRECATED}")
set(CMAKE_WARN_DEPRECATED OFF)
FetchContent_MakeAvailable(highs)
set(CMAKE_WARN_DEPRECATED "${_eta_prev_warn}")
unset(_eta_prev_warn)

message(STATUS "HiGHS 1.11.0 fetched — highs:: targets available")
```

The package's top-level `CMakeLists.txt` gates this behind
`ETA_HIGHS_FETCH_UPSTREAM` so CI / system-install builds skip the
fetch and use `find_package(HIGHS)`.

### 4.3 System-install fallback

```cmake
find_package(HIGHS QUIET CONFIG)
if(NOT HIGHS_FOUND AND ETA_HIGHS_FETCH_UPSTREAM)
    include(cmake/FetchHiGHS.cmake)
endif()
if(NOT TARGET highs::highs)
    message(FATAL_ERROR
        "HiGHS not found. Re-configure with -DETA_HIGHS_FETCH_UPSTREAM=ON"
        " or install HiGHS (>=1.11) and ensure HiGHSConfig.cmake is on"
        " CMAKE_PREFIX_PATH.")
endif()
```

---

## 5) Native sidecar — C++ surface

### 5.1 File layout

Mirrors `packages/ml/native/lightgbm/src/eta/lightgbm/`:

```
src/eta/highs/
    highs_extension.cpp     # eta_register_highs_extension_v1 entrypoint
    highs_primitives.h      # primitive declarations + registration helper
    highs_primitives.cpp    # primitive bodies — argument unpack, dispatch
    highs_model.h           # RAII wrapper over `Highs` instance
    highs_model.cpp
```

### 5.2 Sidecar manifest (`eta.toml`)

```toml
[package]
name = "eta-highs"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.0, <0.8"

[native]
kind   = "sidecar"
abi    = "eta-native-v1"
id     = "eta.highs.sidecar"
entry  = "eta_register_highs_extension_v1"

[[native.targets]]
triple   = "x86_64-pc-windows-msvc"
artifact = "libs/amd64/eta_highs.dll"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"

[[native.targets]]
triple   = "x86_64-unknown-linux-gnu"
artifact = "libs/amd64/libeta_highs.so"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"

[[native.targets]]
triple   = "aarch64-unknown-linux-gnu"
artifact = "libs/arm64/libeta_highs.so"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"

[[native.targets]]
triple   = "x86_64-apple-darwin"
artifact = "libs/amd64/libeta_highs.dylib"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"

[[native.targets]]
triple   = "aarch64-apple-darwin"
artifact = "libs/arm64/libeta_highs.dylib"
sha256   = "0000000000000000000000000000000000000000000000000000000000000000"
```

SHA-256 placeholders are filled in once each platform's release
artefact is built and frozen; the lockfile is generated by the
existing `eta.lock` flow used by `eta-lightgbm`.

### 5.3 Extension entrypoint

`highs_extension.cpp` defines a single exported symbol:

```cpp
extern "C" ETA_NATIVE_EXPORT
int eta_register_highs_extension_v1(eta_native_registry_v1* reg);
```

Body just calls
`eta::highs::register_primitives(reg)` from `highs_primitives.cpp`
and returns `ETA_NATIVE_OK`. Identical structure to
`lightgbm_extension.cpp`.

### 5.4 Primitive surface

All primitives are namespaced `hi/` in Scheme-land (the analogue of
`lgbm/`). They are intentionally **low-level**; the user-facing
ergonomic API lives in `optim/highs.eta` (§6).

| Primitive                              | Arity | Description |
| -------------------------------------- | :---: | ----------- |
| `hi/version`                           | 0     | Returns HiGHS version string. |
| `hi/model-new`                         | 0     | Allocates a new `highs-model` foreign handle (RAII-owned `Highs*`). |
| `hi/model-clear!`                      | 1     | Resets a model in place. |
| `hi/model-add-col!`                    | 5     | `(model lower upper objective integrality)` — appends one variable; `integrality` ∈ `{0,1,2}` (continuous, integer, semi-continuous). Returns column index. |
| `hi/model-add-row!`                    | 4     | `(model lower upper coeffs)` where `coeffs` is `((col-idx . value) …)`. Returns row index. |
| `hi/model-set-objective-sense!`        | 2     | `(model sense)` — `sense` ∈ `{'min, 'max}`. |
| `hi/model-set-objective-offset!`       | 2     | Constant offset added to objective. |
| `hi/model-set-hessian!`                | 2     | `(model triplets)` — `triplets` is `((row col value) …)` for the **lower-triangular** Hessian; switches the model to QP. |
| `hi/model-set-option!`                 | 3     | `(model name value)` — typed dispatch on `value` (bool / int / double / string). |
| `hi/model-get-option`                  | 2     | `(model name) -> value`. |
| `hi/model-write`                       | 2     | `(model path)` — writes MPS / LP based on extension. |
| `hi/model-read!`                       | 2     | `(model path)` — loads MPS / LP into the (empty) model. |
| `hi/run!`                              | 1     | Solve. Returns a status symbol (see §5.6). |
| `hi/solution-objective`                | 1     | Objective value of the incumbent solution. |
| `hi/solution-primal`                   | 1     | Vector of primal variable values. |
| `hi/solution-dual`                     | 1     | Vector of row duals (LP only). |
| `hi/solution-reduced-costs`            | 1     | Vector of column reduced costs. |
| `hi/solution-row-activity`             | 1     | Vector of row activities `Ax`. |
| `hi/solution-basis-cols`               | 1     | Vector of column basis statuses (`'lower 'basic 'upper 'zero 'nonbasic`). |
| `hi/solution-basis-rows`               | 1     | Vector of row basis statuses. |
| `hi/info-iterations`                   | 1     | Simplex / IPM / B&B iteration count. |
| `hi/info-runtime`                      | 1     | Wall-clock seconds. |
| `hi/info-mip-gap`                      | 1     | Relative gap (MIP only); `+inf` for LP. |
| `hi/model-num-cols`                    | 1     | Diagnostic. |
| `hi/model-num-rows`                    | 1     | Diagnostic. |
| `hi/model?`                            | 1     | Foreign-handle predicate. |

### 5.5 Foreign handle representation

`highs-model` is an **opaque foreign object** allocated through the
sidecar SDK's existing foreign-pointer machinery (the same channel
`lgbm-dataset` and `lgbm-booster` use; see
`eta/core/src/eta/native/sdk.h`). It owns:

```cpp
struct HighsModel {
    std::unique_ptr<Highs> impl;
    HighsLp staged_lp;          // accumulated cols/rows before run
    HighsHessian staged_hess;   // accumulated QP Hessian, if any
    HighsModelStatus last_status = HighsModelStatus::kNotset;
    bool dirty_lp = false;      // true between mutation and run
};
```

Mutating primitives operate on the staged matrices; `hi/run!` flushes
them via `passModel(...)` then calls `Highs::run()`. This keeps
incremental builds O(1) per add and avoids round-tripping through the
solver state on every `add-row!`. (`Highs::addCol` / `addRow` are
also acceptable; the staging buffer is purely for batching.)

### 5.6 Status mapping

`hi/run!` and `hi/solution-*` translate HiGHS C++ enums to Scheme
symbols so callers never see raw ints:

| `HighsModelStatus`            | Symbol                |
| ----------------------------- | --------------------- |
| `kOptimal`                    | `'optimal`            |
| `kInfeasible`                 | `'infeasible`         |
| `kUnboundedOrInfeasible`      | `'unbounded-or-infeasible` |
| `kUnbounded`                  | `'unbounded`          |
| `kObjectiveBound`             | `'objective-bound`    |
| `kObjectiveTarget`            | `'objective-target`   |
| `kTimeLimit`                  | `'time-limit`         |
| `kIterationLimit`             | `'iteration-limit`    |
| `kSolutionLimit`              | `'solution-limit`     |
| `kInterrupt`                  | `'interrupt`          |
| `kModelEmpty`                 | `'model-empty`        |
| `kPostsolveError`             | `'postsolve-error`    |
| `kModelError` / `kSolveError` | raises an Eta error   |

Basis statuses are translated similarly. Any HiGHS return code below
`kOk` becomes a Scheme `(error …)` carrying the HiGHS log line as the
message.

### 5.7 Threading and logging

1. The sidecar is **per-model thread-safe by design**: each
   `highs-model` handle owns its own `Highs` instance, and HiGHS
   itself is documented as not thread-safe across instances. The
   wrapper does not add any extra locking; concurrent solves on
   distinct handles are safe.
2. HiGHS log output is rerouted through the existing sidecar log
   channel (the same `spdlog` sink lightgbm uses) by registering a
   custom `HighsLogCallback`. The default is `info`; an option
   `'log-level` ∈ `{'silent 'error 'warning 'info 'debug}` controls it.
3. Time / iteration limits are passed through `setOptionValue` and
   surface as the corresponding status symbols in §5.6.

---

## 6) Eta-side surface — `optim.highs`

### 6.1 Public API

The high-level builder API lives in `src/optim/highs.eta` and is
what the cookbook examples import.

```scheme
(module optim.highs
  (export
    ;; Model construction
    make-model
    add-var!  add-int-var!  add-binary-var!
    add-constraint!  add-eq!  add-le!  add-ge!  add-range!
    set-objective!  set-objective-offset!  set-sense!
    set-hessian!
    ;; Options
    set-option!  get-option
    ;; Solving
    solve  solve-from-mps  solve-from-lp
    ;; Solution access (record-shaped)
    solution-status  solution-objective  solution-values
    solution-duals   solution-reduced-costs  solution-row-activity
    solution-basis   solution-iterations  solution-runtime
    solution-mip-gap
    ;; Re-export the raw layer for advanced use
    hi))
```

### 6.2 Builder semantics

```scheme
(define m (make-model 'min))                     ; sense is required
(define x (add-var!  m 0.0 1.0  0.15))           ; lo, hi, obj-coef
(define y (add-binary-var! m 0.10))              ; obj coef only
(add-constraint! m '<= 0.45 (list (cons x 0.6) (cons y 0.3)))
(add-eq!         m 1.0     (list (cons x 1.0) (cons y 1.0)))
(set-option! m 'time-limit 30.0)
(define sol (solve m))
(solution-status sol)        ; => 'optimal
(solution-objective sol)     ; => 0.135
(solution-values sol)        ; => #(0.5 0.5)
```

`add-var!` / `add-int-var!` / `add-binary-var!` return the column
index so callers can build constraint coefficient lists with
symbolic-style references. `add-constraint!` accepts an explicit
`'<=`, `'>=`, `'=` operator plus rhs and a coefficient alist; the
specialised `add-le!` / `add-ge!` / `add-eq!` / `add-range!` are
sugar.

`solve` returns an opaque `solution` record; all `solution-*`
accessors are pure reads of cached values copied out of the
`highs-model` handle at solve time, so the model can be mutated and
re-solved without invalidating prior solutions.

### 6.3 Pass-through API

For users who already have an MPS / LP file, `solve-from-mps` and
`solve-from-lp` short-circuit the builder:

```scheme
(define sol (solve-from-mps "model.mps"
                            '((presolve . on)
                              (time-limit . 60.0))))
```

### 6.4 Conventions matching the rest of the stdlib

1. Predicates end in `?` (`model?`, `solution?`).
2. Mutators end in `!` (`add-var!`, `set-option!`).
3. Symbol-keyed options match HiGHS option names with `_` →`-`
   normalisation (`'time-limit` ↔ `time_limit`,
   `'mip-rel-gap` ↔ `mip_rel_gap`).
4. Errors raised via `(error "optim.highs: <message>")`, matching
   `std.bitset` and `ml.lightgbm` conventions.

---

## 7) Test plan

### 7.1 C++ unit tests (`tests/unit/highs_model_tests.cpp`)

Built unconditionally when the package CMake target is configured,
mirroring `lightgbm_model_tests.cpp`:

1. `model-roundtrip` — build a 2-var LP, solve, assert
   `kOptimal` + objective within `1e-9` of the analytical optimum.
2. `mip-roundtrip` — set integrality on one column, assert
   integer-valued solution.
3. `qp-roundtrip` — set a small Hessian, assert the QP optimum
   matches the closed-form solution.
4. `infeasible` — contradictory bounds; expect `kInfeasible`.
5. `unbounded` — unbounded LP; expect `kUnbounded`.
6. `time-limit` — set `time_limit = 0.0` on a non-trivial model;
   expect `kTimeLimit`.
7. `option-roundtrip` — set then read `presolve`, `time_limit`,
   `mip_rel_gap`; assert the values are returned intact.

### 7.2 Eta smoke test (`tests/eta/highs_smoke.test.eta`)

Runs through the existing `run_*_eta_smoke.cmake` driver pattern.
Imports `optim.highs`, exercises:

1. `make-model` returns a `model?`.
2. The README quickstart LP (the §6.2 example) returns
   `solution-objective` ≈ `0.135` to `1e-9`.
3. A 3-binary scheduling micro-MIP returns `'optimal` and a
   permutation that respects the precedence constraints.
4. A 2-variable QP (`min ½(x²+y²) s.t. x+y=1`) returns
   `(0.5 0.5)`, objective `0.25`.
5. Reading a fixture MPS file via `solve-from-mps` gives the same
   answer as the equivalent builder code.
6. `set-option! m 'time-limit 0.0` then `solve` returns
   `'time-limit`.

Fixtures live under `tests/eta/fixtures/` and are tiny
(< 5 KB total).

### 7.3 Cookbook examples double as integration tests

`cookbook/numerics/portfolio-lp-highs.eta` is wired into the same
cookbook smoke pass that already runs the existing
`portfolio-lp.eta`, so the end-to-end path (etac compile → load
sidecar → solve → print) is exercised on every CI run.

---

## 8) Documentation deliverables

### 8.1 `docs/stdlib/optim-highs.md`

Follows the shape of `docs/stdlib/torch.md` / the planned
`docs/stdlib/bitset.md`:

1. One-line summary, license note, link to upstream.
2. **Synopsis** code block — the §6.2 quickstart, plus a one-line
   "when to use this vs `std.clpr`" callout.
3. Section per category: Construction, Variables, Constraints,
   Objective / Hessian, Options, Solving, Solution accessors,
   Status symbols, Errors. Sourced verbatim from §5–§6.
4. **Performance notes** — presolve, threading, MIP-gap tuning.
5. **Limitations** — convex QP only; no nonlinear; no warm-start
   across model topology changes in v1.

### 8.2 `docs/stdlib.md`

Insert under a new "Optimization" section:

```
- [optim.highs](stdlib/optim-highs.md) — LP / MIP / QP solver
  (HiGHS native sidecar). Opt-in via the `eta-highs` package.
```

### 8.3 `cookbook/numerics/`

Three new examples; each one is a self-contained `(module …)` with
the same `;;@doc` / `;;@example` headers existing examples use.

1. `portfolio-lp-highs.eta` — same problem as `portfolio-lp.eta`,
   solved through HiGHS. Bottom-of-file comment compares
   wall-clock and asserts identical objective.
2. `scheduling-mip.eta` — small job-shop / assignment MIP that the
   `std.clpr` simplex cannot handle (no integrality). Demonstrates
   `add-int-var!` and `'mip-rel-gap`.
3. `mean-variance-qp.eta` — Markowitz mean-variance with a small
   covariance matrix; demonstrates `set-hessian!` and reading
   `solution-objective`.

### 8.4 `docs/architecture.md`

Add one paragraph under the "Numerical stack" section (or create
one): the Eigen / libtorch / `std.clpr` triad is joined by HiGHS as
the heavyweight optimisation path; `std.clpr` remains the
logic-integrated solver.

### 8.5 `docs/build.md`

Add a bullet under "Optional native packages" listing
`eta-highs` with the `-DETA_HIGHS_FETCH_UPSTREAM=ON` flag and the
disk / build-time cost (~30 MB source, ~2 min build on
4 cores).

---

## 9) Build / packaging touchpoints

1. **Top-level `CMakeLists.txt`** — no change required. Native
   packages are built out-of-tree by their own CMakeLists, the same
   way `eta-lightgbm` is. CI calls `cmake --build` on each package
   that opts in.
2. **`scripts/build_packages.ps1` / `.sh`** — add `numerics/highs`
   to the package iteration list (same one-line edit that landed
   `ml/lightgbm`).
3. **`scripts/build-release.ps1` / `.sh` / `.cmd`** — include the
   staged sidecar binaries in the release bundle when
   `ETA_INCLUDE_HIGHS=1`. Default off in v0.1, on once binaries
   are signed.
4. **`scripts/check-windows-bundle.ps1` / `check-unix-bundle.sh`** —
   add the `eta_highs` artefact to the optional-packages whitelist
   so verification doesn't flag it as foreign content.
5. **`eta.lock`** — produced by the existing locking step in
   `scripts/build_packages.*`; SHA-256s in §5.2 are filled in by
   that flow once the binaries are built.
6. **Top-level `README.md`** — extend the "Native packages" bullet
   list to mention `eta-highs` once v0.1 ships.

---

## 10) Phased delivery roadmap

### M0 — Scaffolding (1 day)

1. Create `packages/numerics/native/highs/` directory, copy the
   `eta-lightgbm` skeleton, rename symbols.
2. Land `eta.toml` with placeholder SHAs, an empty
   `highs_extension.cpp` returning `ETA_NATIVE_OK`, and a
   `(module optim.highs) (begin)` stub.
3. Land the package CMakeLists with `find_package(HIGHS)` only
   (no fetch); confirm it builds against a system install.

Gate: `cmake --build` succeeds on a machine with HiGHS installed;
loading the sidecar from `etai` does not crash.

### M1 — Fetch + solver wiring (2–3 days)

1. Land `cmake/FetchHiGHS.cmake` and the
   `ETA_HIGHS_FETCH_UPSTREAM` switch.
2. Implement `highs_model.{h,cpp}` (RAII wrapper, staged LP).
3. Implement primitives: `hi/version`, `hi/model-new`,
   `hi/model-add-col!`, `hi/model-add-row!`,
   `hi/model-set-objective-sense!`, `hi/run!`, status mapping,
   `hi/solution-objective`, `hi/solution-primal`.
4. C++ unit tests §7.1 cases 1, 4, 5.

Gate: a hand-built 2-var LP solves with the right answer through
`hi/*` primitives from `etai`.

### M2 — Builder API + Eta tests (2 days)

1. Implement `make-model`, `add-var!`, `add-int-var!`,
   `add-binary-var!`, `add-constraint!` + sugar, `set-objective!`
   (sets sense + per-column coefficients), `solve`, the `solution`
   record, all `solution-*` accessors.
2. Land Eta smoke tests §7.2 cases 1–4.
3. Rewrite `cookbook/numerics/portfolio-lp.eta` ⇒ new file
   `portfolio-lp-highs.eta`; assert objective parity with the
   `std.clpr` version.

Gate: `etai cookbook/numerics/portfolio-lp-highs.eta` prints the
same objective as the existing `portfolio-lp.eta`.

### M3 — Options, MIP, QP (2 days)

1. Implement `hi/model-set-option!`, `hi/model-get-option` with
   typed dispatch.
2. Implement `hi/model-set-hessian!` and the QP path; option
   `'integrality` on `hi/model-add-col!`.
3. C++ unit tests §7.1 cases 2, 3, 6, 7. Eta smoke tests §7.2
   cases 5, 6.
4. Land `cookbook/numerics/scheduling-mip.eta` and
   `mean-variance-qp.eta`.

Gate: full cookbook trio passes under CI.

### M4 — Documentation, packaging, release (1–2 days)

1. Write `docs/stdlib/optim-highs.md`.
2. Update `docs/stdlib.md`, `docs/architecture.md`,
   `docs/build.md`, `docs/release-notes.md`.
3. Wire the package into `scripts/build_packages.*` and
   `scripts/build-release.*`.
4. Build artefacts on Windows / Linux x64 (and Linux arm64 if a
   runner is available); freeze SHA-256s in `eta.toml`.
5. Verify bundle checks pass.

Gate: a fresh checkout + `scripts/build_packages.ps1` produces a
loadable sidecar, the Eta smoke tests are green, and
`docs/stdlib.md` lists the module.

### M5 *(optional, post-v0.1)* — Quality of life

1. Warm-start API: `solve` accepts an optional previous `solution`
   to seed the basis (`Highs::setBasis`).
2. Async / interruptible solve via the existing `nng` actor harness
   (`Highs::setCallback` + a cancellation channel).
3. Symbolic `defmodel` macro layer: `(defmodel m (min …) (subject-to …))`.
4. Sensitivity analysis: `hi/ranging` exposing
   `Highs::getRanging`.

---

## 11) Open questions and risks

1. **HiGHS shared-library naming on Windows.** HiGHS produces
   `highs.dll` (no `lib` prefix) which the sidecar must locate at
   load time. The `StageHiGHSSidecar.cmake` helper copies it next
   to `eta_highs.dll` so the OS loader resolves it. Confirm Windows
   delay-load / SxS behaviour during M1.
2. **Boost dependency.** `eta-lightgbm` requires Boost 1.88; HiGHS
   does **not** require Boost. The package's CMakeLists should
   *not* `find_package(Boost)` unless the sidecar SDK header
   transitively needs it. Confirm by inspecting
   `eta/core/src/eta/native/sdk.h` headers during M0.
3. **C vs C++ API.** Plan binds against `Highs.h` (C++). If ABI
   stability across MSVC / Clang / libstdc++ versions causes pain,
   fall back to `interfaces/highs_c_api.h` (plain C, ABI-stable).
   Decision deferred to M1.
4. **`std.clpr` overlap messaging.** Risk that users get confused
   between `clp:r-maximize` and `optim.highs/solve`. Mitigation:
   one-paragraph callout at the top of `optim-highs.md` and a
   one-line cross-reference in `docs/stdlib/clpr.md` (if it exists).
5. **MIP determinism.** HiGHS MIP can be non-deterministic across
   thread counts. Default the package to `'parallel = 'off` and
   `'random_seed = 0` for reproducibility; document how to opt
   into parallel for production.
6. **Binary size.** A static-linked HiGHS adds ~6 MB; shared
   ~3 MB DLL. Acceptable, but ensure the release bundle
   verification script accommodates the new artefact size.
7. **Package group naming.** This plan creates
   `packages/numerics/`. Alternatives considered:
   `packages/optim/` (more specific) or putting it under
   `packages/stdlib/` (already exists). `numerics/` matches
   the `cookbook/numerics/` directory and leaves room for
   sibling packages (`optim.ipopt`, future statistics).
   Decision: go with `numerics/`; revisit if a second package
   in the group disagrees.
8. **License surface area.** HiGHS is MIT, but its optional
   plugin ecosystem (Cupdlp, etc.) may pull in different licenses.
   The fetch CMake disables every optional component (`PYTHON`,
   `FORTRAN`, …) so the resulting binary is MIT-only. Audit the
   transitive dependency graph during M4 before freezing the
   release bundle.

---

## 12) Acceptance checklist

- [ ] `packages/numerics/native/highs/` exists with the file layout
      from §2.
- [ ] `cmake --build` succeeds with both
      `ETA_HIGHS_FETCH_UPSTREAM=ON` and a system HiGHS install.
- [ ] `eta_register_highs_extension_v1` is exported from the
      sidecar and registers every primitive in §5.4.
- [ ] `optim.highs` exports every symbol in §6.1.
- [ ] All seven C++ unit tests in §7.1 pass.
- [ ] All six Eta smoke tests in §7.2 pass.
- [ ] `cookbook/numerics/portfolio-lp-highs.eta` produces objective
      ≈ `0.135`, matching the existing `portfolio-lp.eta` to
      `1e-9`.
- [ ] `cookbook/numerics/scheduling-mip.eta` returns `'optimal`
      with an integer-valued solution.
- [ ] `cookbook/numerics/mean-variance-qp.eta` returns the
      closed-form QP optimum to `1e-6`.
- [ ] `docs/stdlib/optim-highs.md` written; `docs/stdlib.md`
      lists the new module under "Optimization".
- [ ] `eta.toml` lockfile entries carry real SHA-256s for at least
      Windows x64 and Linux x64 artefacts.
- [ ] `scripts/build_packages.*` builds the package end-to-end with
      no manual steps.
- [ ] Bundle verification (`scripts/check-windows-bundle.ps1`,
      `scripts/check-unix-bundle.sh`) is green with the new
      artefact present.

