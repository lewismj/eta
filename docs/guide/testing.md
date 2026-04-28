# Testing

[← Back to Language Guide](./language_guide.md)

Eta ships with a small testing library, `std.test`, and a dedicated
runner binary, `eta_test`. Tests are ordinary Eta values: a *test case*
is a name + thunk; a *group* is a name + a list of children.

> **Reference:** [`std_lib_tests.md`](../std_lib_tests.md) for the
> standard-library test layout.

---

## Quick example

```scheme
(module my-tests
  (import std.test std.io)
  (begin
    (define suite
      (make-group "arithmetic"
        (list
          (make-test "addition"
            (lambda () (assert-equal 4 (+ 2 2))))
          (make-test "near-equal"
            (lambda () (assert-approx-equal 0.1 (- 0.3 0.2) 1e-9))))))

    (define results (run suite))
    (print-summary (summary results))))
```

```bash
etai my-tests.eta
```

---

## API surface

### Constructors

| Form                                | Builds        |
| :---------------------------------- | :------------ |
| `(make-test name thunk)`            | A leaf test   |
| `(make-group name children)`        | A group node  |

Groups may contain tests and other groups, building a tree.

### Assertions

| Assertion                                     | Passes if                          |
| :-------------------------------------------- | :--------------------------------- |
| `(assert-true x)`                             | `x` is truthy                      |
| `(assert-false x)`                            | `x` is `#f`                        |
| `(assert-equal expected actual)`              | `equal?`                           |
| `(assert-not-equal expected actual)`          | `not equal?`                       |
| `(assert-approx-equal expected actual tol)`   | Numeric within `tol`               |

A failed assertion raises a tagged exception that the runner records
without aborting the suite.

### Runner

| Function                  | Returns                                   |
| :------------------------ | :---------------------------------------- |
| `(run group-or-test)`     | A result tree                             |
| `(summary results)`       | Aggregate `test-summary` record           |
| `summary-total`, `summary-passed`, `summary-failed` | Counts          |

### Output

| Function                | Format                                    |
| :---------------------- | :---------------------------------------- |
| `(print-summary s)`     | Human-readable summary                    |
| `(print-tap results)`   | TAP 13 to stdout                          |
| `(print-junit results)` | JUnit XML to stdout                       |

---

## The `eta_test` runner

`eta_test` is the dedicated test driver. It discovers test files,
loads them, and produces machine-readable output for IDE integration.

```bash
eta_test path/to/tests           # run a directory tree
eta_test --tap   …               # TAP output
eta_test --junit out.xml …       # JUnit XML to file
eta_test --filter "regex"  …     # name filter
```

VS Code's Test Explorer picks up `eta_test --json …` output through the
extension — see [`vscode.md`](./reference/vscode.md).

---

## CI integration

```yaml
# GitHub Actions excerpt
- name: Run Eta tests
  run: eta_test --junit test-results.xml stdlib/tests
- uses: actions/upload-artifact@v4
  with:
    name: test-results
    path: test-results.xml
```

---

## Related

- [`std_lib_tests.md`](../std_lib_tests.md)
- [Debugging](./debugging.md)
- [`vscode.md`](./reference/vscode.md)

