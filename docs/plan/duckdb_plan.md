# DuckDB Package Plan

[Back to README](../../README.md) |
[Packages Guide](../guide/packages.md) |
[Native Sidecar Plan](native_sidecar_plan.md)

Status: proposed (2026-05-10).

---

## 1) Objective

Deliver DuckDB support as a package-managed native sidecar plus Eta wrapper
APIs, with these constraints:

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

Use two packages:

1. `eta-duckdb-sidecar`: native sidecar artifact package.
2. `eta-duckdb`: Eta wrapper + DSL package depending on sidecar package.

```text
packages/
  databases/
    native/
      duckdb/
        eta.toml
        README.md
        CMakeLists.txt              # standalone sidecar build
        cmake/
          FetchDuckDB.cmake         # optional, if vendoring/fetching
        src/
          duckdb_sidecar.cpp
          duckdb_bindings.h
        include/
          eta/duckdb/...
        native/
          windows-x64/eta_duckdb_sidecar.dll
          linux-x64/libeta_duckdb_sidecar.so
          macos-x64/libeta_duckdb_sidecar.dylib
          macos-arm64/libeta_duckdb_sidecar.dylib
        tests/
          smoke.test.eta
          query.test.eta
          errors.test.eta
        scripts/
          build.ps1
          build.sh
          test.ps1
          test.sh

    duckdb/
      eta.toml
      README.md
      src/
        std/duckdb.eta
        std/duckdb/query.eta
      tests/
        dsl.test.eta
        integration.test.eta
```

Notes:

1. If you want one package only, `std/duckdb*.eta` can live in the sidecar
   package directly, but two packages keep native/runtime and API/DSL ownership
   cleaner.
2. This layout keeps all build/test operations package-local.

---

## 4) Sidecar package (`eta-duckdb-sidecar`)

## 4.1 Manifest shape

`packages/databases/native/duckdb/eta.toml` follows existing sidecar metadata:

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

## 5) Eta wrapper package (`eta-duckdb`)

`std.duckdb` should expose function-first APIs regardless of DSL use:

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
3. Add wrapper README with import/dependency examples.

Gate:

1. `eta pkg` metadata parses cleanly for both packages.

## D1 - Standalone sidecar build

1. Add package-local `CMakeLists.txt` and scripts (`build.sh`, `build.ps1`).
2. Build per host platform into `native/<platform>/...`.
3. Compute and write target checksums for `[[native.targets]]`.

Gate:

1. sidecar binary builds with no core CMake edits.

## D2 - Sidecar ABI wiring

1. Implement sidecar entrypoint + primitive registration.
2. Return stable extension metadata (`abi/id/version`).
3. Implement MVP primitive set (open/close/exec/query/error).

Gate:

1. package-level smoke test can load sidecar and run `select 1`.

## D3 - Eta wrappers

1. Implement `std.duckdb` function-first API.
2. Normalize result row shape and error behavior.
3. Add wrapper tests for open/close/query/parameterized query.

Gate:

1. wrapper tests green without DSL.

## D4 - Fluent DSL macro tool

1. Add `define-syntax` macros in `std/duckdb/query.eta`.
2. Add expansion-focused tests (`dsl.test.eta`) for each clause form.
3. Verify generated SQL + params match function-only builder output.

Gate:

1. DSL and function paths produce identical results for same query.

## D5 - Packaging and docs hardening

1. finalize README docs and examples,
2. add migration guidance (raw SQL -> builder -> DSL),
3. pin version bounds and compatibility notes.

Gate:

1. package docs + tests fully runnable with package-local scripts.

---

## 8) Testing plan (package-local only)

`packages/databases/native/duckdb/tests/`:

1. sidecar load smoke test,
2. open/close and error-path tests,
3. basic query/parameterization tests.

`packages/databases/duckdb/tests/`:

1. wrapper API tests,
2. DSL expansion/equivalence tests,
3. integration tests with in-memory DB fixtures.

Suggested commands:

```text
packages/databases/native/duckdb/scripts/test.sh
packages/databases/duckdb/scripts/test.sh
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
   - pin Eta compatibility range in both manifests.
2. SQL DSL overreach:
   - keep macro DSL thin; push logic to functions.
3. Platform artifact churn:
   - maintain explicit package-local build scripts and checksums.
4. Query safety:
   - enforce parameterized `where/having` helpers by default.

---

## 11) Open decisions

1. Single package vs split packages (recommended split).
2. Whether v1 includes prepared statements or only `query(conn, sql, params)`.
3. Initial result representation:
   - list-of-alists,
   - fact-table conversion helper,
   - both.

