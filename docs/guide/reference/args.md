# Command-Line Arguments (`std.args`)

[← Back to Language Guide](../language_guide.md) ·
[I/O](../io.md) ·
[`os.md`](./os.md) ·
[`hashmap.md`](./hashmap.md)

---

## Overview

`std.args` is a small, dependency-free command-line parser inspired by
Python's `argparse`. It takes a declarative **spec** plus an `argv`
list and returns an immutable hash map keyed by symbol — including a
distinguished `'positional` key for non-option arguments.

It is intentionally narrow in scope:

- Tuple-style spec only — no chained builder API.
- Long (`--name`) and short (`-n`) flags; `=` and space separators.
- Five value kinds: `flag`, `string`, `int`, `float`, `list`.
- Default actions (`set`, `append`, `count`) cover the common cases.
- Optional `parse` / `validate` lambdas and `choices` for everything else.
- `--` terminator forwards the remainder to `'positional`.
- Errors signal via `(error ...)` — catch with the usual
  `(catch 'runtime.error …)` idiom.

What it deliberately omits: sub-commands, mutually-exclusive groups,
auto-generated short-flag negation, environment-variable fallback,
configuration files. Compose those with `std.os`, `std.json`, and
`std.hashmap` when needed.

---

## Quick start

```scheme
(import std.args std.io)

(define spec
  '((verbose (--verbose -v) flag)
    (out     (--out -o)     string "a.out")
    (jobs    (--jobs -j)    int    1)
    (tag     (--tag)        list)))

(define r (args:parse spec
            '("-v" "--out=demo" "-j" "4"
              "--tag" "risk" "--tag" "stress"
              "--" "input.csv")))

(args:get r 'verbose)     ;; => #t
(args:get r 'out)         ;; => "demo"
(args:get r 'jobs)        ;; => 4
(args:get r 'tag)         ;; => ("risk" "stress")
(args:get r 'positional)  ;; => ("input.csv")
```

For the runnable demo see
[`examples/args.eta`](../../../examples/args.eta); for tested behaviour
see [`stdlib/tests/args.test.eta`](../../../stdlib/tests/args.test.eta).

---

## API

| Function                                       | Purpose                                                            |
| :--------------------------------------------- | :----------------------------------------------------------------- |
| `(args:parse spec argv) -> hash-map`           | Parse an explicit argv list                                        |
| `(args:parse-command-line spec) -> hash-map`   | Parse `(os:command-line-arguments)`                                |
| `(args:get parsed key [default]) -> value`     | Alias for `hash-map-ref` — symbol-keyed lookup                     |
| `(args:help spec [program-name]) -> string`    | Render a usage / help string                                       |

The parsed hash map always contains:

- one entry per declared spec key (default applied when absent);
- `'positional` → list of non-option arguments, in source order
  (post-`--` tokens included).

---

## Spec syntax

Each entry is a list:

```text
(key (flag …) type)
(key (flag …) type default)
(key (flag …) type opts)
(key (flag …) type default opts)
```

Where:

- **`key`** — symbol used to look the value up via `args:get`.
- **`(flag …)`** — one or more long/short flags. Symbols (`'--out`, `'-o`)
  or strings; both are accepted, both must be unique across the spec.
- **`type`** — one of `flag`, `string`, `int`, `float`, `list`, or
  `(list <scalar>)` for an explicitly-typed list (defaults to
  `(list string)`).
- **`default`** — any value. If omitted, a per-kind default is used
  (`#f` for `flag`, `'()` for `list`, `0` for `count`-action flags,
  `'()` for missing scalars).
- **`opts`** — alist of any of the keys below.

### Optional `opts` keys

| Key          | Meaning                                                                                     |
| :----------- | :------------------------------------------------------------------------------------------ |
| `required?`  | `#t` to error if the flag is absent.                                                        |
| `help`       | One-line description shown by `args:help`.                                                  |
| `metavar`    | Placeholder name (e.g. `"FILE"`) — currently informational.                                 |
| `choices`    | List of allowed values; rejected values raise.                                              |
| `parse`      | `(lambda (raw-string) value)` — overrides the built-in coercion.                            |
| `validate`   | `(lambda (value) bool-or-raise)` — runs after coercion / `parse`.                           |
| `action`     | `set` (default for scalars), `append` (default for `list`), or `count` (flag-only counter). |

### Worked spec

