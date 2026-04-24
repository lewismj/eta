# CSV

[Back to README](../README.md) · [Modules and Stdlib](modules.md) · [Fact Tables](fact-table.md)

---

## Overview

Eta ships a native CSV subsystem backed by `vincentlaucsb/csv-parser`.
The public API is in `std.csv`, with optional bridge helpers in `std.fact_table`.

Key properties:

- Correct RFC4180 quoting and escaped quotes.
- CRLF/LF and UTF-8 BOM handling.
- Streaming reads (`csv:fold`, `csv:for-each`) for constant-memory ingestion.
- Native writer with configurable delimiter, quote character, and quote policy.
- Typed row reads with null-token handling.

---

## Quick Start

```scheme
(import std.csv)

;; Reader
(define r (csv:open-reader "trades.csv"))
(define cols (csv:columns r))
(define rec  (csv:read-record r))
(csv:close r)

;; Writer
(define w (csv:open-writer "out.csv" :column-names '(symbol qty price)))
(csv:write-record w '((symbol . "AAPL") (qty . 100) (price . 150.25))
                  '(symbol qty price))
(csv:close w)
```

---

## std.csv API

Reader:

- `csv:open-reader`
- `csv:reader-from-string`
- `csv:columns`
- `csv:read-row`
- `csv:read-record`
- `csv:read-typed-row`
- `csv:reader?`
- `csv:close`

Streaming helpers:

- `csv:fold`
- `csv:for-each`
- `csv:collect`
- `csv:load-file`

Writer:

- `csv:open-writer`
- `csv:write-row`
- `csv:write-record`
- `csv:flush`
- `csv:writer?`
- `csv:save-file`

---

## Options

Accepted keyword options (symbols or strings, with or without leading `:`):

| Key            | Reader               | Writer | Default           |
| -------------- | -------------------- | ------ | ----------------- |
| `delimiter`    | yes                  | yes    | `#\,`             |
| `quote`        | yes                  | yes    | `#\"`             |
| `header`       | yes                  | yes    | `#t`              |
| `header-row`   | yes                  | no     | `0`               |
| `trim`         | yes                  | no     | `#t`              |
| `comment`      | yes                  | no     | `#f`              |
| `column-names` | yes                  | yes    | `#f`              |
| `null-tokens`  | yes                  | no     | `("" "NA" "NaN")` |
| `quote-policy` | no                   | yes    | `'minimal`        |
| `infer-types?` | fact-table-load only | no     | `#t`              |

Writer `quote-policy` values:

- `'minimal`
- `'all`
- `'non-numeric`
- `'none`

---

## Typed Rows

`csv:read-typed-row` parses each cell as:

1. `nil` if it matches a null token.
2. Fixnum if integer parse succeeds (`from_chars`).
3. Flonum if floating parse succeeds (`from_chars`).
4. String otherwise.

This parsing is locale-independent. Example: `1,5` stays a string unless the CSV
delimiter is set so the cell text itself is a valid numeric form.

---

## FactTable Bridge

`std.fact_table` provides:

- `fact-table-load-csv`
- `fact-table-save-csv`

Examples:

```scheme
(import std.fact_table)

(define ft (fact-table-load-csv "trades.csv" :infer-types? #t))
(fact-table-save-csv ft "trades-out.csv")
```

`fact-table-load-csv` supports the reader options plus `:infer-types?`.
When `:infer-types? #f`, numeric-looking cells are kept as strings.



---

## Threading Note

CSV reader and writer handles are stateful heap objects and are not safe for concurrent
mutation from multiple Eta threads.
