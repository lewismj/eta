# Release Notes

[<- Back to README](../README.md)

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
