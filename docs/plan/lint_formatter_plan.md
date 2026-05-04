# Lint + Format Plan: `eta_lint` and `stonebridge`

[Back to README](../../README.md) ·
[Architecture](../architecture.md) ·
[Language Guide](../language_guide.md) ·
[Build](../build.md)

> **Status.** This is the authoritative plan for the Eta linter
> (`eta_lint`) and formatter (`stonebridge`). It is self-contained: an
> implementer should not need any other planning document to execute it.

---

## 0) Naming convention used in this document

| Concern        | Directory                  | CMake target     | Binary        |
| -------------- | -------------------------- | ---------------- | ------------- |
| Linter         | `eta/tools/linter/`        | `eta_linter`     | **`eta_lint`**     |
| Formatter      | `eta/tools/formatter/`     | `eta_formatter`  | **`stonebridge`** |
| Shared CST lib | `eta/core/` (new module)   | `eta_cst`        | — (library)   |
| Linter library | `eta/tools/linter/`        | `eta_linter_lib` | — (library)   |
| Format library | `eta/tools/formatter/`     | `eta_format_lib` | — (library)   |

`stonebridge` here names the **formatter** executable. The linter
executable is `eta_lint`, matching the existing `eta_lsp` / `eta_dap` /
`etac` / `etai` naming pattern.

---

## 1) Why two tools, not one

The linter and formatter answer different questions and have incompatible
contracts. Forcing them into one binary couples concerns that change at
different rates and risks one feature blocking releases of the other.

| Question                                          | `etac`/`etai` | `eta_lint` | `stonebridge` |
| ------------------------------------------------- | :-----------: | :--------: | :-----------: |
| Does it parse / compile?                          |       ✅      |     —      |       —       |
| Does it run correctly on all paths?               |       ❌      |     ✅     |       ❌      |
| Is it idiomatic / consistent / unused / shadowed? |       ❌      |     ✅     |       ❌      |
| Is the whitespace canonical?                      |       ❌      |     ❌     |       ✅      |
| Safe to run on untrusted source in CI?            |       ❌      |     ✅     |       ✅      |

Contracts:

1. **`eta_lint`** reports diagnostics. It may refuse to analyse code it does
   not understand. Its output is *information*. `--fix` is opt-in and limited
   to safe local edits.
2. **`stonebridge`** rewrites whitespace. It must accept every well-formed
   file. Its output is *bytes*. It guarantees round-trip safety,
   idempotence, and comment preservation.

Mixing these (one binary, one rule registry) makes both contracts weaker.

---

## 2) Shared foundations: do not duplicate

A single lossless concrete syntax tree (`eta_cst`) underpins both tools.
Duplicating it would let `eta_lint` and `stonebridge` drift on span and
comment placement — the linter would point at columns the formatter has
already moved. Worst possible UX.

```
eta_core            (existing)  reader: lexer, parser, diagnostics, spans
  └─ eta_cst        (NEW)       lossless CST + trivia (comments, blanks, byte spans)
       ├─ eta_linter_lib (NEW)  rule engine + rule registry
       │     ├─ eta_lint        binary
       │     └─ eta_lsp         publishDiagnostics
       └─ eta_format_lib (NEW)  Doc IR + layout engine + form aliases
             ├─ stonebridge     binary
             └─ eta_lsp         textDocument/formatting
```

`eta_lsp` is a *consumer* of both libraries; it owns neither. There is
nothing to extract from the existing LSP today (it is `lsp_server.{h,cpp}` +
`main_lsp.cpp` and already delegates parsing to `eta_core`).

### Why a new `eta_cst` rather than extending `eta_core::reader::SExpr`

1. `parser::SExpr` is **lossy** by design — comments and blank lines are
   discarded, only token-start spans are kept. The compiler hot path depends
   on it staying small.
2. `eta_cst` reuses `eta_core::reader::lexer` but re-tokenises with trivia
   retained, exposing a `cst::Node` tree with full byte ranges plus
   leading/trailing trivia.
3. The compiler/interpreter keep using `parser::SExpr`. Tools use
   `cst::Node`. One lexer, two views.

### What lives in each library

`eta_cst`:

1. `cst::Trivia` — comment kind (`Line`, `Block`, `Datum`), blank-line
   count, original text, span.
2. `cst::Node` variants — `Atom`, `List`, `Quoted`, `VectorLiteral`,
   `BytevectorLiteral`, `Error` (for tolerant parses).
