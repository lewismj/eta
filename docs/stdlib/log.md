# std.log

Structured logger and sink construction over the `%log-*` runtime
primitives.

```scheme
(import std.log)
```

## Levels and emit

| Symbol | Description |
| --- | --- |
| `(log:trace fmt . args)` | Log at TRACE level. |
| `(log:debug fmt . args)` | Log at DEBUG level. |
| `(log:info fmt . args)` | Log at INFO level. |
| `(log:warn fmt . args)` | Log at WARN level. |
| `(log:error fmt . args)` | Log at ERROR level. |
| `(log:critical fmt . args)` | Log at CRITICAL level. |

Each call uses the default logger; pass a logger as the first argument to
target a specific logger by reference.

## Loggers

| Symbol | Description |
| --- | --- |
| `(log:default)` | Current default logger. |
| `(log:set-default! logger)` | Replace the default logger. |
| `(log:get-logger name)` | Lookup or create a named logger. |
| `(log:make-logger name sinks)` | Build a logger from sinks. |
| `(log:set-level! logger level)` | Set per-logger threshold. |
| `(log:level logger)` | Read the current level. |
| `(log:set-global-level! level)` | Set the global threshold. |
| `(log:set-pattern! logger pattern)` | Set spdlog-style format pattern. |
| `(log:set-formatter! logger formatter)` | Set a custom formatter. |
| `(log:flush! logger)` | Flush buffered output. |
| `(log:flush-on! logger level)` | Auto-flush at `level` and above. |
| `(log:shutdown!)` | Flush and stop background threads. |

## Sinks

| Symbol | Description |
| --- | --- |
| `(log:make-stdout-sink)` | Sink writing to stdout. |
| `(log:make-stderr-sink)` | Sink writing to stderr. |
| `(log:make-file-sink path)` | Sink writing to a file. |
| `(log:make-rotating-sink path max-size max-files)` | Size-based rotation. |
| `(log:make-daily-sink path hour minute)` | Daily rotation. |
| `(log:make-port-sink port)` | Sink writing to a Scheme port. |
| `(log:make-error-port-sink)` | Sink writing to the current error port. |

