# Eta LightGBM 

This package is an independent native sidecar package under
`packages/ml/native/lightgbm`.

It provides:

- a package manifest with native sidecar metadata (`eta.toml`)
- a package-local native build (`CMakeLists.txt`)
- a sidecar entrypoint (`eta_register_lgbm_extension_v1`)
- LightGBM C API hooks behind registered `lgbm/*` primitives
- package-local C++ unit tests and Eta smoke tests

Host sidecar artifacts are staged under `native/<arch>/libs/`:

- `native/amd64/libs/eta_lightgbm.dll` (Windows x86_64)
- `native/amd64/libs/libeta_lightgbm.so` (Linux x86_64)
- `native/amd64/libs/libeta_lightgbm.dylib` (macOS x86_64)
- `native/arm64/libs/libeta_lightgbm.dylib` (macOS arm64)

The CMake fetch helper is pinned to LightGBM tag `v4.6.0`:
`cmake/FetchLightGBM.cmake`.

## Eta API

The package exports the `ml.lightgbm` module with these bindings:

- `dataset-from-list features labels`
- `dataset-from-facttable fact-table feature-cols label-col`
- `booster-create dataset`
- `train! booster dataset`
- `predict booster feature-row`
- `save booster path`
- `load path`
- `eval booster dataset`
- `num-trees booster`
- `feature-importance booster`

## Runtime behavior

- `dataset-from-list` and `dataset-from-facttable` build real LightGBM datasets
  from dense numeric values.
- `booster-create`, `train!`, `predict`, `eval`, `num-trees`, and
  `feature-importance` call LightGBM through the C API.
- `eval` returns mean-squared error over the supplied dataset.
- `save`/`load` use LightGBM model-text serialization inside the sidecar and
  are keyed by the provided path token for the current Eta process.

Current defaults and scope:

- Booster creation currently uses fixed defaults:
  `objective=regression metric=l2 min_data_in_leaf=1 num_leaves=8 verbosity=-1`
- Hyperparameter configuration and true file-backed model persistence are not
  exposed yet.

## Usage example

```scheme
(module ml.lightgbm.example
  (import ml.lightgbm)
  (begin
    (define dataset
      (dataset-from-list
        '((1.0 2.0)
          (2.0 1.0))
        '(0.0 1.0)))

    (define booster (booster-create dataset))
    (train! booster dataset)

    (define score (predict booster '(1.0 2.0)))
    (define quality (eval booster dataset))
    (define importance (feature-importance booster))

    (save booster "model-key:demo")
    (define reloaded (load "model-key:demo"))

    (display (list
      'num-trees (num-trees booster)
      'score score
      'quality quality
      'importance importance
      'reloaded-score (predict reloaded '(1.0 2.0))))
    (newline)))
```

Fact-table backed dataset conversion:

```scheme
(module ml.lightgbm.facttable-example
  (import std.fact_table)
  (import ml.lightgbm)
  (begin
    (define ft (make-fact-table 'f1 'f2 'label))
    (fact-table-insert! ft 1.0 2.0 0.0)
    (fact-table-insert! ft 2.0 1.0 1.0)

    (define dataset (dataset-from-facttable ft '(0 1) 2))
    (define booster (booster-create dataset))
    (train! booster dataset)

    (display (predict booster '(1.0 2.0)))
    (newline)))
```

## Build and test

From repo root:

```powershell
cmake -S packages/ml/native/lightgbm -B out/lightgbm-msvc `
  -DETA_LIGHTGBM_FETCH_UPSTREAM=ON `
  -DETA_ETAI_EXECUTABLE="C:/Users/lewis/develop/eta/out/msvc-release/eta/tools/interpreter/etai.exe" `
  -DETA_STDLIB_DIR="C:/Users/lewis/develop/eta/stdlib"
cmake --build out/lightgbm-msvc --config Release
ctest --test-dir out/lightgbm-msvc --output-on-failure
```

`ETA_ETAI_EXECUTABLE` should point to a built `etai` binary.
Depending on your local setup, you may also need to provide Boost paths
(`Boost_DIR` / `Boost_INCLUDE_DIR`) at configure time.

To stage the host-built sidecar into `native/<arch>/libs/` and update the host
`sha256` entry in `eta.toml`:

```powershell
cmake `
  -DPACKAGE_ROOT="C:/Users/lewis/develop/eta/packages/ml/native/lightgbm" `
  -DSIDECAR_BINARY="C:/Users/lewis/develop/eta/out/lightgbm-msvc/Release/eta_lightgbm.dll" `
  -DHOST_TARGET_TRIPLE="x86_64-pc-windows-msvc" `
  -P "C:/Users/lewis/develop/eta/packages/ml/native/lightgbm/cmake/StageLightGBMSidecar.cmake"
```
