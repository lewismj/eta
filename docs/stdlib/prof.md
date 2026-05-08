# std.prof

[<- Back to stdlib index](../stdlib.md)

Runtime profiling helpers for Eta programs.

`std.prof` wraps runtime profiling primitives for both trace and sample sessions.

## Import

```scheme
(import std.prof)
```

## Exports

- `(prof/start [mode] [hz]) -> session`
- `(prof/stop session) -> handle|#f`
- `(prof/report handle [format]) -> string`
- `(prof/counter name n) -> #t`
- `(prof/with thunk [mode] [hz]) -> (values result handle)`
- `(prof/region name thunk) -> result`
- `(prof/enabled?) -> bool`

## Arguments

`mode`:

- `trace` (default)
- `sample`

`mode` can be passed as a symbol or string.

`hz`:

- sampling frequency for sample mode
- default is `1000`

`format`:

- `pretty`
- `json`
- `speedscope`
- `chrome`
- `eta-prof`

`prof/counter` increments named counters that are included in `pretty` and `json` reports.

`pretty` and `json` `flat` report rows include `bytes_allocated` as a coarse
per-frame allocation metric.

## Example

```scheme
(module demo
  (import std.prof)
  (begin
    (define (fib n)
      (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))

    (call-with-values
      (lambda ()
        (prof/with
          (lambda ()
            (prof/counter "fib-runs" 1)
            (fib 10))
          'trace
          1000))
      (lambda (value handle)
        (display value) (newline)
        (display (prof/report handle "json"))))))
```
