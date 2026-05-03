# std.test

Unit-testing framework with assertions and TAP/JUnit reporters.

```scheme
(import std.test)
```

## Test construction

| Symbol | Description |
| --- | --- |
| `(make-test name thunk)` | Build a single test from a thunk. |
| `(make-group name tests)` | Group tests into a labelled suite. |

## Running

| Symbol | Description |
| --- | --- |
| `(run tests-or-group)` | Execute and return a summary record. |
| `(summary x)` | Build a summary from raw results. |
| `(summary-total s)` | Total test count. |
| `(summary-passed s)` | Passed count. |
| `(summary-failed s)` | Failed count. |

## Assertions

| Symbol | Description |
| --- | --- |
| `(assert-true x)` | Pass when `x` is truthy. |
| `(assert-false x)` | Pass when `x` is `#f`. |
| `(assert-equal expected actual)` | Pass when `equal?`. |
| `(assert-not-equal a b)` | Pass when not `equal?`. |
| `(assert-approx-equal expected actual tolerance)` | Numeric near-equality. |

## Reporters

| Symbol | Description |
| --- | --- |
| `(print-summary s)` | Human-readable summary to stdout. |
| `(print-tap s)` | TAP report. |
| `(print-junit s path)` | Write a JUnit XML report to `path`. |

