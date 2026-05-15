# db.duckdb

DuckDB bindings for Eta - embedded OLAP SQL via a native sidecar.
Pinned to DuckDB `v1.5.1`.

```scheme
(import db.duckdb)
(import db.duckdb.query)   ; optional fluent DSL
```

## Opening and closing

| Symbol | Description |
| --- | --- |
| `(duckdb:open path)` | Open a database at `path`. Use `":memory:"` for an in-memory database. Returns a connection handle. |
| `(duckdb:close! conn)` | Close the connection and release all resources. |
| `(duckdb:last-error conn-or-nil)` | Return the last sidecar error text for `conn-or-nil`, or `#f` when no error is recorded. |

## Executing SQL

| Symbol | Description |
| --- | --- |
| `(duckdb:exec conn sql)` | Execute a DDL or DML statement. Returns `#t` on success. |
| `(duckdb:query conn sql [params])` | Execute SQL and return rows as a list of alists. `params` is an optional list of `?` positional parameters. |

Rows are normalized as alists of `(column-name . value)` pairs, where each
`column-name` is a symbol. Parameter values support nil, booleans, numeric
values, strings, and symbols.

Errors carry stable `duckdb:<operation>:` prefixes.

## Fluent query DSL (`db.duckdb.query`)

| Symbol | Description |
| --- | --- |
| `(from table)` | FROM clause. |
| `(select col ...)` | SELECT list. |
| `(join table on)` | JOIN clause. |
| `(where condition [param ...])` | WHERE clause with optional positional parameters. |
| `(group-by col ...)` | GROUP BY. |
| `(having condition)` | HAVING. |
| `(order-by col ...)` | ORDER BY. |
| `(limit n)` | LIMIT. |
| `(duckdb:build ...)` | Combine clauses into a query record. |
| `(duckdb:query conn ...)` | Execute - accepts a raw SQL string, a query record, or inline DSL clauses. |

## Query styles

All three styles produce identical results. Choose whichever fits your code.

### Raw SQL

```scheme
(import db.duckdb)

(define conn (duckdb:open ":memory:"))
(duckdb:exec conn "CREATE TABLE trades (id INTEGER, pnl DOUBLE)")
(duckdb:exec conn "INSERT INTO trades VALUES (1, 42.5), (2, -3.1), (3, 17.8)")

(duckdb:query conn
  "SELECT id, pnl FROM trades WHERE pnl > ? ORDER BY pnl DESC LIMIT 10"
  '(0))
;; => (((id . 1) (pnl . 42.5)) ((id . 3) (pnl . 17.8)))
```

### Function builder

```scheme
(import db.duckdb)

(define conn (duckdb:open ":memory:"))
(define q
  (duckdb:q-limit
    (duckdb:q-order-by
      (duckdb:q-where
        (duckdb:q-from
          (duckdb:q-select (duckdb:q-new) "id" "pnl")
          "trades")
        "pnl > ?" 0)
      "pnl DESC")
    10))
(duckdb:q-run conn q)
```

### Fluent DSL

```scheme
(import db.duckdb)
(import db.duckdb.query)

(define conn (duckdb:open ":memory:"))
(duckdb:query conn
  (from "trades")
  (select "id" "pnl")
  (where "pnl > ?" 0)
  (order-by "pnl DESC")
  (limit 10))
```

## Modules exported

| Module | Description |
| --- | --- |
| `db.duckdb` | Core connection, exec, query, and function-builder API. |
| `db.duckdb.query` | Fluent DSL macros (`from`, `select`, `join`, `where`, `group-by`, `having`, `order-by`, `limit`). |

## Native sidecar

`db.duckdb` is loaded automatically when `(import db.duckdb)` is evaluated,
provided `ETA_MODULE_PATH` includes the package directory. No separate
installation step is required.

| Platform | Artifact |
| --- | --- |
| Windows x64 | `libs/amd64/eta_duckdb.dll` |
| Linux x64 | `libs/amd64/libeta_duckdb.so` |
| macOS arm64 | `libs/arm64/libeta_duckdb.dylib` |