```scheme
(define parse-level
  (lambda (raw)
    (let ((n (string->number raw)))
      (if (and (number? n) (integer? n))
          n
          (error "level must be an integer")))))

(define level-in-range?
  (lambda (n) (and (>= n 0) (<= n 5))))

(define spec
  (list
    '(v (--verbose -v) flag)
    '(o (--out -o)     string "report.txt")
    '(n (--num)        int    0)
    '(t (--tag)        list)
    (list 'level
          (list '--level)
          'int
          1
          (list (cons 'parse    parse-level)
                (cons 'validate level-in-range?)))))
```

---

## Tokenisation rules

| Token form              | Meaning                                                                |
| :---------------------- | :--------------------------------------------------------------------- |
| `--name value`          | Option `--name` consumes the next token as its value.                  |
| `--name=value`          | Inline value; `value` may be empty.                                    |
| `-x value` / `-x=value` | Same, for short flags.                                                 |
| `--name`                | Bare option. For `flag` types ⇒ `#t` (or `+1` if `action: count`).      |
| `--`                    | Terminator. Every later token is positional, even if it starts with `-`. |
| anything else           | Positional argument.                                                   |

### Negative numbers

Tokens that *look* like options (`-3`, `-1.5`) are accepted as values
when the target spec has type `int` or `float`, so this works:

```scheme
(args:parse '((n (--num -n) int)) '("--num" "-3"))
;; => hash-map with (n . -3)
```

Bundled short flags (`-vj4`) are **not** supported.

### Missing values are rejected

```scheme
(catch 'runtime.error
  (args:parse '((n (--num) int) (m (--msg) string))
              '("--num" "--msg" "ok")))
;; => (runtime-error "args: missing value for option" …)
```

---

## Defaults & access patterns

`args:get` is `hash-map-ref`, so the optional default applies only
when the key was *removed* from the map — declared spec keys always
have a value (their `default`):

```scheme
(args:get r 'unknown 'fallback)   ;; => 'fallback
(args:get r 'verbose)             ;; => #f when --verbose absent
```

Iterate over everything by treating the result as a normal hash map:

```scheme
(import std.hashmap)
(for-each
  (lambda (k) (println (list k '=> (args:get r k))))
  (hash-map-keys r))
```

---

## Help text

`(args:help spec ["my-tool"])` returns a usage string that lists every
declared option with its flags, type, default, requirement marker, and
`help` text:

```text
usage: my-tool [options] [--] [positional ...]
  --verbose, -v  : flag        (default #f)  - chatty output
  --out, -o      : string      (default "report.txt")
  --num          : int         (default 0)
  --tag          : list<string> (default ())
  --level        : int         (default 1)
```

A typical CLI entry point:

```scheme
(defun main ()
  (let ((r (args:parse-command-line spec)))
    (when (args:get r 'help)
      (display (args:help spec "my-tool"))
      (os:exit 0))
    ;; …main logic…
    ))
```

---

## Composing with other stdlib modules

- **`std.os`** — `args:parse-command-line` is just
  `(args:parse spec (os:command-line-arguments))`.
- **`std.hashmap`** — the result is a regular hash map; merge with a
  config-file map via `hash-map-merge`:
  ```scheme
  (define cfg (json:read-string (call-with-input-file "cfg.json"
                                  (lambda (p) (read-string p)))))
  (define final (hash-map-merge cfg parsed-args))   ;; CLI wins
  ```
- **`std.json`** — load defaults from disk; `args:get` and
  `hash-map-ref` interoperate.
- **`std.log`** — wire `--verbose` / `--log-level` straight into
  `log:set-global-level!`.

---

## Error handling

Every parse error raises a runtime error with a tagged message. To
turn errors into a friendly exit:

```scheme
(let ((r (catch 'runtime.error
           (args:parse-command-line spec))))
  (if (and (pair? r) (eq? (car r) 'runtime-error))
      (begin (eprintln (cadr r))
             (display (args:help spec "my-tool"))
             (os:exit 2))
      (run r)))
```

---

## Reference summary

```text
args:parse                spec argv                 -> hash-map
args:parse-command-line   spec                      -> hash-map
args:get                  hash-map key [default]    -> value
args:help                 spec [name]               -> string
```

Spec entry shapes (any of):

```text
(key (flag …) type)
(key (flag …) type default)
(key (flag …) type opts)
(key (flag …) type default opts)
```

`type ∈ { flag | string | int | float | list | (list scalar) }`
`opts keys ⊆ { required? help metavar choices parse validate action }`
`action ∈ { set | append | count }`