3. `cst::parse(source) -> Result<File, Diagnostic>` — tolerant.
4. `cst::to_sexpr(node) -> parser::SExpr` — bridge to the lossy view.

`eta_linter_lib`:

1. `Engine` (lint a file/string), `Diagnostic` (extends `eta::diagnostic`
   with `rule_code`), `RuleRegistry`, `Rule` interface, `Config` parser,
   `Reporter` (pretty/json/sarif/github), `SourceEdit` for `--fix`.

`eta_format_lib`:

1. `Doc` — Wadler/Leijen pretty-print IR.
2. `FormAlias` registry mapping head symbols to indentation rules.
3. `layout(node, width) -> Doc`, `render(doc, width) -> string`,
   `format_source(src, opts) -> Result<string, Diagnostic>`.

---

## 3) Repository layout

```
eta/
├── core/
│   └── src/eta/cst/                  # NEW shared CST + trivia
│       ├── trivia.h / .cpp
│       ├── node.h   / .cpp
│       ├── parse.h  / .cpp
│       └── bridge_sexpr.h / .cpp
└── tools/
    ├── linter/                       # eta_lint
    │   ├── CMakeLists.txt
    │   ├── README.md
    │   ├── src/eta/linter/
    │   │   ├── main_eta_lint.cpp
    │   │   ├── cli/        (args, reporter)
    │   │   ├── core/       (engine, scope, registry, fix, config)
    │   │   ├── rules/
    │   │   │   ├── naming/        # ETA1xx
    │   │   │   ├── imports/       # ETA2xx
    │   │   │   ├── bindings/      # ETA3xx
    │   │   │   ├── style/         # ETA4xx (whitespace-adjacent only)
    │   │   │   ├── complexity/    # ETA5xx
    │   │   │   └── bugs/          # ETA6xx
    │   │   └── lib.h
    │   └── tests/
    │       ├── unit/
    │       ├── fixtures/<CODE>_<slug>/{input.eta,expected.json}
    │       └── golden/runner.cpp
    └── formatter/                    # stonebridge
        ├── CMakeLists.txt
        ├── README.md
        ├── src/eta/format/
        │   ├── main_stonebridge.cpp
        │   ├── cli/      (args)
        │   ├── doc.h     / .cpp
        │   ├── layout.h  / .cpp
        │   ├── render.h  / .cpp
        │   ├── forms.h   / .cpp     # FormAlias registry
        │   └── format_source.h / .cpp
        └── tests/
            ├── unit/
            ├── golden/<name>.{input,expected}.eta
            └── property/             # roundtrip, idempotence, comment-count
```

### Note: no "structure" rule category

There is deliberately no `ETA0xx` rule family for things like unbalanced
parens or malformed `defun` — those are diagnostics the compiler
(`etac`) already produces, and re-implementing them in the linter would
just risk drift. When `eta_lint` is run on a file that fails to parse,
it **surfaces** the compiler's parse diagnostic in lint format and
exits with code `2`; it does not own the detection logic.

---

## 4) Executable naming (CMake)

### Linter — `eta_lint`

```cmake
add_executable(eta_linter
    src/eta/linter/main_eta_lint.cpp
    src/eta/linter/cli/args.cpp
    src/eta/linter/cli/reporter.cpp)
set_target_properties(eta_linter PROPERTIES
    OUTPUT_NAME eta_lint
    CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(eta_linter PRIVATE eta_linter_lib)
install(TARGETS eta_linter RUNTIME DESTINATION bin)
```

### Formatter — `stonebridge`

```cmake
add_executable(eta_formatter
    src/eta/format/main_stonebridge.cpp
    src/eta/format/cli/args.cpp)
set_target_properties(eta_formatter PROPERTIES
    OUTPUT_NAME stonebridge
    CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(eta_formatter PRIVATE eta_format_lib)
install(TARGETS eta_formatter RUNTIME DESTINATION bin)
```

Both `eta_linter_lib` and `eta_format_lib` are separate `add_library(...
STATIC)` targets so `eta_lsp` can link them without pulling either
`main_*.cpp`.

---

## 5) Linter (`eta_lint`)

### 5.1 Scope

Catch what the compiler will not: style, conventions, unused/dead code,
shadowing, complexity, deprecated forms, suspicious calls.

**Guiding principle:** *Never duplicate a diagnostic the compiler already
produces.* If `etac` rejects it, the linter does not need a rule for it.

