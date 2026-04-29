# String Formatting Plan — Multi-line Strings, Interpolation, `format`

[Back to next-steps](../next-steps.md) ·
[Strings guide](../guide/strings.md) ·
[Syntax & values](../guide/syntax-and-values.md) ·
[I/O](../guide/io.md)

---

## 1. Goals

Bring Eta's string story up to "modern scripting language" expectations
without breaking its Scheme heritage. Three independently useful features,
shipped as one coherent slice:

1. **Multi-line / triple-quoted string literals** with deterministic
   indentation stripping.
2. **String interpolation** via an explicit `#"..."`-prefixed reader form
   that desugars to `string-append` + `display->string` (no hidden runtime
   magic, no surprise eval inside ordinary `"..."`).
3. **A real `format` procedure** (SRFI-28 / SRFI-48 superset) plus a
   `printf`-style helper (`fprintf`, `sprintf`), exposed through a new
   `std.format` module re-exported by the prelude.

### Design principles

1. **Backward compatible.** Existing `"..."` literals keep their exact
   current semantics. New behavior is opt-in via prefix (`#"..."`) or
   delimiter (`"""..."""`).
2. **Reader-only desugaring where possible.** Interpolation is expanded in
   `eta/core/src/eta/reader/lexer.cpp` (or `parser.cpp`) into ordinary
   s-exprs so the interpreter, compiler, LSP, and DAP all see plain code.
3. **One source of truth for builtin order.** New primitives go through
   `eta/core/src/eta/runtime/builtin_names.h` and a new
   `register_format_primitives(...)` slotted into
   `eta/interpreter/src/eta/interpreter/all_primitives.h` in canonical
   order — same discipline as the logging plan.
4. **No new heap object kinds.** `format`, interpolation, and heredocs all
   produce ordinary immutable strings.

### Functional coverage (v1)

| Capability | Eta surface | Underlying mechanism |
|---|---|---|
| Triple-quoted multi-line literal | `"""..."""` | New lexer state, indentation strip rules |
| Raw string (no escapes) | `r"..."` and `r"""..."""` | Lexer flag: skip escape processing |
| Interpolated string | `#"hello ${name}, ${(+ 1 2)}"` | Reader desugar to `(string-append ...)` |
| Triple-quoted interpolated | `#"""multi-line ${x}"""` | Same desugar, multi-line aware |
| Classic `format` | `(format port fmt args ...)` | New `%format` primitive |
| String-returning `format` | `(format #f fmt args ...) ⇒ string` | Same primitive, port-less branch |
| `printf` / `fprintf` / `sprintf` | wrappers in `std.format` | Built atop `format` |
| Common directives | `~a ~s ~d ~x ~o ~b ~e ~f ~%` `~~` `~?` `~&` `~_` | Implemented in primitive |
| Width / precision | `~10a`, `~,3f`, `~10,3f`, `~-10a` | Parsed inside primitive |
| `display`-vs-`write` parity | `~a` uses `display`, `~s` uses `write` | Reuses `value_formatter.cpp` |

### Non-goals (v1)

- Full Common Lisp `format` (`~[ ~] ~{ ~} ~* ~^` conditionals, iteration,
  jump). Only the most useful directives ship in v1; the parser is
  structured so the rest can be added incrementally.
- Locale-aware number formatting (always C locale).
- User-extensible `format` directives.
- Mutable string ports beyond what `open-output-string` already provides.
- Python-style `{name!r:>10.3f}` mini-language inside `${...}`. Use
  `format` from inside the interpolation expression instead:
  `#"x = ${(format #f "~,3f" x)}"`.

---

## 2. Decisions (resolved)

1. **Interpolation requires `#"` prefix.** Plain `"..."` is never
   interpolated. This is the only way to keep existing source files —
   notably JSON-shaped strings full of `${}` in test fixtures — working.
2. **Triple-quoted literals are a separate axis from interpolation.**
   Both `"""..."""` and `#"""..."""` are valid; the `#` controls
   interpolation, the `"""` controls multi-line/indent rules.
