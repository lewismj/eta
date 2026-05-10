# Eta DuckDB

This package is an independent native sidecar package under
`packages/db/native/duckdb`.

It provides:

- package manifest metadata for a native sidecar (`eta.toml`)
- package-local native build and test wiring (`CMakeLists.txt`)
- sidecar extension entrypoint scaffold (`eta_register_duckdb_extension_v1`)
- Eta module scaffold in `src/db/duckdb.eta`
- package-local C++ and Eta smoke tests

The pinned upstream DuckDB tag for this package is `v1.5.1`.
The fetch helper is in `cmake/FetchDuckDB.cmake`.
Host sidecar staging helper is in `cmake/StageDuckDBSidecar.cmake`.

## Current scope

- D1 standalone package build/test wiring is implemented.
- Runtime primitive registration is intentionally empty in this scaffold.
- Host sidecar artifacts are staged under `native/<arch>/libs/...` and host
  `sha256` is updated in `eta.toml`.

## Eta API

Current exported module: `db.duckdb`.

- `duckdb-upstream-version`

## Build and test

```powershell
cmake -S packages/db/native/duckdb -B out/duckdb-msvc `
  -DETA_ETA_EXECUTABLE="C:/Users/lewis/develop/eta/out/msvc-release/eta/cli/eta.exe" `
  -DETA_ETAI_EXECUTABLE="C:/Users/lewis/develop/eta/out/msvc-release/eta/tools/interpreter/etai.exe" `
  -DETA_STDLIB_DIR="C:/Users/lewis/develop/eta/stdlib"
cmake --build out/duckdb-msvc --config Release
ctest --test-dir out/duckdb-msvc -C Release --output-on-failure
```

To stage the host-built sidecar into `native/<arch>/libs/` and update the host
`sha256` row in `eta.toml`:

```powershell
cmake `
  -DPACKAGE_ROOT="C:/Users/lewis/develop/eta/packages/db/native/duckdb" `
  -DSIDECAR_BINARY="C:/Users/lewis/develop/eta/out/duckdb-msvc/Release/eta_duckdb.dll" `
  -DHOST_TARGET_TRIPLE="x86_64-pc-windows-msvc" `
  -P "C:/Users/lewis/develop/eta/packages/db/native/duckdb/cmake/StageDuckDBSidecar.cmake"
```
