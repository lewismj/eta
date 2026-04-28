# Strings

[← Back to Language Guide](./language_guide.md)

Strings in Eta are immutable, indexed in characters (not bytes), and
stored in UTF-8 internally. Symbols are interned; convert between the
two with `string->symbol` / `symbol->string`.

---

## Literals and escapes

```scheme
"hello"                      ; ASCII
"héllo"                      ; UTF-8
"line1\nline2"               ; \n, \t, \r, \\, \", \0
"\u03BB-calculus"            ; \uXXXX hex escape
```

---

## Builtins

| Operation                | Result                                       |
| :----------------------- | :------------------------------------------- |
| `(string-length s)`      | Number of characters                         |
| `(string-ref s i)`       | Character at index `i`                       |
| `(substring s a b)`      | Characters in `[a, b)`                       |
| `(string-append a b …)`  | Concatenation                                |
| `(string->list s)`       | List of characters                           |
| `(list->string chars)`   | Build a string from chars                    |
| `(string->number s)`     | Parse number, or `#f`                        |
| `(number->string n)`     | Decimal representation                       |
| `(string=? a b)`         | Character-wise equality                      |
| `(string<? a b)`         | Lexicographic order (also `>?`, `<=?`, `>=?`)|
| `(string-upcase s)`      | Uppercase                                    |
| `(string-downcase s)`    | Lowercase                                    |

```scheme
(string-append "foo" "-" (number->string 42))    ; => "foo-42"
(map char-upcase (string->list "abc"))           ; => (#\A #\B #\C)
```

---

## Symbols

```scheme
(string->symbol "hello")     ; => hello
(symbol->string 'hello)      ; => "hello"
(symbol? 'x)                 ; => #t
```

Symbols are compared with `eq?`; this is the typical key type for hash
maps and dispatch.

---

## Formatting

`std.io` provides `display->string` for converting any value to its
human-readable representation:

```scheme
(import std.io)
(display->string '(1 2 3))   ; => "(1 2 3)"
(display->string 3.14159)    ; => "3.14159"
```

Use `with-output-to-port` plus `open-output-string` /
`get-output-string` for richer multi-step formatting — see
[I/O](./io.md).

---

## Regular expressions

`std.regex` provides PCRE-style regex backed by a native engine:

```scheme
(import std.regex)
(define re (regex:compile "(\\w+)@(\\w+\\.\\w+)"))
(regex:match? re "alice@example.com")            ; => #t
(regex:find-all re "a@b.com x@y.org")
;; => (("a@b.com" "a" "b.com") ("x@y.org" "x" "y.org"))

(regex:replace re "<user@host>" "alice@example.com")
;; => "<user@host>"

(regex:split (regex:compile "\\s+") "  one  two\tthree")
;; => ("one" "two" "three")
```

Match objects expose `regex-match-text`, `regex-match-start`,
`regex-match-end`, `regex-match-group`, `regex-match-named`,
`regex-match-span`, plus `regex:match-groups-hash` for named-group
capture as a hash map.

Full reference: [`regex.md`](./reference/regex.md).

---

## CSV

[`std.csv`](./reference/csv.md) parses and writes RFC 4180 CSV with column
typing, header handling, and streaming readers / writers.

---

## Related

- [I/O & Ports](./io.md)
- [Collections](./collections.md)
- [`regex.md`](./reference/regex.md), [`csv.md`](./reference/csv.md)

