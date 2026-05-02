# Subprocesses

[Back to README](../../../README.md) ·
[Modules and Stdlib](modules.md) ·
[OS Primitives](os.md) ·
[Filesystem](fs.md) ·
[Next Steps](../../next-steps.md)

---

## Quick Start

```scheme
(module demo
  (import std.process std.io)
  (begin
    (let ((result (process:run "git" '("--version"))))
      (println (car result))              ;; exit code
      (println (car (cdr result)))        ;; stdout
      (println (car (cdr (cdr result))))))) ;; stderr
```

`std.process` provides blocking and non-blocking subprocess execution.
Arguments are passed as a list of strings with no shell interpolation.

---

## API

### Blocking

| Function | Signature | Notes |
|---|---|---|
| `process:run` | `(program args [opts]) -> (status stdout stderr)` | Default stdout/stderr mode is capture. |
| `process:run-string` | `(program args [opts]) -> string/bytevector/#f` | Convenience accessor for `stdout` from `process:run`. |

`opts` for `process:run`:

- `cwd`: string
- `env`: alist of `(name . value)` pairs
- `replace-env?`: bool (`#f` default)
- `stdin`: string, bytevector, `'inherit`, or `'null` (`'null` default)
- `stdout`: `'capture` (default), `'inherit`, `'null`
- `stderr`: `'capture` (default), `'inherit`, `'null`, `'merge`
- `timeout-ms`: non-negative integer
- `binary?`: bool (`#f` default, `#t` returns bytevectors for captured streams)

---

### Non-Blocking

| Function | Signature | Notes |
|---|---|---|
| `process:spawn` | `(program args [opts]) -> handle` | Default stdin/stdout/stderr mode is pipe. |
| `process:wait` | `(handle [timeout-ms]) -> exit-code or #f` | Returns `#f` on timeout. |
| `process:kill` | `(handle) -> bool` | Hard kill (`SIGKILL`/`TerminateProcess`). |
| `process:terminate` | `(handle) -> bool` | Graceful terminate (`SIGTERM`/terminate). |
| `process:pid` | `(handle) -> int` | Native process id. |
| `process:alive?` | `(handle) -> bool` | Non-blocking liveness probe. |
| `process:exit-code` | `(handle) -> int or #f` | `#f` if still running. |
| `process:handle?` | `(x) -> bool` | Process-handle predicate. |
| `process:stdin-port` | `(handle) -> port or #f` | Writable child stdin pipe when configured. |
| `process:stdout-port` | `(handle) -> port or #f` | Readable child stdout pipe when configured. |
| `process:stderr-port` | `(handle) -> port or #f` | Readable child stderr pipe when configured. |

---

## Patterns

### Capture stdout and stderr

```scheme
(let ((r (process:run "git" '("status" "--short"))))
  (let ((status (car r))
        (stdout (car (cdr r)))
        (stderr (car (cdr (cdr r)))))
    (list status stdout stderr)))
```

### Provide stdin text

```scheme
(process:run "/bin/cat" '() '((stdin . "hello from eta")))
```

### Timeout guard

```scheme
(catch 'runtime.error
  (process:run "/bin/sh" '("-c" "sleep 10")
               '((timeout-ms . 250))))
```

### Pipe two spawned processes

```scheme
(let* ((a (process:spawn "/bin/sh" '("-c" "printf 'a\nb\n'")))
       (b (process:spawn "/bin/grep" '("b"))))
  ;; transfer bytes from a stdout -> b stdin
  ...)
```

---

## Errors

Operational failures raise runtime errors with stable prefixes:

- `process-spawn-failed:`
- `process-not-found:`
- `process-timeout:`
- `process-wait-failed:`

---

## Notes

- Process handles are GC-tracked heap objects and keep stdio ports alive.
- Captured text output uses UTF-8 decoding with replacement for malformed input.
- `std.process` is opt-in and is not re-exported by `std.prelude`.
