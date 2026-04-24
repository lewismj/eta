# CSV Subsystem — Replace the Pure-Eta Loader with `vincentlaucsb/csv-parser`

[← Back to README](../README.md) · [Fact Tables](fact-table.md) ·
[Modules & Stdlib](modules.md) · [Architecture](architecture.md) ·
[Next Steps](next-steps.md)

---

## Scope

This document is the implementation plan for promoting CSV from a
pure-Eta hack (`examples/causal-factor/csv-loader.eta`) to a
first-class, native-backed `std.csv` module built on
[`vincentlaucsb/csv-parser`](https://github.com/vincentlaucsb/csv-parser)
(MIT, header-only, C++17).

It does **not** introduce Apache Arrow or Parquet — those are
deliberately deferred until a second consumer (columnar interchange,
Parquet on disk, DuckDB integration) actually appears. The public Eta
API is designed so the backend can be swapped later without churning
user code.

---

## Audit of the Current Implementation

> Findings from a fresh read of the source (April 2026).

### What exists

| Location | What it is |
|---|---|
| [`examples/causal-factor/csv-loader.eta`](../examples/causal-factor/csv-loader.eta) (115 lines) | The **only** CSV code in the repo. Lives under `examples/`, not `stdlib/`. Pure Eta. |
| `std.io` (`open-input-file`, `read-line`, ports) | The substrate it builds on. |

There is **no `std.csv`**, no native CSV builtin, no writer, no
streaming API, no test coverage. The file is `(module csv-loader …)`,
so anyone wanting CSV today has to copy-import it from `examples/`.

### Bugs in the current implementation

Reading `csv-loader.eta` carefully:

1. **`csv:split-on-comma` is broken beyond the first comma.**
   Lines 67–84:
   ```scheme
   (let ((field (get-output-string out)))
     (close-output-port out)
     (let ((new-out (open-output-string)))
       (loop (+ i 1) (cons field acc))))
   ```
   `new-out` is bound but never threaded back into the recursion — the
   outer `out` (now closed!) is what the next iteration writes to.
   Either it crashes on the second field or, depending on port
   semantics, silently appends to a dead buffer. Either way, **any CSV
   with more than one comma per line is mis-parsed**.

2. **No quoting / escaping at all.** `"hello, world"` becomes two
   fields. Embedded `""` escapes are not recognised. Embedded newlines
   inside quoted fields terminate the record.

3. **`csv:parse-value` interns arbitrary user data as symbols.** Line
   65: `(string->symbol (csv:trim str))`. On a 1M-row CSV with unique
   string-typed cells, this permanently bloats the intern table and
   leaks identifiers visible to `eq?`. Security/footprint smell.

4. **No CRLF / BOM handling.** Windows-saved CSVs carry `\r` at end of
   field (only `csv:trim` saves us, and only because `\r` is in
   `csv:whitespace?`). UTF-8 BOM on the header line corrupts the first
   column name.

5. **No writer.** Round-tripping a `FactTable` to disk requires the
   user to hand-roll comma joining and re-introduce all the bugs
   above.

6. **Whole-file model.** `csv:read-all` reverses an accumulated list at
   the end — no streaming, no constant-memory ingestion of large
   files. Memory cost is `O(rows × cols × LispVal)` plus the cons-cell
   tax.

7. **No tests.** `stdlib/tests/` has no CSV coverage. The bug in (1)
   has presumably never fired because demos use 1-comma headers and
   single-column rows, or the bug returns enough garbage to look like
   a parse failure that's silently ignored at line 44.

### Why a native backend is the right move

- ETA already takes large native deps for the same reason in adjacent
  domains: Eigen for linear algebra, libtorch for tensors, NNG for
  messaging. CSV is a peer of those — a "performance- and
  correctness-sensitive substrate".
- `vincentlaucsb/csv-parser` is single-header, MIT, C++17 — no
  toolchain burden, fits the existing `FetchContent` pattern from
  `cmake/FetchEigen.cmake`.
- All six bugs above disappear behind one library. RFC 4180 quoting,
  CRLF, BOM, configurable delimiter/quote/comment, type inference,
  streaming, parallel parsing, and a writer are all in-box.

---

## Design

### Layering

```
┌──────────────────────────────────────────────────────────────────┐
│ Eta user code                                                    │
│ (csv:read-record r) (fact-table-load-csv "trades.csv") …         │
└──────────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────────┐
│ stdlib/std/csv.eta            — variadic, port-style wrapper     │
│ stdlib/std/fact_table.eta     — adds csv→fact-table convenience  │
└──────────────────────────────────────────────────────────────────┘
                              │ %csv-* primitives
┌──────────────────────────────────────────────────────────────────┐
│ eta/core/.../builtins/csv_builtins.{h,cpp}                       │
│   - opaque CsvReader / CsvWriter heap objects                    │
│   - registered through core_primitives.h                         │
│   - GC-visited (file handle finalised on sweep)                  │
└──────────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────────┐
│ vincentlaucsb/csv-parser (header-only, fetched via CMake)        │
└──────────────────────────────────────────────────────────────────┘
```

Backend isolation: nothing above the `csv_builtins` translation unit
includes `csv.hpp`. The public Eta API never names the library, so
swapping to Arrow CSV later is an internal refactor.

### Heap object kinds

Two new opaque types, registered alongside `FactTable` in `heap.h` /
`heap_visit.h`:

| Kind | Owns | GC behaviour |
|---|---|---|
| `ObjectKind::CsvReader` | `std::unique_ptr<csv::CSVReader>` plus cached header (`std::vector<std::string>`) and interned column-name symbols (`std::vector<LispVal>`) | Visitor walks the cached symbol vector to keep it alive. Finalizer closes the reader (RAII via `unique_ptr`). |
| `ObjectKind::CsvWriter` | `std::unique_ptr<std::ofstream>` plus a `csv::DelimWriter` configured at construction | Finalizer flushes + closes the stream. No `LispVal`s held. |

Both are GC-managed exactly like `FactTable` — no manual `close`
required for correctness, but `(csv:close r)` is provided for prompt
finalisation.

### Native primitives

Registered in `core_primitives.h`. All `%`-prefixed; user code calls
the wrappers in `std.csv`.

| Builtin | Args | Returns |
|---|---|---|
| `%csv-open-reader` | `(path opts-alist)` | reader handle |
| `%csv-reader-from-string` | `(str opts-alist)` | reader handle (in-memory) |
| `%csv-columns` | `(reader)` | vector of header symbols |
| `%csv-read-row` | `(reader)` | vector of strings, or `#f` at EOF |
| `%csv-read-record` | `(reader)` | alist `((sym . val) …)`, or `#f` at EOF |
| `%csv-read-typed-row` | `(reader)` | vector of values (int / flo / str), or `#f` |
| `%csv-row-index` | `(reader)` | fixnum (rows consumed so far) |
| `%csv-close` | `(handle)` | unspecified |
| `%csv-open-writer` | `(path opts-alist)` | writer handle |
| `%csv-write-row` | `(writer row-vec-or-list)` | unspecified |
| `%csv-write-record` | `(writer alist columns-vec)` | unspecified |
| `%csv-flush` | `(writer)` | unspecified |
| `%csv-reader?` / `%csv-writer?` | `(value)` | bool |

`opts-alist` keys (all optional, sensible defaults match RFC 4180):

| Key | Default | Maps to `csv-parser` |
|---|---|---|
| `delimiter` | `#\,` | `CSVFormat::delimiter(...)` |
| `quote` | `#\"` | `CSVFormat::quote(...)` |
| `header` | `#t` | `CSVFormat::header_row(0)` vs `no_header()` |
| `header-row` | `0` | `CSVFormat::header_row(n)` |
| `comment` | `#f` | `CSVFormat::comment(...)` (e.g. `#\#`) |
| `trim` | `#t` | `CSVFormat::trim({' ', '\t'})` |
| `column-names` | `#f` | `CSVFormat::column_names(...)` (override / supply when `header` is `#f`) |
| `null-tokens` | `("" "NA" "NaN")` | applied during typed reads |
| `quote-policy` | `'minimal` | writer: `'minimal` / `'all` / `'non-numeric` / `'none` |

### Eta-level API (`stdlib/std/csv.eta`)

```scheme
(module csv
  (import std.io)
  (begin

    ;; --- reader ---------------------------------------------------------
    (defun csv:open-reader (path . opts)            ; opts: keyword pairs
      (%csv-open-reader path (csv:opts->alist opts)))

    (defun csv:reader-from-string (str . opts)
      (%csv-reader-from-string str (csv:opts->alist opts)))

    (defun csv:columns      (r) (%csv-columns r))
    (defun csv:read-row     (r) (%csv-read-row r))
    (defun csv:read-record  (r) (%csv-read-record r))
    (defun csv:read-typed-row (r) (%csv-read-typed-row r))
    (defun csv:close        (h) (%csv-close h))

    ;; (csv:fold reader f init) — streaming, constant memory.
    (defun csv:fold (r f init)
      (let loop ((acc init))
        (let ((row (%csv-read-record r)))
          (if row (loop (f acc row)) acc))))

    ;; (csv:for-each reader f) — same, no accumulator.
    (defun csv:for-each (r f)
      (let loop ()
        (let ((row (%csv-read-record r)))
          (when row (f row) (loop)))))

    ;; (csv:collect reader) — eager list of records (small files only).
    (defun csv:collect (r)
      (reverse (csv:fold r (lambda (acc rec) (cons rec acc)) '())))

    ;; --- writer ---------------------------------------------------------
    (defun csv:open-writer (path . opts)
      (%csv-open-writer path (csv:opts->alist opts)))

    (defun csv:write-row     (w row)              (%csv-write-row    w row))
    (defun csv:write-record  (w alist columns)    (%csv-write-record w alist columns))
    (defun csv:flush         (w)                  (%csv-flush        w))

    ;; --- convenience ----------------------------------------------------
    (defun csv:load-file (path . opts)
      (let* ((r (apply csv:open-reader path opts))
             (rows (csv:collect r)))
        (csv:close r)
        rows))

    (defun csv:save-file (path rows . opts)
      (let* ((columns (map* car (car rows)))
             (w       (apply csv:open-writer path
                             (cons 'column-names (cons columns opts)))))
        (csv:for-each-list rows (lambda (rec) (csv:write-record w rec columns)))
        (csv:close w)))

    ;; (csv:opts->alist '(:delimiter #\; :header #f)) → ((delimiter . #\;) ...)
    ;; … helper omitted here for brevity …
  ))
```

### `FactTable` bridge (in `std.fact_table`)

Two thin functions, native-backed by a new `%fact-table-load-csv`
primitive that streams rows directly into the column vectors (no
intermediate Eta lists, no allocation per cell beyond what
`LispVal`-boxing requires):

```scheme
(fact-table-load-csv "trades.csv"
                     :infer-types? #t
                     :null-tokens  '("" "NA" "NaN"))
;; → fact-table

(fact-table-save-csv ft "trades.csv")
;; → unspecified
```

Type inference per column:

- All cells parse as fixnum → `int64` column.
- All cells parse as flonum (or fixnum) → `float64`.
- Any cell fails both → `string`. Symbol interning is **off by
  default** here — the old loader's `string->symbol` fallback is
  exactly the bug from §"Bugs" item (3).
- Empty / `null-tokens` → `nil`.

---

## Implementation Stages

Each stage is independently shippable and ends with green tests.

### S0 — Vendor the library (½ day, low risk)

- **New file** `cmake/FetchCsvParser.cmake`, modelled on
  `cmake/FetchEigen.cmake`:
  ```cmake
  include(FetchContent)
  FetchContent_Declare(
      csv_parser
      GIT_REPOSITORY https://github.com/vincentlaucsb/csv-parser.git
      GIT_TAG        2.3.0          # pin to a release tag
      GIT_SHALLOW    TRUE
      EXCLUDE_FROM_ALL
      SYSTEM
  )
  set(CSV_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
  set(BUILD_PYTHON       OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(csv_parser)
  ```
  Expose `csv::parser` (or add an `INTERFACE` target wrapping the
  `single_include/csv.hpp` header — the upstream project provides
  both). Link `eta_core` `PRIVATE` to it.
- **`eta/CMakeLists.txt`**: `include(FetchCsvParser)` unconditionally,
  alongside `FetchEigen`. CSV is a hard dependency of `eta_core` —
  no opt-out, no compile-time gating, no `#ifdef`s.
- **DoD.** `cmake --build build --target eta_core` succeeds on
  Linux/Windows/macOS.

### S1 — Reader-only native primitives (2–3 days, low risk)

- **New** `eta/core/src/eta/runtime/builtins/csv_builtins.{h,cpp}`.
- Implement: `%csv-open-reader`, `%csv-reader-from-string`,
  `%csv-columns`, `%csv-read-row`, `%csv-read-record`, `%csv-close`,
  `%csv-reader?`.
- Add `ObjectKind::CsvReader` to `memory/heap.h`,
  `memory/heap_visit.h`, `value_formatter.h`
  (`#<csv-reader rows=NN>`), GC visitor in
  `memory/mark_sweep_gc.h` (walks cached header symbols).
- Wire registration in `core_primitives.h`.
- **Tests** `eta/test/src/csv_reader_tests.cpp` (Catch2 / project test
  framework):
  - RFC 4180 quoted fields, escaped `""`, embedded `,` and `\n`.
  - CRLF and LF line endings.
  - UTF-8 BOM handling.
  - Custom delimiter (`;`), custom quote (`'`), comment lines (`#`).
  - Header skipping; explicit `column-names` override when
    `header: #f`.
  - Streaming a 1M-row file without holding it in memory (verify peak
    RSS via `/proc/self/status` or hand-rolled allocator counter).
- **DoD.** All tests green on the three OSes.

### S2 — `std.csv` Eta wrapper + tests (1 day, low risk)

- **New** `stdlib/std/csv.eta` per §"Eta-level API".
- **New** `stdlib/tests/csv.test.eta`:
  - Round-trip a known-bad input (the failing case from bug #1) and
    assert correctness.
  - `csv:fold` over a 100k-row in-memory CSV terminates with the right
    sum.
  - `csv:read-record` keys are interned symbols (so `eq?` works
    between calls).
- **Update** `editors/vscode/snippets/eta.json` with `csv:open-reader`,
  `csv:fold`, `csv:write-record` snippets.
- **DoD.** `eta_test stdlib/tests/csv.test.eta` green.

### S3 — Writer (1–2 days, low risk)

- Add `ObjectKind::CsvWriter`, `%csv-open-writer`, `%csv-write-row`,
  `%csv-write-record`, `%csv-flush`, `%csv-writer?`.
- Use `csv::DelimWriter<std::ofstream>` from the library; map
  `quote-policy` to its quoting modes.
- **Tests**:
  - Round-trip: random `vector<vector<string>>` → write → read →
    bytewise equal at the value level.
  - Embedded `"`, `,`, `\n` escape correctly under each
    `quote-policy`.
  - `flush` is observable mid-stream.
- **DoD.** Round-trip property test (≥ 1k random tables) passes.

### S4 — Typed reads + null tokens (1–2 days, medium risk)

- Implement `%csv-read-typed-row`. Per cell:
  - `null-tokens` → `nil`.
  - Try fixnum (`std::from_chars` for speed) → `LispVal::fixnum`.
  - Try flonum → `LispVal::flonum`.
  - Fall back to `LispVal::string` (heap-allocated, **never**
    `string->symbol`).
- Document the fallback rule prominently in `docs/csv.md` (created in
  S6) — it's a deliberate departure from the old loader.
- **Tests**:
  - Mixed-type column resolves cell-by-cell.
  - `"NA"` and empty cells return `nil`.
  - Locale-independent number parsing (German `1,5` is **not**
    silently parsed as `1.5`; `;`-delimited German CSVs work via
    `delimiter: #\;`).
- **DoD.** A test fixture covering all three numeric edge cases is
  green.

### S5 — `FactTable` bridge (2–3 days, medium risk)

- **New** `%fact-table-load-csv` and `%fact-table-save-csv` primitives
  in `core_primitives.h`, implemented in
  `csv_builtins.cpp` (so the dependency edge from `fact_table.h` to
  `csv.hpp` is one-way).
- Streaming column-builder: walk rows once, push directly into
  `FactTable::columns[col_idx]`. Two-pass for type inference is OK
  for v1 (read the file once for type detection on a sample of N
  rows, then again for ingestion); `infer-types?: #f` skips the first
  pass and stores everything as strings.
- **Update** `stdlib/std/fact_table.eta` to expose
  `fact-table-load-csv` and `fact-table-save-csv`.
- **Tests** `stdlib/tests/fact_table.test.eta` extended:
  - Load `examples/causal-factor/sample-observations.csv` (or
    generate one) and verify column count, row count, dtype per
    column.
  - Save → reload round-trip preserves cells (modulo string vs
    inferred number, documented).
- **DoD.** End-to-end `(fact-table-load-csv …)` works in
  `examples/causal-factor/causal_demo.eta`.

### S6 — Documentation (1 day)

- **New** `docs/csv.md`:
  - Quick start.
  - API reference (mirrors §"Eta-level API").
  - Type-inference rules.
  - Performance notes (streaming, expected throughput).
  - Migration table for the old `csv-loader.eta` (see §"Migration
    Path").
- **Update** `docs/fact-table.md` with a "Loading from CSV" section
  pointing at `csv.md`.
- **Update** `README.md` examples section.
- **Update** `docs/next-steps.md`: remove any "CSV needs work" item;
  link to `csv.md`.

### S7 — Migrate the old loader (½ day)

- **Delete** `examples/causal-factor/csv-loader.eta`.
- **Update** every importer (`examples/causal-factor/causal_demo.eta`
  and any other in-tree consumer found via `grep -r csv-loader`) to
  `(import std.csv)` and rename call sites to the new API per the
  §"Migration Path" table.
- **No shim, no deprecation alias.** The old module name disappears
  cleanly.
- **DoD.** `grep -r csv-loader .` returns no matches anywhere in the
  repo.

---

## Migration Path for User Code

| Old (`examples/causal-factor/csv-loader.eta`) | New (`std.csv`) |
|---|---|
| `(csv:load-file path columns)` | `(csv:load-file path :column-names columns)` *(if header missing)* or just `(csv:load-file path)` *(headers in file)* |
| `(csv:read-all port columns)` | `(csv:open-reader path …)` + `(csv:collect r)` |
| `(csv:parse-row line columns)` | `(csv:reader-from-string line …)` + `(csv:read-record r)` |
| `(csv:make-sample-data columns rows)` | unchanged — pure Eta helper, lift into `std.csv` as-is |
| n/a | `(csv:open-writer …)`, `(csv:write-record …)` |
| n/a | `(fact-table-load-csv …)` |

Behavioural differences callers must know about:

1. **String cells stay strings.** The old `csv:parse-value`
   `string->symbol` fallback is gone. If callers depend on `eq?` on
   string-typed cells, they must `string->symbol` themselves.
2. **Quoting actually works.** Files that "round-tripped" only because
   their content avoided commas inside fields will now parse correctly
   instead of "differently wrong".
3. **`csv:fold` exists.** Callers manually walking ports row-by-row
   should switch to it.

---

## Sequencing & Sizing

| Stage | Days | Risk | Blocks |
|---|---:|:---:|---|
| S0 Vendor library | 0.5 | Low | all later |
| S1 Reader primitives | 2–3 | Low | S2, S5 |
| S2 `std.csv` wrapper | 1 | Low | S5, S7 |
| S3 Writer | 1–2 | Low | S5 |
| S4 Typed reads | 1–2 | Med | S5 |
| S5 `FactTable` bridge | 2–3 | Med | S7 |
| S6 Documentation | 1 | Low | release |
| S7 Migrate examples | 0.5 | Low | release |

**Total: ~9–13 engineer-days**, mostly serial S0 → S7. S3 and S4 can
proceed in parallel after S1.

---

## Cross-cutting Concerns

### Build / packaging

- `vincentlaucsb/csv-parser` is header-only — no DLL plumbing, no
  `install(TARGETS …)`. The release-bundle scripts
  (`scripts/build-release.*`) need no change.
- CSV is an unconditional dependency of `eta_core`. There is no
  CMake option, no compile-time flag, no `#ifdef` guard — if you
  build ETA, you get CSV.
- License compatibility: csv-parser MIT, ETA — fine.

### Backwards compatibility

- The old `csv-loader.eta` module is **removed**, not aliased. Every
  in-tree caller is migrated in S7 to use `std.csv` directly.
- Out-of-tree callers (if any exist) must update imports and call
  sites per §"Migration Path"; this is a one-time, mechanical change.
- The behavioural change in §"Migration Path" item (1) — string cells
  no longer auto-intern as symbols — is called out in release notes.

### Performance

- Expected throughput: 500 MB/s – 1 GB/s on a single core for the
  reader (library benchmark numbers). The Eta side will be dominated
  by `LispVal` boxing, not parsing.
- `%fact-table-load-csv` avoids the `LispVal`-list-of-`LispVal`
  intermediate that a "read all rows then push" would create — the
  whole point of doing it natively.

### Memory & GC

- Reader holds a `std::ifstream` (or `std::stringstream` for the
  in-memory variant). RAII closes it on sweep.
- Cached header symbols are kept alive by the reader's GC visitor so
  `eq?`-on-keys works across calls without re-interning per row.
- No retained references to parsed rows after `%csv-read-record`
  returns — the row vector is freshly allocated each call, so streams
  of arbitrary size run in constant heap.

### Thread safety

- Each reader / writer is a heap object with its own state;
  concurrent access from multiple Eta threads is **not** supported in
  v1. Document this in `docs/csv.md`. (The parallel parser inside
  `csv-parser` is internal to a single reader — fine.)

### Backwards compatibility

- The old `csv-loader.eta` API symbols (`csv:load-file`,
  `csv:read-all`, `csv:parse-row`, `csv:parse-value`,
  `csv:split-on-comma`, `csv:trim`, `csv:make-sample-data`) are
  preserved, either via the shim (S7 option A) or re-exported from
  `std.csv` with the same arities and same return shapes.
- `csv:parse-value`'s symbol-fallback removal is the **only**
  intentional break; it's noted in §"Migration Path" and called out
  in release notes.

### Security

- The library does not execute untrusted input as code; CSV is data.
- File-path arguments to `%csv-open-reader` go through the same
  `std.io` path-validation that `open-input-file` does today.
- Opt-in `comment` character means `#`-prefixed user data is not
  silently dropped unless requested.

---

## What We Are NOT Doing (in this plan)

- **Apache Arrow / Parquet.** Deliberately deferred. The public API
  is shaped so the backend can be swapped to `arrow::csv::TableReader`
  later without churning any `.eta` file.
- **TSV / fixed-width / Excel.** `delimiter: #\tab` covers TSV; the
  rest are out of scope.
- **Schema validation DSL.** Type inference is per-column only; no
  declarative schema (yet). Add when a second consumer needs it.
- **Async / coroutine reads.** The synchronous streaming API is
  enough until ETA itself has language-level async.
- **Compression** (`.csv.gz`, `.csv.zst`). One-line addition once
  needed, but not in v1.

---

## Definition of Done

The CSV subsystem is "done" when:

1. All bugs in §"Bugs in the current implementation" are
   demonstrably fixed by tests in `csv_reader_tests.cpp` and
   `stdlib/tests/csv.test.eta`.
2. `(fact-table-load-csv "x.csv")` works end-to-end and is used by at
   least one example under `examples/`.
3. `docs/csv.md` exists; `README.md`, `docs/fact-table.md`, and
   `docs/next-steps.md` link to it.
4. `examples/causal-factor/csv-loader.eta` is deleted and every
   in-tree caller imports `std.csv` instead — `grep -r csv-loader .`
   returns no matches.
5. Reader streams a 1M-row file in constant heap (peak-RSS test
   green).

---

## Source Locations Referenced

| Component | File |
|---|---|
| Old CSV loader | [`examples/causal-factor/csv-loader.eta`](../examples/causal-factor/csv-loader.eta) |
| Eigen fetch (template) | [`cmake/FetchEigen.cmake`](../cmake/FetchEigen.cmake) |
| FactTable struct | [`eta/core/src/eta/runtime/types/fact_table.h`](../eta/core/src/eta/runtime/types/fact_table.h) |
| Heap kinds / visitor | [`eta/core/src/eta/runtime/memory/heap.h`](../eta/core/src/eta/runtime/memory/heap.h), [`heap_visit.h`](../eta/core/src/eta/runtime/memory/heap_visit.h) |
| Mark-sweep GC | [`eta/core/src/eta/runtime/memory/mark_sweep_gc.h`](../eta/core/src/eta/runtime/memory/mark_sweep_gc.h) |
| Factory | [`eta/core/src/eta/runtime/factory.h`](../eta/core/src/eta/runtime/factory.h) |
| Value formatter | [`eta/core/src/eta/runtime/value_formatter.h`](../eta/core/src/eta/runtime/value_formatter.h) |
| Core primitives registry | [`eta/core/src/eta/runtime/core_primitives.h`](../eta/core/src/eta/runtime/core_primitives.h) |
| `std.fact_table` | [`stdlib/std/fact_table.eta`](../stdlib/std/fact_table.eta) |
| `std.io` | [`stdlib/std/io.eta`](../stdlib/std/io.eta) |
| Snippets | [`editors/vscode/snippets/eta.json`](../editors/vscode/snippets/eta.json) |
| Upstream library | <https://github.com/vincentlaucsb/csv-parser> |