### 5.2 Rule catalogue (initial, ~20 rules across 6 categories)

Severity defaults: **E** error, **W** warning, **I** info.

#### Naming (ETA1xx)

1. **ETA101 — PredicateMissingQuestionMark** (W)
2. **ETA102 — MutatorMissingBang** (W)
3. **ETA103 — PrivateNotPrefixed** (I) — module-local symbol not prefixed `%`
4. **ETA104 — NonKebabCase** (I)
5. **ETA105 — ReservedShadow** (W) — user defines `car`, `if`, etc.

#### Imports / exports (ETA2xx)

1. **ETA201 — UnusedImport** (W)
2. **ETA202 — ExportedSymbolUndefined** (E)
3. **ETA203 — DuplicateExport** (W)
4. **ETA204 — SelfImport** (E)
5. **ETA205 — ImportOrder** (I) — auto-fixable; sorts `import` lines

#### Bindings (ETA3xx)

1. **ETA301 — UnusedLetBinding** (W)
2. **ETA302 — UnusedParameter** (I) — `_`-prefix opts out
3. **ETA303 — ShadowsBuiltin** (W)
4. **ETA304 — ShadowsOuterBinding** (I)

#### Style — comments only, **never** whitespace (ETA4xx)

Whitespace is the formatter's job. The linter only handles comment-marker
style, which the formatter deliberately leaves alone.

1. **ETA401 — CommentMarkerStyle** (I) — `;` end-of-line, `;;` statement,
   `;;;` banner. Auto-fixable.

#### Complexity (ETA5xx)

1. **ETA501 — FunctionTooLong** (W) — default 60 lines
2. **ETA502 — NestingTooDeep** (W) — default 6
3. **ETA503 — ArityTooHigh** (I) — default 7
4. **ETA504 — CyclomaticComplexity** (I) — counts `cond`/`if`/`and`/`or`

#### Common bugs (ETA6xx)

1. **ETA601 — IfMissingElseInValuePosition** (W)
2. **ETA602 — EqualsOnNonNumeric** (W) — `=` on string/symbol; suggest `equal?`
3. **ETA603 — DeadCodeAfterTail** (W) — forms after `error`/`raise` in tail position
4. **ETA604 — DuplicateCondClause** (W)
5. **ETA605 — UnreachableElse** (W) — clause after `(else …)`

### 5.3 Configuration

**Project file** `.eta-lint.toml` (the formatter has no config file by
design, so this name is unambiguous):

```toml
[eta-lint]
include      = ["stdlib/**/*.eta", "cookbook/**/*.eta"]
exclude      = ["**/build/**", "**/.eta/**"]
max-warnings = 0
fix          = false

[rules]
ETA101 = "warn"
ETA104 = "off"
ETA501 = { severity = "warn", max-lines = 80 }

[rules.preset]
use = "recommended"   # or "all", "minimal"
```

**Inline directives** (recognised by the trivia scanner):

```scheme
;; eta-lint:disable=ETA301        ; disables for the next form
;; eta-lint:disable-line=ETA401   ; disables for the current line
;; eta-lint:disable-file=ETA104   ; whole file (top-of-file only)
;; eta-lint:enable=ETA301         ; re-enables until end of enclosing form
```

### 5.4 CLI surface

```
eta_lint [paths...]                 # recursive lint of given paths (cwd if none)
eta_lint --config <path>            # explicit config file
eta_lint --fix                      # apply auto-fixes in-place (safe rules only)
eta_lint --format pretty|json|sarif|github   # default: pretty (tty) / github (CI)
eta_lint --rules ETA101,ETA301      # only run these rules
eta_lint --no-rule ETA104           # repeatable
eta_lint --max-warnings N
eta_lint --stdin --stdin-filename foo.eta
eta_lint --quiet
eta_lint --no-color
eta_lint --jobs N
eta_lint --list-rules
eta_lint --explain ETA301
eta_lint --version
eta_lint --help
```

### Exit codes

1. `0` — clean, or only infos.
2. `1` — errors, or warnings above `--max-warnings`.
3. `2` — input file failed to parse (compiler diagnostic surfaced).
4. `64` — bad CLI usage.
5. `70` — internal error.

### 5.5 Auto-fix policy

`--fix` is opt-in and limited to **textual, local, idempotent** edits:

1. **ETA205** — sort import lines alphabetically inside the `module` form.
2. **ETA401** — rewrite comment markers to canonical style.

