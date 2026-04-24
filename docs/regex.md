# Regex

[Back to README](../README.md) · [Modules and Stdlib](modules.md) · [Next Steps](next-steps.md)

---

## Quick Start

```scheme
(module demo
  (import std.regex std.io)
  (begin
    (println (regex:match? "^foo\\d+$" "foo42"))
    (println (regex:replace "(\\d+)" "item-42" "<$1>"))
    (println (regex:find-all "(\\d+)" "a1 b22 c333"))))
```

`std.regex` is backed by C++ `std::regex` with ECMAScript syntax.

---

## API

### Compile and Introspection

| Function | Signature | Notes |
|---|---|---|
| `regex:compile` | `(pattern . flags) -> regex` | Compile once and reuse. |
| `regex?` | `(x) -> bool` | Regex object predicate. |
| `regex:pattern` | `(regex) -> string` | Original source pattern. |
| `regex:flags` | `(regex) -> list` | Symbol flags (`ecmascript`, `icase`, ...). |

### Query and Transform

| Function | Signature | Notes |
|---|---|---|
| `regex:match?` | `(regex-or-pattern input) -> bool` | Full-string match. |
| `regex:search` | `(regex-or-pattern input [start]) -> match-or-#f` | First match from `start`. |
| `regex:find-all` | `(regex-or-pattern input) -> list` | All match payloads. |
| `regex:replace` | `(regex-or-pattern input replacement) -> string` | Supports `$&`, `$1`, `$<name>`, `$$`. |
| `regex:replace-fn` | `(regex-or-pattern input fn) -> string` | `fn` receives a match payload and returns replacement text. |
| `regex:split` | `(regex-or-pattern input) -> vector` | Regex delimiter split. |
| `regex:quote` | `(s) -> string` | Escape regex metacharacters in `s`. |

### Match Payload Accessors

| Function | Signature | Notes |
|---|---|---|
| `regex-match?` | `(x) -> bool` | Match payload predicate. |
| `regex-match-start` | `(m) -> int` | Start offset (inclusive). |
| `regex-match-end` | `(m) -> int` | End offset (exclusive). |
| `regex-match-span` | `(m) -> (start . end)` | Whole-match span. |
| `regex-match-text` | `(m) -> string` | Whole-match substring. |
| `regex-match-group-span` | `(m i) -> (start . end) or #f` | Group span by index. |
| `regex-match-group` | `(m i) -> string or #f` | Group text by index. |
| `regex-match-named` | `(m sym) -> string or #f` | Named group text. |

---

## Syntax Notes (ECMAScript)

Common constructs:

- Anchors: `^`, `$`
- Groups: `( ... )`, `(?<name> ... )`
- Alternation: `a|b`
- Character classes: `[abc]`, `[^abc]`, `[a-z]`
- Quantifiers: `*`, `+`, `?`, `{m}`, `{m,n}`
- Lazy quantifiers: `*?`, `+?`, `??`, `{m,n}?`
- Backreferences: `\\1`, `\\2`, ...
- Escapes: `\\d`, `\\w`, `\\s`, `\\uXXXX`

Caveats vs PCRE-style engines:

- No atomic groups.
- Lookbehind support depends on the C++ standard library implementation.
- Unicode property classes like `\\p{L}` are not portable in this backend.

---

## Performance and Safety

- String-pattern calls (`regex:match?` with a pattern string) use an internal LRU cache of compiled patterns.
- For hot loops, prefer explicit `regex:compile` and pass the compiled regex handle.
- `optimize` can improve repeated-match throughput at compile-time cost.
- Catastrophic backtracking is possible with patterns like `(a+)+$` on input such as `"aaaaaaaaaaaaaaaaX"`.
  - If patterns come from untrusted input, use `regex:quote` when constructing literal fragments.

