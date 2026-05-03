# std.csv

CSV reader and writer wrappers over the `%csv-*` runtime primitives.

```scheme
(import std.csv)
```

## Reader

| Symbol | Description |
| --- | --- |
| `(csv:open-reader path . opts)` | Open a CSV file for reading. Options: `'delimiter`, `'quote`, `'header?`. |
| `(csv:reader-from-string str . opts)` | Reader backed by an in-memory string. |
| `(csv:reader? x)` | True when `x` is a CSV reader. |
| `(csv:columns r)` | Header column names, or `#f` if no header. |
| `(csv:read-row r)` | Read next row as a list of strings, or EOF. |
| `(csv:read-record r)` | Read next row as an alist keyed by column. |
| `(csv:read-typed-row r types)` | Read next row coerced according to `types` (`'string`, `'int`, `'real`, `'bool`). |

## Writer

| Symbol | Description |
| --- | --- |
| `(csv:open-writer path . opts)` | Open a CSV file for writing. |
| `(csv:writer? x)` | True when `x` is a CSV writer. |
| `(csv:write-row w row)` | Write a list of values. |
| `(csv:write-record w alist)` | Write an alist using header order. |
| `(csv:flush w)` | Flush buffered output. |

## Lifecycle

| Symbol | Description |
| --- | --- |
| `(csv:close x)` | Close a reader or writer. |

## Iteration helpers

| Symbol | Description |
| --- | --- |
| `(csv:fold r f init)` | Left fold over rows. |
| `(csv:for-each r f)` | Apply `f` to each row. |
| `(csv:collect r)` | Read all rows into a list. |
| `(csv:load-file path . opts)` | Convenience: open, read all rows, close. |
| `(csv:save-file path rows . opts)` | Convenience: open, write all rows, close. |

