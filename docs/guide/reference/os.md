# Operating System

[Back to README](../../../README.md) ·
[Modules and Stdlib](modules.md) ·
[Language Guide](../language_guide.md) ·
[Subprocesses](process.md) ·
[Filesystem](fs.md) ·
[Next Steps](../../next-steps.md)

---

## Quick Start

```scheme
(module demo
  (import std.os std.io)
  (begin
    ;; Inspect the script's invocation
    (println (os:command-line-arguments))      ;; e.g. ("--verbose" "in.csv")

    ;; Environment variables
    (println (or (os:getenv "ETA_HOME") "(unset)"))
    (os:setenv! "ETA_DEMO_RUN" "1")

    ;; Working directory
    (println (os:current-directory))

    ;; Clean exit with a status code
    (when (equal? (os:getenv "DEMO_FAIL") "1")
      (os:exit 2))))
```

`std.os` is a thin Eta layer over the native process / environment
builtins (`getenv`, `setenv!`, `current-directory`, …). It is
auto-imported by `std.prelude`; you can also import it directly:

```scheme
(import std.os)
```

---

## API

### Environment Variables

| Function                    | Signature                        | Notes |
|-----------------------------|----------------------------------|-------|
| `os:getenv`                 | `(name) -> string \| #f`         | Returns `#f` for unset variables. |
| `os:setenv!`                | `(name value) -> '()`            | Mutates the live process environment. |
| `os:unsetenv!`              | `(name) -> '()`                  | Idempotent — succeeds even if the variable is already unset. |
| `os:environment-variables`  | `() -> alist`                    | Sorted alist of `("KEY" . "value")` pairs. |

### Process

| Function                       | Signature                | Notes |
|--------------------------------|--------------------------|-------|
| `os:command-line-arguments`    | `() -> list`             | List of strings passed to `etai` / `etac` after the script path. |
| `os:exit`                      | `([code]) -> never`      | Terminates the host process. `code` defaults to `0`; non-integer codes are coerced where unambiguous. |

### Working Directory

| Function                  | Signature           | Notes |
|---------------------------|---------------------|-------|
| `os:current-directory`    | `() -> string`      | Absolute path, with the platform-preferred separator. |
| `os:change-directory!`    | `(path) -> '()`     | Equivalent of `chdir`. Errors if `path` is not a directory. |

---

## Patterns

### Config from environment with a fallback

```scheme
(import std.os)
(defun config-or (name default)
  (or (os:getenv name) default))

(define log-level (config-or "ETA_LOG_LEVEL" "info"))
```

### CLI flag parsing

```scheme
(import std.os std.collections)
(defun has-flag? (flag)
  (any? (lambda (a) (equal? a flag))
        (os:command-line-arguments)))

(when (has-flag? "--verbose")
  (println "verbose mode on"))
```

### Scoped working directory

```scheme
(import std.os)
(defun with-cwd (dir thunk)
  (let ((prev (os:current-directory)))
    (dynamic-wind
      (lambda () (os:change-directory! dir))
      thunk
      (lambda () (os:change-directory! prev)))))
```

### Exit codes from a script

```scheme
(import std.os)
(catch (lambda () (run-pipeline!))
       (lambda (tag payload)
         (eprintln "fatal: " tag " " payload)
         (os:exit 1)))
```

---

## Notes

- `os:setenv!` / `os:unsetenv!` mutate global process state; they are
  not undone by the trail and are visible to subprocesses spawned
  after the call.
- `os:exit` does **not** unwind through `dynamic-wind` *after* thunks
  in all hosted environments (Jupyter kernels in particular swallow
  the exit). Prefer raising and letting the top-level handler convert
  to a status code.
- Filesystem operations — paths, directories, file metadata — live in
  [`std.fs`](fs.md).
- Subprocess execution and child stdio piping live in
  [`std.process`](process.md).
- Wall-clock and monotonic time live in [`std.time`](time.md).

