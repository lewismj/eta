# Filesystem

[Back to README](../../../README.md) ·
[Modules and Stdlib](modules.md) ·
[Language Guide](../language_guide.md) ·
[OS Primitives](os.md) ·
[Subprocesses](process.md) ·
[Next Steps](../../next-steps.md)

---

## Quick Start

```scheme
(module demo
  (import std.fs std.io)
  (begin
    ;; List the source tree
    (when (fs:directory? "examples")
      (for-each println (fs:list-directory "examples")))

    ;; Build a platform-correct path under the system temp dir
    (let ((p (fs:path-join (fs:temp-directory) "eta" "scratch.txt")))
      (println (fs:path-normalize p)))

    ;; Stat a file
    (when (fs:file-exists? "README.md")
      (println (fs:file-size "README.md"))
      (println (fs:file-modification-time "README.md")))))
```

`std.fs` is a thin Eta layer over the native filesystem builtins
(`file-exists?`, `path-join`, `temp-file`, …). Paths are plain strings
and use the platform-preferred separator on output (`\` on Windows,
`/` elsewhere).

It is auto-imported by `std.prelude`; importing it directly is also
fine when you want a smaller surface:

```scheme
(import std.fs)
```

---

## API

### Predicates and Stat

| Function                       | Signature                | Notes |
|--------------------------------|--------------------------|-------|
| `fs:file-exists?`              | `(path) -> bool`         | True for any existing path (file, dir, symlink). |
| `fs:directory?`                | `(path) -> bool`         | True only for directories. |
| `fs:file-size`                 | `(path) -> int`          | Size in bytes. Errors if the path is not a regular file. |
| `fs:file-modification-time`    | `(path) -> int`          | Epoch milliseconds, comparable with `time:now-ms`. |

### Mutation

| Function              | Signature             | Notes |
|-----------------------|-----------------------|-------|
| `fs:delete-file`      | `(path) -> '()`       | Errors if the path is a directory. Use a separate recursive helper for trees. |
| `fs:make-directory`   | `(path) -> '()`       | Idempotent — succeeds if the directory already exists. Does **not** create parents. |

### Enumeration

| Function              | Signature             | Notes |
|-----------------------|-----------------------|-------|
| `fs:list-directory`   | `(path) -> list`      | Sorted list of entry names (no `.` / `..`, no leading path). |

### Paths

| Function              | Signature                              | Notes |
|-----------------------|----------------------------------------|-------|
| `fs:path-join`        | `(seg1 seg2 ...) -> string`            | Variadic; joins with the platform separator. |
| `fs:path-split`       | `(path) -> list`                       | Inverse of `fs:path-join` — returns `(root component …)`. |
| `fs:path-normalize`   | `(path) -> string`                     | Lexical normalisation (`a//b/../c` → `a/c`). |

### Temp Allocation

| Function              | Signature           | Notes |
|-----------------------|---------------------|-------|
| `fs:temp-file`        | `() -> string`      | Reserves a unique temp-file path under the system temp directory. The file is created empty. |
| `fs:temp-directory`   | `() -> string`      | Reserves and creates a unique temp directory. |

Both helpers return absolute paths; cleanup is the caller's
responsibility (use `fs:delete-file` or platform tooling).

---

## Patterns

### Read-modify-write with a temp file

```scheme
(import std.fs std.io)
(defun atomic-write (path text)
  (let ((tmp (fs:temp-file)))
    (with-output-to-port (open-output-file tmp)
      (lambda () (display text)))
    ;; On POSIX a rename would replace `path` atomically; on Windows
    ;; delete-file first if the destination exists.
    (when (fs:file-exists? path) (fs:delete-file path))
    (display "wrote ") (display tmp) (newline)))
```

### Walk a directory tree

```scheme
(defun walk (root visit)
  (visit root)
  (when (fs:directory? root)
    (for-each
      (lambda (name) (walk (fs:path-join root name) visit))
      (fs:list-directory root))))
```

### Build platform-correct paths from segments

```scheme
(define cache-dir (fs:path-join (or (os:getenv "XDG_CACHE_HOME")
                                    (fs:path-join (os:getenv "HOME") ".cache"))
                                "eta"))
(fs:make-directory cache-dir)
```

---

## Notes

- All path arguments accept symbols as well as strings; symbols are
  coerced via `symbol->string`.
- Errors are raised as runtime errors (catchable with `(catch …)`).
  Operations that can legitimately "miss" (e.g. `fs:file-exists?`)
  return `#f` instead.
- For wall-clock comparisons of `fs:file-modification-time` use
  `time:now-ms` from [`std.time`](time.md).
- Process-level concerns — environment variables, working directory,
  exit codes, command-line arguments — live in
  [`std.os`](os.md).
- Subprocess execution and pipe-based process I/O live in
  [`std.process`](process.md).