3. **Interpolation hole syntax is `${...}`.** Single-identifier shorthand
   `$name` is also accepted (terminated by any non-`identifier-cont`).
   Literal `$` in an interpolated string is `\$` (escape), or `$$`. `\` in
   a non-raw triple-quoted string keeps standard escape semantics.
4. **Indent-strip rule for `"""..."""`** — Python `textwrap.dedent` style:
   - If the literal starts with `"""\n`, the leading newline is dropped.
   - The common leading whitespace prefix of all non-blank lines (computed
     after the leading-newline drop) is stripped.
   - Blank lines are preserved as `""` (no trailing-whitespace folding).
   - The closing `"""` may sit on its own line at the strip column without
     contributing a trailing newline.
5. **`format` follows SRFI-28 surface, SRFI-48 directives.** Tilde-based.
   This matches Scheme tradition and avoids a clash with `${...}`
   interpolation.
6. **`(format #f ...)` returns a string; `(format #t ...)` writes to the
   current output port; `(format port ...)` writes to `port`.** Standard
   SRFI-28 contract.
7. **Reader desugar shape (interpolation):**
   ```
   #"a${x}b${(f y)}c"
   ⇒ (string-append "a" (%->display-string x) "b" (%->display-string (f y)) "c")
   ```
   `%->display-string` is a tiny new primitive (alias of the existing
   "display to string" path) that accepts strings as-is (no quoting) and
   uses `display` semantics for everything else. This means
   `#"name=${"alice"}"` → `"name=alice"`, not `"name=\"alice\""`.

---

## 3. Lexer / Reader Changes

All work lives under `eta/core/src/eta/reader/`. The current string scanner
is `Lexer::consume_string()` in
`eta/core/src/eta/reader/lexer.cpp` (lines ~342–395) and is dispatched from
the `case '"':` arm of `Lexer::next_token()` (~line 78). The `#`-prefixed
reader forms go through `Lexer::consume_sharp()` (~line 481) which today
handles `#t #f #\ #( #u8( #b #o #d #x #e #i`.

### 3.1 New token kinds

In `eta/core/src/eta/reader/types.h` (where `Token::Kind` lives — confirm
during implementation):

- `Token::Kind::String` — keep as-is for plain `"..."`.
- `Token::Kind::InterpolatedString` — new. Carries a structured payload:
  `std::vector<StringPart>` where `StringPart` is either
  `Literal{std::string}` or `Expr{std::string source, Span span}`.

The expression source is *re-lexed* by the parser through a sub-`Lexer`
instance (the source span is preserved for diagnostics, including
inside-DAP step locations).

### 3.2 Lexer dispatch updates

In `Lexer::next_token()`:

- Extend `case '"'`:
  - Detect triple quote (`peek(1) == '"' && peek(2) == '"'`) and route to
    `consume_triple_string(/*interpolated=*/false, /*raw=*/false)`.
  - Otherwise route to `consume_string()` (existing behavior).
- Extend `case '#'` in `consume_sharp()`:
  - On `#"` → `consume_string(/*interpolated=*/true, /*raw=*/false)` or
    triple-string variant.
- New top-level prefix `r"` / `R"` recognised in
  `consume_bare_identifier_or_number()` only when the buffer at start is
  exactly `r` followed by `"` (so identifiers like `r2d2` keep working).
  Combined `#r"..."` and `r#"..."` are both rejected in v1 with a clear
  diagnostic ("use `#r\"\"\"...\"\"\"` for raw interpolated string"). For
  v1 we accept these in this order:
  - `"..."`             — plain
  - `"""..."""`         — plain, multi-line
  - `r"..."`            — raw, no escapes
  - `r"""..."""`        — raw multi-line
  - `#"..."`            — interpolated
  - `#"""..."""`        — interpolated multi-line
  - `#r"..."` / `#r"""..."""` — raw interpolated (still expands `${...}`,
    no `\` escapes)

### 3.3 New scanner functions in `lexer.cpp`

```cpp
std::expected<Token, LexError> consume_triple_string(bool interpolated, bool raw);
std::expected<Token, LexError> consume_string_inner(bool triple, bool interpolated, bool raw);
std::expected<std::string, LexError> consume_interpolation_expr_source(Position start);
```

Indentation strip is a post-process on the assembled vector of physical
lines (literal parts only — interpolated holes are not used for column
math).

### 3.4 Parser desugar

In `eta/core/src/eta/reader/parser.cpp`, when an
`InterpolatedString` token is consumed, build an s-expr:

```
(string-append <part0> <part1> ... <partN>)
```

where each part is either:
- a literal `Datum::String("...")`, or
- `(%->display-string <reparsed-expr>)`.

`<reparsed-expr>` is produced by feeding the captured expression source
back through a fresh `Lexer + Parser` instance with the original `FileId`
and an offset baked into spans so error reporting points back at the
original source location (think: the same trick used by macro expanders
for hygiene-preserving spans).

If a part is the *first or only* part and is a literal string, no wrapping
in `string-append` is performed (small optimization, mirrors what the
expander already does for trivial concatenations elsewhere).

### 3.5 Diagnostics

Add to `LexErrorKind` (in `eta/core/src/eta/reader/types.h`):

- `UnterminatedTripleString`
- `UnterminatedInterpolation`
- `EmptyInterpolation`           — for `#"...${}..."`
- `InvalidInterpolationStart`    — `$` not followed by `{` or identifier
- `InvalidRawStringEscape`       — `\"` etc. attempted inside raw

Update `eta/core/src/eta/reader/error_format.h` to render each.

### 3.6 Tests for the reader

`eta/core/test/` (or wherever `lexer_tests.cpp` lives — find with
`file_search`): add cases for every literal form, including:

- Triple-string indent stripping (Python-compatible fixtures).
- `\` escapes still work inside `"""..."""` (non-raw).
- Raw string preserves `\n` literally.
- Interpolation: `${ident}`, `${(expr)}`, nested parens, nested strings,
  unterminated `${`.
- Span correctness: error inside `${(/ 1 0)}` points at `1 0` source
  positions inside the interpolated expression.

---

## 4. Runtime: `format` Primitive

### 4.1 Where it lives

New files:

```
eta/core/src/eta/runtime/format_primitives.h     ; register_format_primitives(...)
eta/core/src/eta/runtime/format_engine.h         ; pure formatter
eta/core/src/eta/runtime/format_engine.cpp
```

`format_engine` is a self-contained tilde-directive parser/emitter that
takes a `std::string_view fmt`, a `std::span<const LispVal> args`, and a
`Port&` sink. It reuses `value_formatter.cpp` for `~a` (display) and
`~s` (write) so format output is identical to existing `display`/`write`.

### 4.2 Builtin surface

Added to `eta/core/src/eta/runtime/builtin_names.h` in a new "Formatting"
block immediately after the existing string operations (so the canonical
slot ordering stays contiguous and easy to audit):

```
;; Formatting
r("format",              0, true);   ; (format dest fmt . args)
r("%->display-string",   1, false);  ; reader-emitted helper for #"..."
r("%format-to-string",   0, true);   ; internal: (sprintf fmt . args)
```

`%->display-string` semantics:
- string → returned unchanged (no quoting),
- char   → 1-char string,
- other  → same as `(display->string x)`.

### 4.3 Directive set (v1)

| Directive | Meaning | Notes |
|---|---|---|
| `~a` | Display arg | width prefix supported: `~10a`, `~-10a` left-align |
| `~s` | Write arg (quoted strings, escaped chars) | width prefix supported |
| `~d` | Decimal integer | `~10d`, `~,'0d` zero-pad via SRFI-48 `~mincol,padcharD` |
| `~x` `~o` `~b` | Hex / octal / binary integer | width prefix supported |
| `~e` | Exponential float | `~,3e` |
| `~f` | Fixed float | `~,3f`, `~10,3f` |
| `~g` | General float | |
| `~c` | Character | |
| `~%` | Newline | |
| `~&` | Fresh line (only emit `\n` if not already at column 0) | |
| `~~` | Literal `~` | |
| `~_` | Single space | |
| `~t` | Tab | |
| `~?` | Recursive format (`~?` consumes `(fmt args-list)`) | SRFI-48 |
| `~r` | Number as english/roman (deferred to v2) | reserved |

Width/precision grammar: `~[+-]?[0-9]*(,[0-9]*)?(,'.)?<directive>`

### 4.4 `format` dispatch contract

```
(format #f fmt . args)    ⇒ string
(format #t fmt . args)    ⇒ '() ; writes to (current-output-port)
(format port fmt . args)  ⇒ '() ; writes to port (must be output-port?)
```

Errors:
- Unknown directive → `RuntimeErrorCode::ArgumentError`
  ("unknown format directive ~?").
- Arg-count mismatch → same code, message includes index of the offending
  directive and the format string with a caret.

### 4.5 Registration order

Update `eta/interpreter/src/eta/interpreter/all_primitives.h`:

```cpp
runtime::register_core_primitives(env, heap, intern, &vm);
runtime::register_format_primitives(env, heap, intern, &vm);   // NEW — slot 2
runtime::register_port_primitives(env, heap, intern, vm);
runtime::register_io_primitives(env, heap, intern, vm);
// ...existing order...
```

Mirror that exact slot in `builtin_names.h` (Formatting block lives
between Core and Port). The doc comment at the top of `all_primitives.h`
listing canonical order is updated to call out `format_primitives.h`
explicitly, matching the logging plan's discipline (Section 2 / 5.2 of
`docs/plan/logging_plan.md`).

### 4.6 No new `ObjectKind`

`format`/`sprintf` allocate ordinary `String` values. No `heap.h`,
`heap_visit.h`, or `value_formatter.cpp` changes required.

---

## 5. `std.format` Stdlib Module

New file `stdlib/std/format.eta`:

```scheme
(module std.format
  (export
    format
    sprintf
    printf
    fprintf
    println-fmt
    ;; re-exports of the underlying primitives for power users
    %format-to-string)
  (begin
    ;; `format` is the bare primitive — already a builtin, just re-export.
    (defun sprintf (fmt . args)
      (apply format (cons #f (cons fmt args))))

    (defun printf (fmt . args)
      (apply format (cons #t (cons fmt args))))

    (defun fprintf (port fmt . args)
      (apply format (cons port (cons fmt args))))

    (defun println-fmt (fmt . args)
      (apply format (cons #t (cons fmt args)))
      (newline))))
```

Naming rationale: `printf`/`sprintf`/`fprintf` are universally understood
and don't collide with existing `print`/`println` in `std.io` (which take
plain values, not format strings).

### 5.1 Prelude exports

Update `stdlib/prelude.eta`:

- Add `(import std.format)` after `(import std.io)`.
- Add export block:
  ```
  ;; -- std.format -----------------------------------------------------
  format sprintf printf fprintf println-fmt
  ```

`format` itself is a builtin so no further hookup is required, but adding
it explicitly to the prelude export list documents intent and matches how
`std.io` re-exports `display`.

---

## 6. Interaction With Existing Code

### 6.1 Existing escape set is preserved

`Lexer::consume_string()` currently accepts `\a \b \t \n \r \" \\ \xNN;`.
The new triple-string and interpolated paths share this escape table via a
common helper (`apply_escape(char, std::string& out)`) extracted from the
current switch. Raw strings skip it entirely.

### 6.2 `\u03BB` documented escape — verify

`docs/guide/strings.md` documents `\uXXXX`. `consume_string()` only
implements `\xNN;`. Decision: either implement `\u` for parity, or update
docs to match. **Plan: implement `\u{XXXX}` and `\uXXXX` in the shared
escape helper as part of this slice** so the new multi-line literals don't
inherit a documentation lie. (Keep `\xNN;` working for back-compat.)

### 6.3 `value_formatter.cpp` reuse

`%->display-string` and `~a`/`~s` go through the existing
`value_formatter::display(value, port)` / `write(value, port)` paths so
all numeric/character/structural printing stays consistent across
`display`, `format`, and interpolation.

### 6.4 LSP / DAP

The reader desugars interpolated strings before semantic analysis runs,
so `eta_lsp` and `eta_dap` need no changes for goto-definition or
breakpoint placement — interpolated holes appear as ordinary subexpr
spans with their original source positions preserved.

---

## 7. Tests

### 7.1 C++ runtime tests

`eta/test/src/format_tests.cpp`:

- All directives: `~a ~s ~d ~x ~o ~b ~e ~f ~g ~c ~% ~& ~~ ~_ ~t ~?`.
- Width and precision parsing edge cases.
- `(format #f ...)` returns string; `(format #t ...)` writes to current
  output port; `(format port ...)` honours `with-output-to-port`.
- Error paths: unknown directive, too few args, bad width.
- `%->display-string` on string/char/list/number/symbol.

`eta/core/test/src/lexer_string_tests.cpp` (or merge into existing
`lexer_tests.cpp`):

- All six literal forms tokenize.
- Indent strip fixtures (12+ Python-compatible cases).
- Interpolation desugar shape via parser.
- Span preservation through interpolation expression re-lex.
- Negative cases: unterminated `${`, empty `${}`, escape inside raw.

### 7.2 Stdlib integration tests

`stdlib/tests/format.test.eta`:

- `(sprintf "Hello, ~a!" 'world)` → `"Hello, world!"`.
- `(printf "x=~,3f y=~d~%" 3.14159 42)` captured via
  `with-output-to-port`.
- Round-trip: `(equal? (sprintf "~s" v) (display->string v))` for `v` in
  a small fixture set (intentionally NOT equal for strings: `~s` quotes,
  `display` does not — assert that explicitly).

`stdlib/tests/string-interp.test.eta`:

- `#"hi ${name}"` with `(define name "world")` → `"hi world"`.
- Triple-quoted multi-line indent strip on all the documented rules.
- Raw string: `r"\n"` length 2.
- Interpolated triple: `#"""…${(format #f "~,2f" x)}…"""`.

---

## 8. Documentation

- Rewrite `docs/guide/strings.md`:
  - Add a "Multi-line strings" subsection with indent-strip rules.
  - Add an "Interpolation" subsection with the prefix-only rule.
  - Add a "Raw strings" subsection.
  - Replace the current "Formatting" section with a real `format`
    walkthrough; cross-link to `std.format` reference.
- Add `docs/guide/reference/format.md` (full directive table + examples).
- Add `docs/guide/reference/string-interpolation.md` (or fold into
  `strings.md`).
- Update `docs/guide/syntax-and-values.md` to mention the new literal
  prefixes.
- Update `docs/guide/reference/modules.md` to list `std.format`.
- Update `docs/release-notes.md` and `docs/next-steps.md`.
- Update `editors/vscode/` grammar / snippets:
  - TextMate string patterns for `"""…"""`, `#"…"`, `r"…"`, including
    embedded-expression highlighting inside `${…}`.
  - Snippets for `printf`, `sprintf`, and `#"${…}"` literal.

---

## 9. Build / Packaging Checklist

No new third-party dependency. Only first-party files and existing build
graph touched.

- [ ] `eta/core/src/eta/reader/lexer.cpp` — new scanner branches
- [ ] `eta/core/src/eta/reader/lexer.h` — new function decls + token kind
- [ ] `eta/core/src/eta/reader/types.h` — `InterpolatedString` token, new
      `LexErrorKind` values
- [ ] `eta/core/src/eta/reader/error_format.h` — render new errors
- [ ] `eta/core/src/eta/reader/parser.cpp` — desugar `InterpolatedString`
- [ ] `eta/core/src/eta/runtime/format_engine.{h,cpp}` — formatter
- [ ] `eta/core/src/eta/runtime/format_primitives.h` — `register_format_primitives`
- [ ] `eta/core/src/eta/runtime/builtin_names.h` — Formatting block
      (`format`, `%->display-string`, `%format-to-string`) in canonical
      order between Core and Port blocks
- [ ] `eta/interpreter/src/eta/interpreter/all_primitives.h` — call
      `register_format_primitives` in matching slot, update header
      comments
- [ ] `eta/core/CMakeLists.txt` — add `format_engine.cpp`
- [ ] `stdlib/std/format.eta` — new module
- [ ] `stdlib/prelude.eta` — import + export
- [ ] `stdlib/tests/format.test.eta`, `stdlib/tests/string-interp.test.eta`
- [ ] `eta/test/src/format_tests.cpp`, lexer tests
- [ ] `editors/vscode/syntaxes/*.json` and `editors/vscode/snippets/*`
- [ ] Docs (Section 8)

---

## 10. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Interpolation breaks existing `"$..."` strings (e.g. JSON fixtures, regex sources) | Interpolation requires explicit `#"` prefix; plain `"..."` semantics are byte-identical to today. Add a regression test that lexes every `examples/*.eta` and `stdlib/**/*.eta` file unchanged. |
| Builtin slot drift between `builtin_names.h` and `all_primitives.h` | Add Formatting block as one atomic patch; existing builtin-sync test verifies counts and arities (same safeguard the logging plan relies on). |
| Triple-string indent rules surprise users | Document precisely with a worked example and a "gotchas" subsection; mirror Python `textwrap.dedent` behaviour exactly so prior knowledge transfers. |
| Span/diagnostic regressions for code inside `${...}` | Re-lex with offset-shifted `Span`s in the parser; add lexer-test cases that assert error positions point inside the original interpolated expression. |
| `~a` vs `display` divergence | Implement both via the same `value_formatter::display` entry point; round-trip test in `format.test.eta`. |
| Doc/runtime drift on `\u` escape | Implement `\uXXXX` and `\u{XXXX}` in the shared escape helper as part of this slice (Section 6.2). |
| Raw + interpolation combination is confusing | Allow it (`#r"..."`) but document with a clear example showing escapes are off but `${...}` is still expanded; add explicit lexer test. |
| `format`'s tilde directives clash with existing tilde usage in user strings | None today (tilde has no special meaning in `"..."`); `format` only interprets directives in its first format-string argument. |

---

## 11. Effort Estimate

| Slice | Effort |
|---|---|
| Lexer: triple-string + raw + escape helper extraction | 1.0 day |
| Lexer: interpolation tokenization + span tracking | 1.0 day |
| Parser: interpolation desugar + re-lex with shifted spans | 0.5 day |
| `format_engine` + directive parser + width/precision | 1.5 days |
| `register_format_primitives` + builtin slot wiring | 0.5 day |
| `std.format` module + prelude wiring | 0.25 day |
| `%->display-string` + reader/runtime hookup | 0.25 day |
| C++ tests (lexer + format engine) | 1.0 day |
| Stdlib tests (format + interpolation) | 0.5 day |
| Docs + VS Code grammar/snippets | 0.75 day |
| Regression sweep over `examples/`, `stdlib/` | 0.25 day |
| **Total** | **~7.5 days** |

---

## 12. Future Work (post-v1)

- `~{ ~}` iteration and `~[ ~]` conditional directives (full SRFI-48 / CL
  `format` parity).
- Python-style `${expr:spec}` mini-language inside interpolation, lowering
  to `(format #f "~spec" expr)` in the reader.
- `string-builder` / `with-string-output` ergonomic wrappers around
  `open-output-string` for hot concatenation loops.
- Locale-aware `format` directive (`~n` for thousands separator).
- User-extensible `format` directive registry.