That is the entire auto-fix surface for v1. Anything that touches inter-token
whitespace is delegated to `stonebridge`. Rules that need AST-aware rewrites
(e.g. ETA101 rename `foo` → `foo?`) are surfaced as `note:` suggestions
only.

### 5.6 Output formats

Pretty (default), JSON, SARIF 2.1.0, GitHub Actions workflow commands.
Examples:

**Pretty:**

```
cookbook/basics/basics.eta:23:5: warning[ETA605]: unreachable cond clause after (else …)
   |
23 |     (cond
24 |       ((= 1 2) "nope")
   |     ^^^^
   = help: remove the trailing clauses or move them above (else …)

eta_lint: 1 error, 3 warnings, 0 infos in 14 files (0.42s)
```

**JSON:**

```json
{
  "version": "1",
  "summary": { "errors": 1, "warnings": 3, "infos": 0, "files": 14 },
  "diagnostics": [
    {
      "file": "cookbook/basics/basics.eta",
      "span": { "start": {"line":23,"col":5}, "end": {"line":26,"col":6} },
      "rule": "ETA605",
      "severity": "warning",
      "message": "unreachable cond clause after (else …)"
    }
  ]
}
```

**GitHub Actions:**

```
::warning file=cookbook/basics/basics.eta,line=23,col=5,title=ETA605::unreachable cond clause after (else …)
```

---

## 6) Formatter (`stonebridge`)

### 6.1 Guarantees

1. **Round-trip:** `parse(format(src)) ≡ parse(src)` semantically.
2. **Idempotent:** `format(format(src)) == format(src)` byte-equal.
3. **Comment-preserving:** comment count and text bag unchanged.
4. **Single style:** no per-project rule knobs. Only `--max-width` and
   `--indent`. (Borrowed from `gofmt` / `rustfmt`.)

### 6.2 Lossless CST trivia model

Every token carries leading and trailing trivia:

1. *Leading* trivia of token `T` = whitespace/comments from the previous
   token's end up to `T`'s start.
2. *Trailing* trivia of `T` = up to the next newline.

(Roslyn / rust-analyzer convention; reproduces idiomatic Lisp commentary
placement automatically.)

