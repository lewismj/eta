# Release Notes

[<- Back to README](../README.md)

---

## 2026-04-26

### Jupyter Kernel (`eta_jupyter`)

A native xeus-based Jupyter kernel now ships alongside the existing
executables. The kernel embeds the same `Driver` used by `etai` and
`eta_repl`, so notebook cells share the language semantics — modules,
macros, AAD, libtorch, CLP, causal, actors — with no FFI hop.

**Highlights:**

- New executable `eta_jupyter` built from `eta/jupyter/` against
  `xeus` + `xeus-zmq` (vendored via `cmake/FetchXeus.cmake`); listed in
  the standard `eta_all` target.
- Runtime kernelspec installation:

  ```powershell
  eta_jupyter --install --user
  eta_jupyter --install --sys-prefix
  eta_jupyter --install --prefix <path>
  ```

- Rich display MIME bundles (with `text/plain` fallback) for tensors,
  fact tables, and heap snapshots:
  `application/vnd.eta.tensor+json`,
  `application/vnd.eta.facttable+json`,
  `application/vnd.eta.heap+json`.
- `(import std.jupyter)` exposes `jupyter:table`, `jupyter:plot`, and
  `jupyter:dag` rich-display helpers used by the showcase notebooks.
- Three showcase notebooks under `examples/notebooks/`:
  `LanguageBasics.ipynb`, `AAD.ipynb`, `Portfolio.ipynb`.
- Docs: [docs/jupyter.md](jupyter.md).

### Documentation

- Top-level `README.md` re-pitched: notebook links surfaced under
  Featured Examples, install / REPL / VS Code / build sections folded
  into `<details>` blocks, duplicated directory trees moved to
  `docs/quickstart.md` and `docs/build.md`, doc table trimmed to top
  hits with the full index living in `docs/index.md`.
- `docs/aad.md` and `docs/examples.md` link directly to their
  notebook equivalents.

---

## 2026-04-20

### CLP(R) Convex QP Stage 6 Rollout Gate

- Added a baseline benchmark executable: `eta_qp_bench`.
- Added benchmark runner scripts:
  - `scripts/qp-benchmark.ps1`
  - `scripts/qp-benchmark.sh`
- Benchmark output now compares LP-proxy and QP solve paths for representative
  sizes (`8,16,24,32`) with:
  - objective quality (`true_qp - true_lp`)
  - runtime (`lp_ms`, `qp_ms`, `qp/lp`)
  - numerical stability (`qp_obj_drift`, `qp_weight_drift`)
- Added optional rollout gating with `--gate` to enforce parity/stability
  thresholds before enabling or promoting changes.
- Documented convex QP optimization and backend flag behavior in:
  - `docs/clp.md`
  - `docs/portfolio.md`

### Commands

Windows PowerShell:

```powershell
.\scripts\qp-benchmark.ps1 -BuildDir C:\Users\lewis\develop\eta\out\msvc-release -Gate
```

Linux/macOS:

```bash
BUILD_DIR=./out/wsl-clang-release GATE=1 ./scripts/qp-benchmark.sh
```

### Backend Default

- `ETA_CLP_QP_BACKEND` is now compiled in unconditionally in
  `eta/core/CMakeLists.txt`.
