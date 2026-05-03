# std.time

Clocks, sleep, and ISO-8601 formatting wrapping the `%time-*` primitives.

```scheme
(import std.time)
```

## Clocks

| Symbol | Description |
| --- | --- |
| `(time:now-ms)` | Wall-clock milliseconds since Unix epoch. |
| `(time:now-us)` | Wall-clock microseconds since Unix epoch. |
| `(time:now-ns)` | Wall-clock nanoseconds since Unix epoch. |
| `(time:monotonic-ms)` | Monotonic milliseconds. |

## Sleep and elapsed

| Symbol | Description |
| --- | --- |
| `(time:sleep-ms ms)` | Block the current fiber for `ms` milliseconds. |
| `(time:elapsed-ms thunk)` | Run `thunk` and return its elapsed time in milliseconds. |

## Formatting

| Symbol | Description |
| --- | --- |
| `(time:utc-parts ms)` | Decompose a UTC timestamp to `(year month day hour minute second ms)`. |
| `(time:local-parts ms)` | Same in local time. |
| `(time:format-iso8601-utc ms)` | Format UTC timestamp as ISO-8601 string. |
| `(time:format-iso8601-local ms)` | Format local timestamp as ISO-8601 string with offset. |

