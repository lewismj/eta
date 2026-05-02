# `std.log` — Structured Logging

[← Language Guide](../../language_guide.md) ·
[Modules & Stdlib](./modules.md) ·
[I/O](../io.md)

`std.log` is Eta's structured-logging façade, backed by the native
[`spdlog`](https://github.com/gabime/spdlog) runtime that ships inside
the VM. It exposes named loggers, multiple sinks, configurable levels,
flush policies, and two formatters (a human-readable line layout and a
machine-readable JSON layout). Every wrapper in `std.log` is a thin
veneer over a `%log-*` runtime primitive, so the surface is small,
allocation-free per call, and safe to use from hot paths.

```scheme
(import std.log)

(let* ((sink   (log:make-stdout-sink))
       (logger (log:make-logger "app" (list sink))))
  (log:set-default! logger)
  (log:info "service started" '((port . 8080)))
  (log:warn "slow query" '((ms . 412) (table . "orders")))
  (log:flush! logger))
```

```text
[2026-04-29 09:14:01.221] [info]     [app] service started port=8080
[2026-04-29 09:14:01.221] [warning]  [app] slow query ms=412 table=orders
```

---

## Concepts

### Loggers

A **logger** is a named handle that owns a list of sinks, a level
threshold, a pattern, a formatter mode, and a flush policy. Build one
with `log:make-logger`, retrieve a previously registered one by name
with `log:get-logger`, or use the implicit default returned by
`log:default`.

```scheme
(define request-log
  (log:make-logger "http.requests"
                   (list (log:make-stdout-sink)
                         (log:make-rotating-sink "logs/http.log"
                                                  (* 10 1024 1024) 5))))
```

### Sinks

A **sink** is a single output destination. A logger fans every accepted
record out to all of its sinks. The provided constructors are listed
below.

| Constructor                                                  | Destination                                              |
| :----------------------------------------------------------- | :------------------------------------------------------- |
| `(log:make-stdout-sink ['color? bool])`                      | Standard output, optional ANSI colour (default `#t`).    |
| `(log:make-stderr-sink ['color? bool])`                      | Standard error, optional ANSI colour (default `#t`).    |
| `(log:make-file-sink path ['truncate? bool])`                | Plain file; truncates if `'truncate? #t`.                |
| `(log:make-rotating-sink path max-size max-files)`           | Size-rotated file (bytes per file, file count cap).      |
| `(log:make-daily-sink path hour minute max-files)`           | Daily-rotated file rolled at the given local-time clock. |
| `(log:make-port-sink port)`                                  | Any open output port (string ports included).            |
| `(log:make-error-port-sink)`                                 | Tracks `(current-error-port)`; respects `with-error-to-port`. |

> [!TIP]
> The error-port sink is the one to reach for inside tests — it captures
> output via `with-error-to-port` without touching the real stderr.

### Levels

Levels match `spdlog`'s ordering, lowest to highest:

```text
trace < debug < info < warn < error < critical < off
```

They are passed and read as plain Eta symbols (`'trace`, `'debug`,
`'info`, `'warn`, `'error`, `'critical`, `'off`). `'warning'` and
`'err'` are accepted as aliases on input.

| Function                              | Effect                                              |
| :------------------------------------ | :-------------------------------------------------- |
| `(log:set-level! logger level)`       | Drop records below `level` for this logger only.    |
| `(log:level logger)`                  | Read the current threshold as a symbol.             |
| `(log:set-global-level! level)`       | Process-wide floor applied to *every* logger.       |

### Patterns and formatters

A **pattern** is a `spdlog` format string ([reference](https://github.com/gabime/spdlog/wiki/3.-Custom-formatting));
the default is

```text
[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v
```

A **formatter** decides how the message and its payload alist are
rendered into the `%v` slot. Two modes are bundled:

| Formatter | `log:set-formatter!` value | Output for `(log:info "boom" '((code . 7)))` |
| :-------- | :------------------------- | :-------------------------------------------- |
| Human     | `'human` (default)         | `boom code=7`                                 |
| JSON      | `'json`                    | `{"msg":"boom","code":7}`                     |

For JSON output, set the pattern to `"%v"` so the line is the raw JSON
record — useful for ingestion by structured log shippers.

```scheme
(log:set-pattern!   logger "%v")
(log:set-formatter! logger 'json)
```

### Flush policy

| Function                                  | Effect                                                       |
| :---------------------------------------- | :----------------------------------------------------------- |
| `(log:flush! logger)`                     | Force every sink to flush queued records.                    |
| `(log:flush-on! logger level)`            | Auto-flush whenever a record at `level` or higher is emitted. |
| `(log:shutdown!)`                         | Flush and tear down every registered logger.                  |

Call `log:shutdown!` from a top-level `dynamic-wind` *after* clause if
your program may exit non-normally — `spdlog`'s async sinks rely on it
to drain.

---

## Emitting records

Every level wrapper accepts three call shapes:

```scheme
(log:info "message")                          ; default logger, no payload
(log:info "message" '((key . value) ...))     ; default logger + payload
(log:info logger "message")                   ; explicit logger
(log:info logger "message" '((key . val)))    ; explicit logger + payload
```

The full set is `log:trace`, `log:debug`, `log:info`, `log:warn`,
`log:error`, and `log:critical`. The dispatcher decides which form was
used by inspecting whether the first argument is a string (default
logger) or a logger value (explicit logger), so don't pass a string as
the first positional argument when you mean to target an explicit
logger.

The payload is an association list of `(symbol . value)` pairs. The
human formatter renders it as space-separated `key=value` tokens; the
JSON formatter merges it into the top-level object alongside `"msg"`.

---

## Default logger

`log:default` returns the implicit logger used when you call a level
wrapper with no logger argument. It is created lazily and writes to
stderr in human format. Replace it once at startup with
`log:set-default!`:

```scheme
(let ((root (log:make-logger "root"
                             (list (log:make-stderr-sink)
                                   (log:make-file-sink "logs/app.log")))))
  (log:set-default! root)
  (log:flush-on! root 'warn))
```

`log:get-logger` retrieves an already-created logger by name, returning
`#f` if none is registered.

---

## Option syntax

The variadic constructors (`log:make-stdout-sink`, `log:make-stderr-sink`,
`log:make-file-sink`) accept alternating keyword/value pairs. Both
keyword-style symbols (`':color?`) and bare symbols (`'color?`) work,
as do strings (`"color?"`); unknown keys raise an error.

```scheme
(log:make-stdout-sink 'color? #f)
(log:make-file-sink "build.log" ':truncate? #t)
```

| Constructor              | Allowed keys |
| :----------------------- | :----------- |
| `log:make-stdout-sink`   | `color?`     |
| `log:make-stderr-sink`   | `color?`     |
| `log:make-file-sink`     | `truncate?`  |

---

## API summary

| Function                                                     | Purpose |
| :----------------------------------------------------------- | :------ |
| `log:default`                                                | Return the implicit default logger. |
| `log:set-default! logger`                                    | Install a new default logger. |
| `log:get-logger name`                                        | Look up a logger by name (or `#f`). |
| `log:make-logger name sinks`                                 | Create and register a new logger. |
| `log:make-stdout-sink ['color? bool]`                        | Construct a coloured stdout sink. |
| `log:make-stderr-sink ['color? bool]`                        | Construct a coloured stderr sink. |
| `log:make-file-sink path ['truncate? bool]`                  | Construct a plain file sink. |
| `log:make-rotating-sink path max-size max-files`             | Construct a size-rotated file sink. |
| `log:make-daily-sink path hour minute max-files`             | Construct a daily-rotated file sink. |
| `log:make-port-sink port`                                    | Construct a sink that writes to an output port. |
| `log:make-error-port-sink`                                   | Construct a sink bound to `(current-error-port)`. |
| `log:set-level! logger level`                                | Set the per-logger threshold. |
| `log:level logger`                                           | Read the per-logger threshold. |
| `log:set-global-level! level`                                | Set the process-wide threshold. |
| `log:set-pattern! logger pattern`                            | Replace the spdlog format string. |
| `log:set-formatter! logger 'human` &#124; `'json`            | Switch payload rendering. |
| `log:flush! logger`                                          | Flush all sinks immediately. |
| `log:flush-on! logger level`                                 | Auto-flush at or above `level`. |
| `log:shutdown!`                                              | Flush and dispose every logger. |
| `log:trace`, `log:debug`, `log:info`, `log:warn`, `log:error`, `log:critical` | Level-tagged emit wrappers. |

---

## Recipes

### Capture log output in a test

```scheme
(import std.test std.log std.io)

(make-test "log emission"
  (lambda ()
    (let ((p (open-output-string)))
      (with-error-to-port p
        (lambda ()
          (let* ((sink   (log:make-error-port-sink))
                 (logger (log:make-logger "test.log" (list sink))))
            (log:set-pattern! logger "%v")
            (log:warn logger "boom" '((code . 7)))
            (log:flush! logger))))
      (assert-true (string-contains? (get-output-string p) "boom code=7")))))
```

### JSON logs to a rotating file

```scheme
(let* ((sink   (log:make-rotating-sink "logs/app.json" (* 10 1024 1024) 7))
       (logger (log:make-logger "app.json" (list sink))))
  (log:set-pattern!   logger "%v")
  (log:set-formatter! logger 'json)
  (log:set-level!     logger 'info)
  (log:flush-on!      logger 'error)
  (log:set-default!   logger))
```

### Per-component loggers

```scheme
(define http-log
  (log:make-logger "http"
                   (list (log:make-stderr-sink 'color? #f))))

(define db-log
  (log:make-logger "db"
                   (list (log:make-file-sink "logs/db.log" 'truncate? #t))))

(log:set-level! http-log 'info)
(log:set-level! db-log   'debug)
```

---

## Source & tests

- Wrapper module: [`stdlib/std/log.eta`](../../../stdlib/std/log.eta)
- TAP tests: [`stdlib/tests/log.test.eta`](../../../stdlib/tests/log.test.eta)
- Native primitives: [`eta/builtins/log/`](../../../eta/builtins/log/) (built on
  [`spdlog`](https://github.com/gabime/spdlog))


