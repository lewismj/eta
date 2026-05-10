# DuckDB Package Plan

[Back to README](../../README.md) |
[Packages Guide](../guide/packages.md) |
[Native Sidecar Plan](../old/native_sidecar_plan.md)

Status: D1 standalone package build implemented (2026-05-10); D2-D5 proposed.

---

## 1) Objective

Deliver DuckDB support as a package-managed native sidecar with Eta wrapper
APIs in the same package, with these constraints:

1. no edits to core/runtime CMake wiring,
2. sidecar builds outside core system,
3. sidecar tests live under package-local `tests/`,
4. add a `define-syntax` query tool for higher-level fluent query composition.

---

## 2) Hard constraints

1. Do not edit:
   - `eta/CMakeLists.txt`,
   - `eta/core/CMakeLists.txt`,
   - core test targets under `eta/qa/*` for package-local validation.
2. Keep all DuckDB build logic in package-local files.
3. Keep all package tests under package-local `tests/` directories.
4. Load through normal package + lockfile sidecar metadata, not bundled stdlib
   fallback discovery.

---

## 3) Recommended package layout

Use one package that contains both sidecar/native sources and Eta module files.

```text
packages/
  db/
    native/
      duckdb/
        eta.toml
        README.md
        CMakeLists.txt              # standalone sidecar build
        cmake/
          FetchDuckDB.cmake         # optional, if vendoring/fetching
          StageDuckDBSidecar.cmake  # stage artifact + update host sha256
        src/
          db/duckdb.eta             # Eta module surface
          eta/duckdb/...            # native extension entry + primitives
        libs/
          amd64/eta_duckdb.dll
          amd64/libeta_duckdb.so
          amd64/libeta_duckdb.dylib
          arm64/libeta_duckdb.dylib
        tests/
          unit/...
          eta/...
```

Notes:

1. `db.duckdb` Eta APIs now live under `packages/db/native/duckdb/src/db`.
2. This layout keeps all build/test operations package-local.

---

## 4) Sidecar package (`eta-duckdb`)

## 4.1 Manifest shape

`packages/db/native/duckdb/eta.toml` follows existing sidecar metadata:

1. `[package]`,
2. `[compatibility]`,
3. `[native]` with:
   - `kind = "sidecar"`,
   - `abi = "eta-native-v1"`,
   - `id = "eta.duckdb.sidecar"`,
   - `entry = "eta_register_duckdb_extension_v1"` (or default entry name),
4. `[[native.targets]]` rows per supported host triple.

## 4.2 MVP primitive surface

Register a minimal, stable set first:

1. `%duckdb-open` `(path)` -> connection handle,
2. `%duckdb-close!` `(conn)` -> boolean,
3. `%duckdb-exec` `(conn sql)` -> command result/meta,
4. `%duckdb-query` `(conn sql params)` -> rows,
5. `%duckdb-last-error` `(conn-or-nil)` -> string.

Stage-2 surface:

1. prepared statements (`prepare/bind/step/finalize`),
2. appender/bulk insert APIs,
3. typed column extraction for large results.

---

## 5) Eta wrapper module (`db.duckdb`)

`db.duckdb` should expose function-first APIs regardless of DSL use:

1. `duckdb:open`, `duckdb:close!`,
2. `duckdb:exec`, `duckdb:query`,
3. query builder helpers:
   - `duckdb:q-new`,
   - `duckdb:q-select`,
   - `duckdb:q-from`,
   - `duckdb:q-where`,
   - `duckdb:q-join`,
   - `duckdb:q-order-by`,
   - `duckdb:q-limit`,
   - `duckdb:q-to-sql`,
   - `duckdb:q-params`,
   - `duckdb:q-run`.

The DSL macro layer should compile to these helpers only.

---

## 6) `define-syntax` query tool (fluent style)

Goal: add ergonomic syntax, but keep expansion deterministic and simple.

## 6.1 User-facing shape

```scheme
(duckdb:query conn
  (from "trades t")
  (select "t.id" "t.pnl" "b.book")
  (join "books b" "b.id = t.book_id")
  (where "b.desk = ?" desk)
  (where "t.trade_date >= ?" start-date)
  (order-by "t.pnl DESC")
  (limit 50))
```

## 6.2 Expansion strategy

Use a fluent macro that rewrites into pure function composition:

```scheme
(define-syntax duckdb:>
  (syntax-rules (from select join where group-by having order-by limit run)
    ((_ q) q)
    ((_ q (from table) rest ...)
     (duckdb:> (duckdb:q-from q table) rest ...))
    ((_ q (select col ...) rest ...)
     (duckdb:> (duckdb:q-select q col ...) rest ...))
    ((_ q (join table on-expr) rest ...)
     (duckdb:> (duckdb:q-join q table on-expr) rest ...))
    ((_ q (where pred arg ...) rest ...)
     (duckdb:> (duckdb:q-where q pred arg ...) rest ...))
    ((_ q (group-by expr ...) rest ...)
     (duckdb:> (duckdb:q-group-by q expr ...) rest ...))
    ((_ q (having pred arg ...) rest ...)
     (duckdb:> (duckdb:q-having q pred arg ...) rest ...))
    ((_ q (order-by expr) rest ...)
     (duckdb:> (duckdb:q-order-by q expr) rest ...))
    ((_ q (limit n) rest ...)
     (duckdb:> (duckdb:q-limit q n) rest ...))
    ((_ q (run conn))
     (duckdb:q-run conn q))))

(define-syntax duckdb:query
  (syntax-rules ()
    ((_ conn clause ...)
     (duckdb:> (duckdb:q-new) clause ... (run conn)))))
```