```scheme
(defun foo (x)   ; trailing trivia of `)`
  ;; leading trivia of `(if ...`
  (if (> x 0) x (- x)))
```

Blank lines are recorded as `blank_lines_before` (capped: 1 between
top-level forms, 0 inside forms). The formatter never *inserts* blank
lines but preserves up to the cap that the user wrote.

Comment kinds: `;`, `;;`, `;;;`, `#| … |#`, `#;<datum>`. The formatter
**does not rewrite comment markers** — that is `eta_lint` ETA401.

### 6.3 Layout algorithm

Wadler/Leijen pretty-printer with `Group`/`IfFlat`, plus a small registry
of form-specific aliases keyed off the head symbol.

```
Doc ::= Nil | Text(s) | Line | SoftLine | HardLine
      | Nest(n, Doc) | Align(Doc) | Group(Doc) | Concat(Doc, Doc)
```

**Default list rule.** Generic `(h a b c)` is laid out as

```
Group( "(" <> head <> Nest(2, args_separated_by_Line) <> ")" )
```

Flat if it fits within remaining width: `(h a b c)`. Otherwise head on the
open line, each arg on its own line indented 2.

**Form aliases (initial).**

| Head                                                    | Style                                                     |
| ------------------------------------------------------- | --------------------------------------------------------- |
| `defun`, `define`, `lambda`, `let`, `let*`, `letrec*`   | Body indent 2; signature/bindings on first line if fits   |
| `let` (named-let), `do`                                 | Same as `let`                                             |
| `cond`                                                  | Each clause on its own line indented 2                    |
| `case`                                                  | Like `cond`                                               |
| `if`                                                    | Collapsed `(if p t e)` if fits; else `t`/`e` indented 4   |
| `when`, `unless`                                        | Body indent 2                                             |
| `begin`, `module` body                                  | Each form on its own line; one blank line preserved       |
| `module` head                                           | `(module name` + `import`/`export`/`begin` indented 2     |
| `import`, `export`                                      | Inline if fits; otherwise one symbol per line indented 2  |
| `define-record-type`                                    | Constructor / predicate / fields each on own line, indent 2 |
| `quote`, `quasiquote`, `unquote`, `unquote-splicing`    | Reader-macro shorthand preserved (`'x`, never `(quote x)`) |

The registry is a code-only table; not user-extensible in v1. Adding an
entry is a code change reviewed against the round-trip test suite.

### 6.4 Worked example

Input:

```scheme
(
define
foo
1)
```

CST → `List(open=(, [Atom(define), Atom(foo), Atom(1)], close=))`.
`define` with two args fits flat in 100 columns →

```
(define foo 1)
```

The user's `shoe.eta` sample (already canonical) is a no-op — the
idempotence test exercises this.

### 6.5 CLI surface

```
stonebridge [paths...]              # rewrite files in place (default)
stonebridge --check [paths...]      # exit 1 if any file would change
stonebridge --stdout [paths...]     # write to stdout
stonebridge --stdin --stdin-filename foo.eta
stonebridge --diff [paths...]       # unified diff; exit 1 if non-empty
stonebridge --max-width N           # default 100
stonebridge --indent N              # default 2
stonebridge --no-color
stonebridge --jobs N
stonebridge --version
stonebridge --help
```

### Exit codes

1. `0` — clean (or `--check`/`--diff` reports no changes).
2. `1` — `--check`/`--diff` found changes; or in-place mode found a parse error.
3. `64` — bad CLI usage.
4. `70` — internal error (round-trip / idempotence assertion failed).

There is deliberately no `--config` and no per-rule selection.

### 6.6 Tolerant parsing

If the CST contains an `Error` node, `stonebridge` refuses to format the
file and exits non-zero with the original `eta::diagnostic` rendered. A
formatter must never invent structure for malformed input.

---

## 7) Division of responsibility (cheat sheet)

| Concern                                     | Owner          |
| ------------------------------------------- | -------------- |
| Whitespace, indentation, line-breaking      | `stonebridge`  |
| Comment marker style (`;` vs `;;`)          | `eta_lint` (ETA401) |
| Comment preservation (text + count)         | `stonebridge`  |
| Blank-line preservation (capped)            | `stonebridge`  |
| Naming conventions (`?`, `!`, `%`, kebab)   | `eta_lint`     |
| Unused/shadowed bindings, imports, params   | `eta_lint`     |
| Dead code, duplicate cond clauses           | `eta_lint`     |
| Complexity caps                             | `eta_lint`     |
| Suspicious calls (`=` on strings, etc.)    | `eta_lint`     |
| Sort imports                                | `eta_lint --fix` (ETA205) |
| Reflow `(if p t e)` to fit width            | `stonebridge`  |
| Parse errors / unbound symbols              | `etac`/`etai`, surfaced by `eta_lint` |

If a feature falls in both columns: it belongs to whichever tool owns the
output file. `stonebridge` owns whitespace bytes; `eta_lint` owns
diagnostics. They never overlap.

---

## 8) Integration points

### 8.1 CMake wiring

Add to `eta/core/CMakeLists.txt`:

```cmake
add_library(eta_cst STATIC
    src/eta/cst/trivia.cpp
    src/eta/cst/node.cpp
    src/eta/cst/parse.cpp
    src/eta/cst/bridge_sexpr.cpp)
target_link_libraries(eta_cst PUBLIC eta_core)
```

Add to `eta/CMakeLists.txt`:

```cmake
add_subdirectory("${ETA_LAYOUT_ROOT_DIR}/tools/linter")
add_subdirectory("${ETA_LAYOUT_ROOT_DIR}/tools/formatter")
```

`eta/tools/linter/CMakeLists.txt`:

```cmake
add_library(eta_linter_lib STATIC
    src/eta/linter/core/lint_engine.cpp
    src/eta/linter/core/scope.cpp
    src/eta/linter/core/rule_registry.cpp
    src/eta/linter/core/config.cpp
    src/eta/linter/core/fix.cpp
    # rules/**/*.cpp
)
target_link_libraries(eta_linter_lib PUBLIC eta_cst)
eta_spdlog_target(_spd)
target_link_libraries(eta_linter_lib PRIVATE ${_spd})

add_executable(eta_linter
    src/eta/linter/main_eta_lint.cpp
    src/eta/linter/cli/args.cpp
    src/eta/linter/cli/reporter.cpp)
set_target_properties(eta_linter PROPERTIES
    OUTPUT_NAME eta_lint
    CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(eta_linter PRIVATE eta_linter_lib)
install(TARGETS eta_linter RUNTIME DESTINATION bin)
add_subdirectory(tests)
```

`eta/tools/formatter/CMakeLists.txt`:

```cmake
add_library(eta_format_lib STATIC
    src/eta/format/doc.cpp
    src/eta/format/layout.cpp
    src/eta/format/render.cpp
    src/eta/format/forms.cpp
    src/eta/format/format_source.cpp)
target_link_libraries(eta_format_lib PUBLIC eta_cst)

add_executable(eta_formatter
    src/eta/format/main_stonebridge.cpp
    src/eta/format/cli/args.cpp)
set_target_properties(eta_formatter PROPERTIES
    OUTPUT_NAME stonebridge
    CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(eta_formatter PRIVATE eta_format_lib)
install(TARGETS eta_formatter RUNTIME DESTINATION bin)
add_subdirectory(tests)
```

### 8.2 LSP wiring

`eta_lsp` links **both** libraries:

```cmake
target_link_libraries(eta_lsp PRIVATE eta_linter_lib eta_format_lib)
```

and implements:

1. `publishDiagnostics` ← `linter::Engine::lint_text(uri, content)` on
   document change. Rule codes flow through unchanged.
2. `textDocument/formatting` ← `format::format_source(buffer, opts)`,
   returned as a single full-document `TextEdit`.
3. `textDocument/rangeFormatting` ← format the smallest enclosing
   top-level form; return its replacement edit.

The rule registry is partitioned into `fast` (lex/AST-local) and `slow`
(scope/dataflow) sets; LSP runs `fast` on every keystroke and `slow` on
save.

### 8.3 VS Code extension

`editors/vscode/`:

1. Settings: `eta.lint.enabled` (default true), `eta.lint.path` (default
   `eta_lint`); `eta.format.path` (default `stonebridge`),
   `eta.format.maxWidth`, `eta.format.onSave`.
2. Problem matcher `$eta-lint` parsing
   `file:line:col: severity[CODE]: message`.
3. Tasks: `Eta: Lint workspace` (runs `eta_lint --format github .`) and
   `Eta: Format workspace` (runs `stonebridge .`).
4. Register `DocumentFormattingEditProvider` /
   `DocumentRangeFormattingEditProvider` delegating to `eta_lsp`.

### 8.4 Linter ↔ formatter relationship

They are **independent binaries**. They never call into each other.

Recommended operator workflow (and pre-commit hook order):

```
1. stonebridge --check         # are we formatter-clean?
2. eta_lint   --max-warnings 0 # any lint regressions?
```

For `--fix` flows, the order is:

```
1. eta_lint --fix              # may rewrite import order, comment markers
2. stonebridge                 # canonicalise whitespace
```

Never the reverse — `eta_lint --fix` may invalidate spans that
`stonebridge` just produced.

### 8.5 CI and pre-commit

1. `.github/workflows/lint.yml` — two jobs:
   - `format-check`: build `eta_formatter`, run `stonebridge --check stdlib cookbook demo`.
   - `lint-check`: build `eta_linter`, run `eta_lint --max-warnings 0 --format github stdlib cookbook`.
2. `scripts/pre-commit-eta.sh` — runs both checks on staged `.eta` files
   in the order above; non-zero blocks commit.
3. **Bootstrap commit** for the formatter, titled
   `chore: stonebridge-bootstrap`, runs
   `stonebridge stdlib cookbook demo` once and commits the result. Hash
   recorded in `docs/stonebridge.md` so `git blame` users can
   `--ignore-rev` it.

---

## 9) Testing strategy

### 9.1 Linter

1. **Per-rule unit tests** under `tests/fixtures/<CODE>_<slug>/` with
   `input.eta` + `expected.json`. Walked by a golden runner; `--bless` to
   update.
2. **CLI integration tests** spawning the `eta_lint` binary; assert exit
   codes for `--max-warnings`, `--fix`, `--stdin`.
3. **Cookbook regression** (Phase 3): `eta_lint --max-warnings 0 cookbook`
   in CTest.
4. **Stdlib regression** (Phase 3): same on `stdlib/std/`.
5. **Auto-fix idempotence**: each fix-capable rule run twice; second pass
   reports zero diagnostics and produces zero edits.
6. **Fuzz** via `qa/fuzz`: never crash on malformed input.

### 9.2 Formatter

1. **Doc IR unit tests**: handcrafted `Doc` values rendered at widths 80,
   40, 20.
2. **Form-rule unit tests**: one per entry in the alias table; short and
   long ("doesn't fit") variants.
3. **Golden corpus** at `tests/golden/<name>.{input,expected}.eta` with
   `--bless`.
4. **Round-trip property** over `cookbook/`, `stdlib/`, `demo/` — assert
   `parse(formatted) ≡ parse(original)`. CTest entry
   `stonebridge_roundtrip`.
5. **Idempotence property** — same corpus, format twice, byte-equal.
   `stonebridge_idempotent`.
6. **Comment preservation** — same corpus, comment count + text bag
   unchanged.
7. **Fuzz** via `qa/fuzz`: must never crash; either produce output or a
   `Diagnostic`.
8. **Bootstrap dogfood** (Phase 3): `stonebridge --check stdlib cookbook`
   gates CI.

### 9.3 Joint

1. **Order safety test**: pick 50 cookbook files. Run `eta_lint --fix`
   then `stonebridge`; assert clean. Then run twice more; assert
   idempotent. Then run `stonebridge` then `eta_lint --fix`; assert no
   regression in lint diagnostics. (Confirms the documented order is
   correct *and* the reverse order is not catastrophic.)

---

## 10) Documentation deliverables

1. `docs/eta_lint.md` — linter user guide: install, configure, CLI,
   rule reference, exit codes.
2. `docs/eta_lint_rules.md` — auto-generated rule catalogue from
   `rules/**/*.md`.
3. `docs/stonebridge.md` — formatter user guide: install, CLI, exit
   codes, editor integration, FAQ ("why no config?").
4. `docs/stonebridge_style.md` — canonical style spec, before/after for
   each form alias. Argued about *once*; frozen at 1.0.
5. Append "Linting and formatting" section to `docs/architecture.md`
   covering the `eta_cst` shared layer.
6. `eta/tools/linter/README.md` — how to add a rule.
7. `eta/tools/formatter/README.md` — how to add a form alias, debug
   layout decisions.
8. `editors/vscode/README.md` — settings + format-on-save + lint-on-save.

---

## 11) Phased delivery roadmap

### Phase 0 — Shared foundation

1. Build `eta_cst` (trivia model, CST nodes, tolerant parse, S-expr
   bridge).
2. Unit tests for trivia attachment, blank-line capping, all five comment
   kinds.

Gate: round-trip `cst::to_text(cst::parse(src)) == src` over `cookbook/`.

### Phase 1 — `stonebridge` MVP

1. `eta_format_lib` Doc IR + renderer + generic list rule (no aliases).
2. `stonebridge` CLI: `--stdin`, `--stdout`, `--check`, `--diff`,
   `--max-width`, `--version`, `--help`.
3. Round-trip + idempotence + comment-count assertions every run.
4. Golden corpus seeded with 10 fixtures.

Gate: `stonebridge --check cookbook/basics/hello.eta` passes; round-trip
and idempotence green on `cookbook/basics/`.

### Phase 2 — `eta_lint` MVP

1. `eta_linter_lib` engine consuming `eta_cst`.
2. CLI: arg parser, pretty + JSON reporters, exit codes.
3. Five seed rules: ETA101, ETA201, ETA301, ETA605, ETA601.
4. Per-rule fixtures + golden runner.
5. Tool README + `docs/eta_lint.md` skeleton.

Gate: `eta_lint cookbook/basics/basics.eta` produces correct output;
CTest passes.

### Phase 3 — Full rule + alias coverage

1. `stonebridge`: every form alias from §6.3 implemented; ≥80 golden
   fixtures; round-trip and idempotence green over **all** of `cookbook/`
   and `demo/`.
2. `eta_lint`: full rule catalogue from §5.2; `.eta-lint.toml` parser;
   inline disable directives; `--list-rules`, `--explain`.
3. Auto-fix for ETA205, ETA401.

Gate: `stonebridge --check cookbook demo` exit 0; cookbook lints clean
with project config.

### Phase 4 — Editor + CI bootstrap

1. `eta_lsp` links both libraries; `publishDiagnostics` and
   `textDocument/formatting`/`rangeFormatting` work.
2. VS Code provider, settings, status bar, problem matcher, tasks.
3. CI `format-check` and `lint-check` jobs; pre-commit hook.
4. **Bootstrap commit** for `stdlib/` (formatter); from this commit the
   stdlib must stay clean.
5. Lint `stdlib/std/` clean at `--max-warnings 0`.

Gate: stdlib formatter-clean and lint-clean in main; LSP works in VS
Code.

### Phase 5 — Hardening & 1.0

1. Block + datum comment edge cases; quasiquote / nested-unquote audit.
2. Performance: parallel workers, incremental cache keyed on file hash.
3. Public API freeze on `eta_format_lib` and `eta_linter_lib`; semver
   from here.
4. Style spec frozen.

Gate: 1.0 release.

---

## 12) Risks and open questions

1. **CST drift between tools.** Mitigated by sharing `eta_cst` as the
   *only* trivia-aware tree. Any feature requiring trivia goes through
   `eta_cst`; no parallel implementations allowed.
2. **Spec arguments at bootstrap.** `if`/`cond`/long-arg-list breaking
   are flashpoints. Mitigation: publish `docs/stonebridge_style.md` for
   review *before* the stdlib bootstrap commit; freeze at 1.0; only
   major versions can change.
3. **Round-trip on macros.** `syntax-rules` patterns have literal `…`
   and `_`. Add explicit golden cases under
   `tests/golden/syntax_rules/`.
4. **Reader macros.** `'x`, `` `x ``, `,x`, `,@x` must format as
   shorthand, never expand to `(quote x)`. CST stores the original
   form.
5. **CRLF / BOM.** Preserve original file's line ending; preserve BOM
   if present at offset 0; never inject. Mixed line endings → normalise
   to LF and warn.
6. **Strings.** Atoms; never reformatted, never reflowed.
7. **`cookbook/notebooks/`.** Large generated files. Respect `exclude`
   globs.
8. **Bootstrap diff size.** Land in a single PR with no other changes;
   record hash for `--ignore-rev`.
9. **Order of `eta_lint --fix` then `stonebridge`.** Documented as the
   only supported order; joint test in §9.3 enforces it.
10. **LSP perf budget.** Only `fast` lint rules run on every keystroke;
    `slow` rules run on save. Format requests only on explicit
    invocation, never on type.
11. **Config file naming.** `.eta-lint.toml` chosen over nesting under
    `eta.toml` (`[tool.eta-lint]`) for discoverability. Open question
    revisitable at end of Phase 3 based on user feedback.
12. **Rule-code stability.** Once published, codes never change meaning.
    Renaming a rule allocates a new code; old one marked
    `deprecated`.

---

## 13) Milestones / acceptance criteria (combined v1)

The combined linter + formatter v1 is complete when **all** of the
following hold:

- [ ] `cmake --build` produces binaries literally named `eta_lint` (from
      target `eta_linter`) and `stonebridge` (from target
      `eta_formatter`).
- [ ] `eta_cst`, `eta_linter_lib`, `eta_format_lib` exist as separate
      static libraries; `eta_lsp` links both tool libraries.
- [ ] `eta_lint --version`, `--help`, `--list-rules`, `--explain <code>`
      work; ≥20 rules implemented across the 6 categories.
- [ ] `stonebridge --version`, `--help`, `--check`, `--diff`, `--stdin`,
      `--stdout` work; every form alias from §6.3 implemented and
      golden-tested.
- [ ] Round-trip, idempotence, and comment-preservation tests green
      over `stdlib`, `cookbook`, `demo`.
- [ ] Pretty / JSON / SARIF / GitHub reporters in `eta_lint` produce
      conformant output (SARIF validated).
- [ ] `.eta-lint.toml` parsed; inline `disable`/`disable-line`/
      `disable-file`/`enable` directives honoured.
- [ ] `eta_lint --fix` applies safe edits and is idempotent.
- [ ] CI: `stonebridge --check stdlib cookbook demo` exits 0 and
      `eta_lint --max-warnings 0 stdlib cookbook` exits 0.
- [ ] `eta_lsp` surfaces lint diagnostics with rule codes and serves
      `textDocument/formatting` + `rangeFormatting`.
- [ ] VS Code: format-on-save and lint diagnostics visible in editor.
- [ ] Documentation present: `docs/eta_lint.md`, `docs/eta_lint_rules.md`
      (auto-gen), `docs/stonebridge.md`, `docs/stonebridge_style.md`,
      both tool READMEs, architecture appendix.
- [ ] Bootstrap commit landed; stdlib formatter-clean *and* lint-clean
      in main.
- [ ] Joint order-safety test from §9.3 green.
- [ ] Performance: `eta_lint` < 1 ms/file/rule; `stonebridge` < 2 ms/file
      on cookbook with `--jobs 1`.
- [ ] `stonebridge` has no user-tunable style options beyond
      `--max-width` and `--indent`.

