# Time

[Back to README](../README.md) · [Modules and Stdlib](modules.md) · [Next Steps](next-steps.md)

---

## Quick Start

```scheme
(module demo
  (import std.time std.io)
  (begin
    ;; Wall-clock epoch values
    (println (time:now-ms))                     ;; e.g. 1745712345678
    (println (time:format-iso8601-utc 0))       ;; "1970-01-01T00:00:00Z"

    ;; Monotonic timing for benchmarks
    (let ((t0 (time:monotonic-ms)))
      (time:sleep-ms 50)
      (let ((t1 (time:monotonic-ms)))
        (println (time:elapsed-ms t0 t1))))     ;; ~50

    ;; Broken-down calendar parts
    (let ((parts (time:utc-parts (time:now-ms))))
      (println (cdr (assoc 'year   parts)))
      (println (cdr (assoc 'month  parts)))
      (println (cdr (assoc 'day    parts))))))
```

`std.time` is a thin Eta layer over native `%time-*` primitives. It has no
external dependencies and is auto-loaded by the prelude.

---

## API

### Wall-Clock & Monotonic Time

| Function | Signature | Notes |
|---|---|---|
| `time:now-ms` | `() -> int` | Unix epoch in **milliseconds**. |
| `time:now-us` | `() -> int` | Unix epoch in **microseconds**. |
| `time:now-ns` | `() -> int` | Unix epoch in **nanoseconds**. |
| `time:monotonic-ms` | `() -> int` | Monotonic clock; never goes backwards. Use for elapsed-time measurement. |
| `time:elapsed-ms` | `(start end) -> int` | Non-negative delta `end - start`; clamped to 0 if negative. |
| `time:sleep-ms` | `(ms) -> unspecified` | Block the current process for `ms` milliseconds. |

### Broken-Down Calendar Parts

`time:utc-parts` and `time:local-parts` take a millisecond epoch value and
return an association list with the following keys:

| Key | Range | Notes |
|---|---|---|
| `year` | full year, e.g. `2026` | |
| `month` | `1`–`12` | |
| `day` | `1`–`31` | Day of month. |
| `hour` | `0`–`23` | |
| `minute` | `0`–`59` | |
| `second` | `0`–`60` | `60` allows for leap seconds where the platform reports them. |
| `weekday` | `0`–`6` | `0` = Sunday. |
| `yearday` | `0`–`365` | Day of year. |
| `is-dst` | `bool` | DST in effect (always `#f` for UTC). |
| `offset-minutes` | int | Offset from UTC in minutes (always `0` for UTC). |

```scheme
(let ((parts (time:utc-parts (time:now-ms))))
  (cdr (assoc 'year parts)))     ;; => 2026
```

### ISO-8601 Formatting

| Function | Signature | Output |
|---|---|---|
| `time:format-iso8601-utc` | `(epoch-ms) -> string` | `"YYYY-MM-DDTHH:MM:SSZ"` (20 chars). |
| `time:format-iso8601-local` | `(epoch-ms) -> string` | `"YYYY-MM-DDTHH:MM:SS±HH:MM"` (25 chars). |

```scheme
(time:format-iso8601-utc 0)      ;; "1970-01-01T00:00:00Z"
(time:format-iso8601-local 0)    ;; e.g. "1969-12-31T19:00:00-05:00"
```

---

## Patterns

### Benchmarking

Always use the **monotonic** clock for elapsed-time measurement — wall-clock
time can jump (NTP adjustments, DST transitions, manual clock changes).

```scheme
(defun bench (thunk)
  (let ((t0 (time:monotonic-ms)))
    (let ((result (thunk)))
      (let ((t1 (time:monotonic-ms)))
        (cons result (time:elapsed-ms t0 t1))))))
```

### Timestamps in Logs

```scheme
(defun log-line (msg)
  (println (string-append
            "[" (time:format-iso8601-utc (time:now-ms)) "] " msg)))
```

### Throttled Polling

```scheme
(defun poll-every (interval-ms thunk)
  (let loop ()
    (thunk)
    (time:sleep-ms interval-ms)
    (loop)))
```

---

## Notes

- `time:now-ms` / `time:now-us` / `time:now-ns` all read the system wall clock
  and may move backwards if the clock is adjusted. Prefer
  `time:monotonic-ms` for durations and timeouts.
- All values are 47-bit fixnums where possible — millisecond, microsecond,
  and nanosecond epochs all fit comfortably without heap allocation
  (see [NaN-Boxing](nanboxing.md)).
- `time:sleep-ms` yields to the OS scheduler; resolution is platform-dependent
  (~1 ms on modern desktop OSes).

