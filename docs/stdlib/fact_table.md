# std.fact_table

Columnar fact tables with hash indexes, grouping, aggregation, and a CSV
bridge. Built on the `%fact-table-*` runtime primitives.

```scheme
(import std.fact_table)
```

## Construction

| Symbol | Description |
| --- | --- |
| `(make-fact-table columns)` | Create a table with the given column names. |
| `(fact-table? x)` | True when `x` is a fact table. |

## Mutation and indexing

| Symbol | Description |
| --- | --- |
| `(fact-table-insert! t row)` | Append a row (alist or list matching column order). |
| `(fact-table-build-index! t cols)` | Build a hash index over `cols` for fast queries. |

## Query

| Symbol | Description |
| --- | --- |
| `(fact-table-query t bindings)` | Return matching rows for the given column/value bindings. |
| `(fact-table-ref t row-index col)` | Cell value. |
| `(fact-table-row-count t)` | Number of rows. |
| `(fact-table-row t row-index)` | Row as alist. |
| `(fact-table-for-each t f)` | Apply `f` to each row. |
| `(fact-table-filter t pred)` | Return rows for which `pred` is true. |
| `(fact-table-fold t f init)` | Left fold over rows. |

## Aggregation

| Symbol | Description |
| --- | --- |
| `(fact-table-group-count t cols)` | Count rows per distinct combination of `cols`. |
| `(fact-table-group-sum t cols sum-col)` | Sum `sum-col` per group. |
| `(fact-table-group-by t cols)` | Group rows by `cols`. |
| `(fact-table-partition t pred)` | Two tables: rows passing and rows failing `pred`. |

## CSV bridge

| Symbol | Description |
| --- | --- |
| `(fact-table-load-csv path)` | Build a fact table from a CSV file (header required). |
| `(fact-table-save-csv t path)` | Write a fact table to a CSV file. |

