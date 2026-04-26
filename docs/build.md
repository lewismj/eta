# Eta — Building from Source

[← Back to README](../README.md) · [Quick Start](quickstart.md) ·
[Compiler (`etac`)](compiler.md) · [Architecture](architecture.md)

---

## Prerequisites

| Tool            | Version                                    |
|-----------------|--------------------------------------------|
| CMake           | ≥ 3.28                                     |
| C++ compiler    | C++23 (Clang 17+, GCC 13+, or MSVC 17.8+) |
| Boost           | ≥ 1.88 (`unit_test_framework`, `concurrent_flat_map`) |
| Node.js / npm   | ≥ 20.18.1 *(for VS Code extension)*       |
| vsce (optional) | `npm install -g @vscode/vsce`              |

---

## One-Script Build

Both scripts configure, build, install, package the VS Code extension,
and create a distributable archive. They take an optional install
directory argument.

### Linux / macOS / WSL

```bash
chmod +x scripts/build-release.sh
./scripts/build-release.sh ./dist/eta-v0.2.0-linux-x86_64
```

Produces `dist/eta-v0.2.0-linux-x86_64/` and `dist/eta-v0.2.0-linux-x86_64.tar.gz`.

Additional flags:

```bash
# Explicit version tag
./scripts/build-release.sh -v v0.2.0

# Enable libtorch bindings (auto-downloads libtorch)
./scripts/build-release.sh -t
./scripts/build-release.sh -t --torch-backend cu124   # CUDA 12.4
```

### Windows (PowerShell)

```powershell
.\scripts\build-release.ps1 .\dist\eta-v0.2.0-win-x64
```

Produces `dist\eta-v0.2.0-win-x64\` and `dist\eta-v0.2.0-win-x64.zip`.

Additional flags:

```powershell
# Explicit version + vcpkg for Boost
.\scripts\build-release.ps1 -Version v0.2.0 -VcpkgRoot C:\src\vcpkg

# Enable libtorch bindings
.\scripts\build-release.ps1 -EnableTorch
.\scripts\build-release.ps1 -EnableTorch -TorchBackend cu124
```

---

## Manual Build Steps

If you prefer not to use the scripts:

### Linux / WSL

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cmake --install build --prefix dist/eta-v0.2.0-linux-x86_64
```

### Windows (MSVC)

```powershell
cmake -B build
cmake --build build --config Release
cmake --install build --prefix dist\eta-v0.2.0-win-x64 --config Release
```

### With CUDA

By default the build downloads the CPU-only libtorch archive automatically.
To select a CUDA variant:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DETA_TORCH_BACKEND=cu124
cmake --build build -j$(nproc)
```

The `FetchLibtorch.cmake` module downloads the correct libtorch archive for
your platform and caches it in the build directory.  Supported backends:
`cpu` (default), `cu118`, `cu121`, `cu124`.

---

## Bundle Layout

After build or extraction:

```
eta-v0.2.0-<platform>/
  bin/
    etac(.exe)              # Ahead-of-time bytecode compiler
    etai(.exe)              # File interpreter (also runs .etac files)
    eta_repl(.exe)          # Interactive REPL
    eta_lsp(.exe)           # Language Server (JSON-RPC over stdio)
    eta_dap(.exe)           # Debug Adapter (DAP over stdio, used by VS Code)
    eta_jupyter(.exe)       # Jupyter kernel executable (`--install` writes kernelspec)
    resources/
      logo-32x32.png        # Kernel logo copied by `eta_jupyter --install`
      logo-64x64.png
  stdlib/
    prelude.eta             # Auto-loaded standard library
    std/
      core.eta  math.eta  io.eta  collections.eta  test.eta
      logic.eta  clp.eta  clpb.eta  clpr.eta  causal.eta
      db.eta  fact_table.eta  freeze.eta  net.eta  stats.eta
      supervisor.eta  torch.eta
  examples/
    hello.eta  basics.eta  functions.eta  higher-order.eta  ...
  editors/
    eta-lang-<version>.vsix # VS Code extension
  install.sh / install.cmd  # Post-extract installer
