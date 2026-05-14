# std.prof

[← Stdlib Reference](./README.md) · [Language Guide](../../language_guide.md)

Runtime profiling helpers for Eta programs. Wraps the `%prof-*` runtime
primitives for both trace and sample sessions.

```scheme
(import std.prof)
```

## API

| Symbol | Description |
| --- | --- |
| `(prof/start [mode] [hz])` | Start a profiling session; returns a session handle. |
| `(prof/stop session)` | Stop the session; returns a report handle or `#f`. |
| `(prof/report handle [format])` | Render the report as a string. |
| `(prof/counter name n)` | Increment a named counter by `n`. |
| `(prof/with thunk [mode] [hz])` | Run `thunk`; returns `(values result handle)`. |
| `(prof/region name thunk)` | Profile `thunk` under `name`; returns its result. |
| `(prof/enabled?)` | True when profiling is available in this build. |

## Arguments

**`mode`** — `'trace` (default) or `'sample`.

**`hz`** — Sampling frequency for sample mode (default `1000`).

**`format`** — One of `'pretty` (default), `'json`, `'speedscope`, `'chrome`, `'eta-prof`.

`prof/counter` increments named counters included in `pretty` and `json` reports.

## Example

```scheme
(import std.prof)

(define (fib n)
  (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

(call-with-values
  (lambda ()
    (prof/with
      (lambda ()
        (prof/counter "fib-runs" 1)
        (fib 30))
      'trace))
  (lambda (value handle)
    (display value) (newline)
    (display (prof/report handle 'pretty)) (newline)))
```

---

**Source:** [`stdlib/std/prof.eta`](../../../stdlib/std/prof.eta)

