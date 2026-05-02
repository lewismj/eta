# I/O & Ports

[← Back to Language Guide](../language_guide.md)

Eta's I/O follows the R7RS port model. The runtime provides the core
port operations as builtins; `std.io` adds convenience functions and
scoped redirection helpers.

---

## Builtins

| Operation                          | Purpose                              |
| :--------------------------------- | :----------------------------------- |
| `display`, `write`, `newline`      | Write a value (`display` is unquoted, `write` is `read`-able) |
| `write-string`, `write-char`       | Direct character / string output     |
| `read-char`                        | Read one character                   |
| `current-input-port`               | The active input port                |
| `current-output-port`              | The active output port               |
| `current-error-port`               | The active error port                |
| `set-current-{input,output,error}-port!` | Replace the active port        |
| `open-input-file path`             | Open a file for reading              |
| `open-output-file path`            | Open a file for writing              |
| `open-input-string s`              | Read from an in-memory string        |
| `open-output-string`               | Accumulate output into a string      |
| `get-output-string p`              | Snapshot of the string-port contents |
| `close-port`, `close-input-port`, `close-output-port` | Close a port      |
| `port?`, `input-port?`, `output-port?` | Predicates                       |

---

## `std.io` helpers

```scheme
(import std.io)

(println "hi")                       ; (display x) (newline)
(eprintln "warn: …")                 ; same, but to current-error-port
(read-line)                          ; reads up to newline; #!eof at EOF

(display->string '(1 2 3))           ; => "(1 2 3)"
```

### Scoped redirection

```scheme
(with-output-to-port (open-output-file "out.txt")
  (lambda ()
    (println "captured to file")))

(define out (open-output-string))
(with-output-to-port out
  (lambda ()
    (println 1) (println 2)))
(get-output-string out)              ; => "1\n2\n"
```

Companions: `with-input-from-port`, `with-error-to-port`, `with-port`.

---

## File patterns

```scheme
(define (read-all path)
  (let ((p (open-input-file path)))
    (let loop ((acc '()))
      (let ((line (read-line p)))
        (cond
          ((eof-object? line) (begin (close-input-port p) (reverse acc)))
          (else               (loop (cons line acc))))))))
```

> [!TIP]
> Wrap file operations in `dynamic-wind` (or a custom `with-port`) so
> the handle closes even on exceptions:
>
> ```scheme
> (define (with-input-file path proc)
>   (let ((p (open-input-file path)))
>     (dynamic-wind
>       (lambda () #f)
>       (lambda () (proc p))
>       (lambda () (close-input-port p)))))
> ```

---

## Structured input

| Format | Module                      |
| :----- | :-------------------------- |
| CSV    | [`std.csv`](./reference/csv.md)      |
| Datalog facts | [`std.db`](./reference/db.md) |
| Regex on text streams | [`std.regex`](./reference/regex.md) |

```scheme
(import std.csv)
(define rows (csv:load-file "trades.csv" '(:header #t)))
```

---

## Related

- [Strings](./strings.md)
- [Collections](./collections.md)
- [`csv.md`](./reference/csv.md), [`db.md`](./reference/db.md)


