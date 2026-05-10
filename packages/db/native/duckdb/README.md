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
- D2 primitive registration and package-local primitive behavior are implemented.
- D3 Eta wrapper API and wrapper tests are implemented.
- D4 fluent query DSL module and DSL equivalence tests are implemented.
- D5 docs/examples hardening is implemented.
- Host sidecar artifacts are staged under `libs/<arch>/...` and host
  `sha256` is updated in `eta.toml`.

## Eta API

Current exported modules: `db.duckdb` and `db.duckdb.query`.

`db.duckdb` exports:

- `duckdb-upstream-version`
- `duckdb:open`
- `duckdb:close!`
- `duckdb:exec`
- `duckdb:query`
- `duckdb:last-error`
- `duckdb:q-new`
- `duckdb:q-select`
- `duckdb:q-from`
- `duckdb:q-join`
- `duckdb:q-where`
- `duckdb:q-group-by`
- `duckdb:q-having`
- `duckdb:q-order-by`
- `duckdb:q-limit`
- `duckdb:q-to-sql`
- `duckdb:q-params`
- `duckdb:q-run`

`duckdb:query` returns normalized list-of-alists row output and wrapper
errors with stable `duckdb:<operation>:` prefixes.

`db.duckdb.query` exports:

- `duckdb:query`
- `duckdb:build`
- `from`
- `select`
- `join`
- `where`
- `group-by`
- `having`
- `order-by`
- `limit`

Example fluent usage:

```scheme
(import db.duckdb)
(import db.duckdb.query)

(define conn (duckdb:open ":memory:"))
(duckdb:query conn
  (from "trades t")
  (select "t.id" "t.pnl")
  (where "t.pnl > ?" 0)
  (order-by "t.pnl DESC")
  (limit 10))
```

## Query styles

Use whichever style you prefer. All three paths are supported.

Raw SQL:

```scheme
(import db.duckdb)

(define conn (duckdb:open ":memory:"))
(duckdb:query conn
  "SELECT t.id, t.pnl FROM trades t WHERE t.pnl > ? ORDER BY t.pnl DESC LIMIT 10"
  '(0))
```

Function builder:

```scheme
(import db.duckdb)

(define conn (duckdb:open ":memory:"))
(define q
  (duckdb:q-limit
    (duckdb:q-order-by
      (duckdb:q-where
        (duckdb:q-from
          (duckdb:q-select (duckdb:q-new) "t.id" "t.pnl")
          "trades t")
        "t.pnl > ?"
        0)
      "t.pnl DESC")
    10))
(duckdb:q-run conn q)
```

Fluent DSL:

```scheme
(import db.duckdb)
(import db.duckdb.query)

(define conn (duckdb:open ":memory:"))
(duckdb:query conn
  (from "trades t")
  (select "t.id" "t.pnl")
  (where "t.pnl > ?" 0)
  (order-by "t.pnl DESC")
  (limit 10))
```

## Build and test

```powershell
cmake -S packages/db/native/duckdb -B out/duckdb-msvc `
  -DETA_ETA_EXECUTABLE="C:/Users/lewis/develop/eta/out/msvc-release/eta/cli/eta.exe" `
  -DETA_ETAI_EXECUTABLE="C:/Users/lewis/develop/eta/out/msvc-release/eta/tools/interpreter/etai.exe" `
  -DETA_STDLIB_DIR="C:/Users/lewis/develop/eta/stdlib"
cmake --build out/duckdb-msvc --config Release
ctest --test-dir out/duckdb-msvc -C Release --output-on-failure
```

Top-level `eta_all` integration:

- `cmake --build <repo-build-dir> --target eta_all` builds `eta_duckdb`.
- It also stages the host sidecar into `libs/<arch>/...` and refreshes the host
  `sha256` in `eta.toml`.

To stage the host-built sidecar into `libs/<arch>/` and update the host
`sha256` row in `eta.toml`:

```powershell
cmake `
  -DPACKAGE_ROOT="C:/Users/lewis/develop/eta/packages/db/native/duckdb" `
  -DSIDECAR_BINARY="C:/Users/lewis/develop/eta/out/duckdb-msvc/Release/eta_duckdb.dll" `
  -DHOST_TARGET_TRIPLE="x86_64-pc-windows-msvc" `
  -P "C:/Users/lewis/develop/eta/packages/db/native/duckdb/cmake/StageDuckDBSidecar.cmake"
```
