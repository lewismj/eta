# eta-log

Structured logging for Eta, backed by [spdlog](https://github.com/gabime/spdlog).

`eta-log` is a native sidecar that provides the `%log-*` runtime primitives used
by the `std.log` Eta module.  The high-level Eta API lives in `std/log.eta`;
this package ships the compiled native library that makes those primitives
available.

## Quick start

```eta
(import std.log)

;; Use the default logger (writes to current-error-port)
(log:info "server started" '((port . 8080)))
(log:warn "slow query" '((ms . 1250) (table . "orders")))
(log:error "unexpected nil" '())
```

## Module

Import `std.log` in your Eta programme:

```eta
(import std.log)
```

## Sinks

A **sink** is a log destination.  Compose one or more sinks into a logger.

| Constructor | Description |
|---|---|
| `(log:make-stdout-sink)` | Coloured stdout sink (disable colour with `:color? #f`) |
| `(log:make-stderr-sink)` | Coloured stderr sink |
| `(log:make-file-sink path)` | Plain file sink; accepts `:truncate? #t` to overwrite on open |
| `(log:make-rotating-sink path max-size max-files)` | Rolling file; rotates when `max-size` bytes reached |
| `(log:make-daily-sink path hour minute max-files)` | Daily rolling file; rolls at `hour:minute` |
| `(log:make-port-sink port)` | Writes to any open Eta output port |
| `(log:make-error-port-sink)` | Writes to `current-error-port` (used by the default logger) |

```eta
(import std.log)

;; File sink, truncated on each run
(let* ((sink   (log:make-file-sink "app.log" 'truncate? #t))
       (logger (log:make-logger "app" (list sink))))
  (log:info logger "application started"))
```

```eta
;; Multi-sink logger: stdout + rotating file
(import std.log)

(let* ((out  (log:make-stdout-sink))
       (file (log:make-rotating-sink "app.log" (* 10 1024 1024) 5))
       (log  (log:make-logger "app" (list out file))))
  (log:set-level! log 'debug)
  (log:info  log "ready")
  (log:debug log "connection pool size" '((size . 4))))
```

## Loggers

| Function | Description |
|---|---|
| `(log:make-logger name sinks)` | Create a named logger with a list of sinks |
| `(log:default)` | Return the session default logger (error-port sink) |
| `(log:set-default! logger)` | Install a logger as the session default |
| `(log:get-logger name)` | Look up a previously registered named logger, or `#f` |

```eta
(import std.log)

(let ((logger (log:make-logger "risk" (list (log:make-stdout-sink)))))
  (log:info logger "model loaded"))

;; Retrieve a named logger anywhere in the session
(let ((logger (log:get-logger "risk")))
  (when logger
    (log:warn logger "model diverged")))
```

## Logging calls

All level functions accept two call shapes:

```
(log:info msg)                 ; use default logger, no payload
(log:info msg payload)         ; use default logger, with alist payload
(log:info logger msg)          ; use explicit logger, no payload
(log:info logger msg payload)  ; use explicit logger, with alist payload
```

| Level | Function |
|---|---|
| Trace | `log:trace` |
| Debug | `log:debug` |
| Info | `log:info` |
| Warning | `log:warn` |
| Error | `log:error` |
| Critical | `log:critical` |

```eta
(import std.log)

(log:info  "order placed"    '((id . "ord-001") (qty . 100)))
(log:warn  "retry attempt"   '((n . 3) (max . 5)))
(log:error "payment failed"  '((code . 402) (msg . "insufficient funds")))
```

## Level control

```eta
(import std.log)

(let ((logger (log:make-logger "svc" (list (log:make-stdout-sink)))))
  (log:set-level! logger 'warn)          ; this logger only emits warn+
  (display (log:level logger))           ; => warn
  (log:set-global-level! 'info)          ; global minimum across all loggers
  (log:flush-on! logger 'error)          ; auto-flush on every error
  (log:flush! logger))                   ; manual flush
```

Valid level symbols: `'trace` `'debug` `'info` `'warn` `'error` `'critical` `'off`.

The environment variable `ETA_LOG_LEVEL` sets the initial level of the default
logger when it is first created.

## Formatters and patterns

```eta
(import std.log)

(let ((logger (log:make-logger "svc" (list (log:make-stdout-sink)))))
  ;; Structured JSON output (useful for log aggregators)
  (log:set-formatter! logger 'json)

  ;; Or a raw spdlog pattern string
  (log:set-pattern! logger "[%H:%M:%S] [%l] %v"))
```

Valid formatter symbols: `'human` (default, timestamped) · `'json`.

## Shutdown

```eta
;; At programme exit: flush all sinks and release spdlog resources.
(log:shutdown!)
```

## Environment

| Variable | Effect |
|---|---|
| `ETA_LOG_LEVEL` | Initial level for the default logger (`trace` `debug` `info` `warn` `error` `critical` `off`) |