```

The binaries automatically locate `stdlib/` relative to themselves
(`<exe>/../stdlib/`), so no environment variables are needed when
using the installed layout.

To register the bundled Jupyter kernel in your current user environment:

```bash
eta_jupyter --install --user
```

---

## Development: Manual stdlib Path

When running from a build directory (not the install tree):

```bash
# Via CLI flag
./build/eta/interpreter/etai --path ./stdlib examples/hello.eta

# Or via environment variable
export ETA_MODULE_PATH=./stdlib
./build/eta/interpreter/etai examples/hello.eta
```

---

## Running Tests

### C++ Unit Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

### Torch Test Suite


Ten standalone test files in `examples/torch_tests/` exercise every
`std.torch` primitive end-to-end using the `std.test` framework:

```bash
# Run all torch tests (Linux / macOS)
for f in examples/torch_tests/*.eta; do echo "── $f"; etai "$f"; done

# Run all torch tests (Windows PowerShell)
Get-ChildItem examples\torch_tests\*.eta | ForEach-Object { Write-Host "── $_"; etai $_ }

# Run a single test file
etai examples/torch_tests/tensor_creation.eta
```

| File | Coverage |
|------|----------|
| `tensor_creation.eta` | `tensor`, `ones`, `zeros`, `randn`, `arange`, `linspace`, `from-list`, `tensor?` |
| `arithmetic.eta` | `t+`, `t-`, `t*`, `t/`, `matmul`, `dot`, `neg`, `tabs`, `texp`, `tlog`, `tsqrt`, `relu`, `sigmoid`, `ttanh`, `softmax` |
| `shape_ops.eta` | `shape`, `reshape`, `transpose`, `squeeze`, `unsqueeze`, `cat`, `numel` |
| `reductions.eta` | `tsum`, `mean`, `tmax`, `tmin`, `argmax`, `argmin` |
| `autograd.eta` | `requires-grad!`, `requires-grad?`, `backward`, `grad`, `zero-grad!`, `detach` |
| `nn_layers.eta` | `linear`, `sequential`, `relu-layer`, `sigmoid-layer`, `dropout`, `forward`, `parameters`, `module?`, `train!`, `eval!` |
| `loss_functions.eta` | `mse-loss`, `l1-loss` |
| `optimizers.eta` | `sgd`, `adam`, `step!`, `optim-zero-grad!`, `optimizer?` |
| `device_info.eta` | `gpu-available?`, `gpu-count`, `device`, `to-device`, `to-cpu` |
| `training.eta` | `train-step!`, SGD/Adam convergence, sequential network training |

#### C++ Unit Tests (Torch)

The C++ tests in `eta/test/src/torch_tests.cpp` mirror the Eta tests at
the primitive level, additionally verifying heap lifecycle, GC behaviour,
error paths, and builtin registration:

```bash
cmake -B build
cmake --build build
ctest --test-dir build -R torch
```

The `example_runner_tests` suite also automatically discovers and runs
all `.eta` files (including `torch_tests/`) when built with torch support.

---

## QP Benchmark and Rollout Gate

Stage 6 QP rollout benchmarking uses `eta_qp_bench` plus wrapper scripts.

### Windows (PowerShell)

```powershell
cmake --build out\msvc-release --target eta_qp_bench
.\scripts\qp-benchmark.ps1 -BuildDir C:\Users\lewis\develop\eta\out\msvc-release
.\scripts\qp-benchmark.ps1 -BuildDir C:\Users\lewis\develop\eta\out\msvc-release -Gate
```

### Linux / macOS

```bash
cmake --build out/wsl-clang-release --target eta_qp_bench -j"$(nproc)"
BUILD_DIR=./out/wsl-clang-release ./scripts/qp-benchmark.sh
BUILD_DIR=./out/wsl-clang-release GATE=1 ./scripts/qp-benchmark.sh
```

`-Gate` / `GATE=1` enables rollout checks for quality parity and stability.

---

## GitHub Actions CI

The repository includes `.github/workflows/release.yml` which:

1. Builds release bundles for **Linux x86_64** and **Windows x64**
2. Uploads them as workflow artifacts (downloadable from the Actions tab)
3. On tag push (`v*`), creates a **GitHub Release** with both archives

To trigger a release:

```bash
git tag v0.2.0
git push origin v0.2.0
```

To test the workflow without tagging, use **Actions → Release Bundle →
Run workflow** (manual dispatch).