## 6.3 DSL scope rules

1. Keep clause order fixed for v1 (`from -> join -> where -> group -> having -> order -> limit`).
2. Allow repeated `where` and `join`.
3. Macro does not parse SQL expressions; strings remain user-authored SQL
   fragments with positional `?` parameters.
4. Parameter values are collected by helper functions, not macro-time logic.

This gives fluent ergonomics without requiring procedural macros.

---

## 7) Implementation roadmap

## D0 - Scaffold and metadata

1. Create package directories and `eta.toml` files.
2. Add sidecar README/build/test usage.
3. Add Eta module scaffold in package-local `src/db`.

Implemented in this repo snapshot:

1. `packages/db/native/duckdb` sidecar scaffold with pinned `v1.5.1` fetch helper.
2. importable `db.duckdb` module in `packages/db/native/duckdb/src/db/duckdb.eta`.
3. package-local C++ tests validating extension metadata and entrypoint behavior.

Gate:

1. `eta tree --manifest-path` parses cleanly for the package.

## D1 - Standalone sidecar build

1. Add package-local `CMakeLists.txt` and host staging helpers.
2. Build per host platform into `libs/<arch>/...`.
3. Compute and write target checksums for `[[native.targets]]`.

Implemented in this repo snapshot:

1. package-local CMake build and tests for `eta_duckdb`.
2. `cmake/StageDuckDBSidecar.cmake` to stage host artifact and update host
   `sha256` in `eta.toml`.
3. Eta smoke test fixture that validates metadata parse, lockfile sidecar
   metadata, and `db.duckdb` module import.

Gate:

1. sidecar binary builds with no core CMake edits.

## D2 - Sidecar ABI wiring

1. Implement sidecar entrypoint + primitive registration.
2. Return stable extension metadata (`abi/id/version`).
3. Implement MVP primitive set (open/close/exec/query/error).

Gate:

1. package-level smoke test can load sidecar and run `select 1`.

## D3 - Eta wrappers

1. Implement `db.duckdb` function-first API.
2. Normalize result row shape and error behavior.
3. Add wrapper tests for open/close/query/parameterized query.

Gate:

1. wrapper tests green without DSL.

## D4 - Fluent DSL macro tool

1. Add `define-syntax` macros in `db/duckdb/query.eta`.
2. Add expansion-focused tests (`dsl.test.eta`) for each clause form.
3. Verify generated SQL + params match function-only builder output.

Gate:

1. DSL and function paths produce identical results for same query.

## D5 - Packaging and docs hardening

1. finalize README docs and examples,
2. add migration guidance (raw SQL -> builder -> DSL),
3. pin version bounds and compatibility notes.

Gate:

1. package docs + tests fully runnable with package-local build/test commands.

---

## 8) Testing plan (package-local only)

`packages/db/native/duckdb/tests/`:

1. C++ unit tests for extension metadata + entrypoint scaffold behavior.
2. Eta smoke test that materializes a fixture package with host sidecar artifact.
3. D2+ will add open/close/query/error behavior tests.

Suggested commands:

```text
cmake -S packages/db/native/duckdb -B out/duckdb-msvc \
  -DETA_ETA_EXECUTABLE=<path-to-eta> \
  -DETA_ETAI_EXECUTABLE=<path-to-etai> \
  -DETA_STDLIB_DIR=<path-to-stdlib>
ctest --test-dir out/duckdb-msvc -C Release --output-on-failure
```

No requirement to touch `eta/qa/*`.

---

## 9) Effort estimate

1. D0-D1 scaffold + standalone build: 0.5-1.0 day.
2. D2 sidecar MVP primitives: 1.0-2.0 days.
3. D3 wrapper API + tests: 0.5-1.0 day.
4. D4 DSL macro tool + tests: 0.5-1.0 day.
5. D5 hardening/docs: 0.5 day.

Total:

1. MVP package set: 3-5 days.
2. with prepared statements + bulk APIs + extra platform polish: 5-9 days.

---

## 10) Risks and mitigations

1. ABI drift vs Eta runtime headers:
   - pin Eta compatibility range in package manifest.
2. SQL DSL overreach:
   - keep macro DSL thin; push logic to functions.
3. Platform artifact churn:
   - maintain explicit package-local build scripts and checksums.
4. Query safety:
   - enforce parameterized `where/having` helpers by default.

---

## 11) Open decisions

1. Whether v1 includes prepared statements or only `query(conn, sql, params)`.
2. Initial result representation:
   - list-of-alists,
   - fact-table conversion helper,
   - both.
