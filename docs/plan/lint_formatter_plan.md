# Lint + Format Plan: `eta_lint` and `eta_fmt`

[Back to README](../../README.md) ·
[Architecture](../architecture.md) ·
[Language Guide](../language_guide.md) ·
[Build](../build.md)

> **Status.** This is the authoritative plan for the Eta linter
> (`eta_lint`) and formatter (`eta_fmt`). It is self-contained: an
> implementer should not need any other planning document to execute it.

---

## 0) Naming convention used in this document

| Concern | Directory | CMake target | Binary |
| --- | --- | --- | --- |
| Linter | `eta/tools/linter/` | `eta_linter` | **`eta_lint`** |
| Formatter | `eta/tools/formatter/` | `eta_formatter` | **`eta_fmt`** |
| Shared CST library | `eta/core/` (new module) | `eta_cst` | — |
| Linter library | `eta/tools/linter/` | `eta_linter_lib` | — |
| Formatter library | `eta/tools/formatter/` | `eta_format_lib` | — |

`eta_fmt` here names the **formatter** executable. The linter
executable is `eta_lint`, matching the existing `eta_lsp` / `eta_dap` /
`etac` / `etai` naming pattern.

---

## 1) Separate tools and their contracts

Keep two executables and independent libraries. The compiler owns language
validity; the linter reports justified diagnostics; the formatter owns
whitespace layout. Neither tooling library executes source, loads native
sidecars, or runs user macros to discover meaning. Resource limits also apply
to untrusted input (§9.5).

| Question | `etac` / `etai` | `eta_lint` | `eta_fmt` |
| --- | --- | --- | --- |
| Is the input valid under Eta's reader/compiler? | Authoritative | Consumes diagnostics | Uses reader validation |
| Are there provable suspicious uses or convention violations? | Compiler checks | Analysis-dependent rules | — |
| Is whitespace canonical? | — | — | Yes |
| Does the program behave correctly on every path? | No general guarantee | No general guarantee | No general guarantee |

1. **`eta_lint`** reports diagnostics with declared analysis prerequisites.
   Missing semantic information means a rule is skipped with coverage recorded,
   rather than a speculative warning. `--fix` is an explicit opt-in to proven
   safe edits (§5.5).
2. **`eta_fmt`** formats valid, supported reader syntax without changing its
   meaning. Known forms receive dedicated layouts; unfamiliar forms receive
   conservative layouts or exact source passthrough when boundaries are known.
   Malformed input and valid but unsupported/ambiguous reader constructs receive
   distinct diagnostics and no file changes. Reader coverage is a release gate;
   safety takes precedence over accepting an unmodelled construct.

The revision keeps the existing scope: two executables, three shared libraries,
phased rules and aliases, CLI, LSP, CI, and documentation. Semantic prerequisites
are delivery gates, not a reason to expand the MVP.

---

## 2) Shared foundations and lossless CST contract

`eta_cst` supplies one source-preserving representation to both tools. The
compiler/interpreter continue using the compact, lossy `parser::SExpr`.

```text
eta_core                 existing reader, diagnostics, module/builtin metadata
  +-- eta_cst            lossless scanner, tokens, CST, source maps
       +-- eta_linter_lib  rule engine, scope analysis, fixes, configuration
       |    +-- eta_lint
       |    +-- eta_lsp    diagnostics
       +-- eta_format_lib  Doc IR, layouts, form aliases, validation
            +-- eta_fmt
            +-- eta_lsp    formatting
```

`eta_lsp` consumes the two libraries directly; it owns neither implementation.
`eta_cst` is an internal library in v1, not an installed or supported public SDK.

### 2.1 Relationship to Eta's reader

Reuse Eta's lexical recognisers and reader rules through a lossless scanning
mode. `lexer::Lexer::next_token()` discards whitespace, ordinary comments and
datum comments using `next_raw_item()` and `skip_datum()`; simply wrapping
its token stream cannot reconstruct source. The shared `next_raw_item()` and
`scan_next()` entry points expose raw reader items and actual discard boundaries
while keeping the compiler's skip-trivia API and behaviour unchanged. Do not
create a second independent language grammar for tooling.

Phase 0 records the supported reader inventory from
`eta/core/src/eta/reader/{lexer,parser}.{h,cpp}`, with fixtures and source references
for delimiter matching, escapes, numeric spellings, directives, and strict versus
non-strict quasiquote modes. Current code recognises parentheses and brackets,
`#(` vectors, `#u8(` bytevectors, quote/backtick/comma/comma-at abbreviations,
line comments, nested block comments, datum comments, and `#!` directives or a
leading shebang. This inventory is a starting point, not an assumption that Eta
implements every Scheme dispatch form. The current fold-case directives are
no-ops; tooling must not assign them Scheme semantics independently.

Resolve one immutable `ReaderConfig` per operation: grammar/version, the caller's
`strict_quasiquote` setting and all initial lexical/reader flags. CLI defaults
follow the selected compiler frontend's reader configuration; the parser
constructor currently defaults to `strict_quasiquote=false`, but that does not
set every frontend's policy. LSP captures the document's language configuration,
and library callers pass it explicitly. This adds no reader-mode CLI flag or new style option. Original and
formatted text each start with a fresh reader using that identical configuration
and BOF state; never reuse the original reader's final state for the output.
Non-recovering validation does not mean forcing strict quasiquote mode.

Reader validation returns an ordered trace of directive kind/raw spelling,
enclosing reader path (including discarded data), and state before/after each
reader item. Compare traces through concrete-token correspondence, ignoring only
changed offsets. State includes every flag that affects later reading; current
no-op directives still have trace entries. If a required state transition or
scope cannot be represented, refuse rather than comparing expression trees alone.

Both reader views must agree on valid syntax and consumed datum boundaries.
Disagreements block the corresponding formatter/rule feature until resolved and
regression-tested; a tooling revision does not silently change reader semantics.

### 2.2 Tokens, nodes and unique trivia ownership

Every concrete token has a kind, its exact raw spelling as a source slice, and
an immutable token ID within its source snapshot. Tokens include opening and
closing delimiters, dotted-list dots, abbreviation and dispatch prefixes, atom
lexemes, directives, and a zero-width EOF token. Preserve `(...)` versus `[...]`,
raw strings and escapes, symbol escaping, numeric representations, and prefixes;
semantic equivalence never authorises canonicalising those bytes.

A file owns an ordered token stream. **Only tokens own trivia**, as a leading
sequence covering the gap since the preceding token. EOF owns the final gap.
Trivia records have snapshot-local IDs, kinds (`Whitespace`, `LineComment`,
`BlockComment`), half-open byte spans and exact raw source views. Whitespace and
ordinary comments occur exactly once in that sequence; nodes have token ranges
and child references, not a second leading/trailing trivia store. Header ownership
is concrete too:

- An initial UTF-8 BOM is a `FileBom` token owning exactly `[0,3)`; otherwise no
  BOM token exists. A later U+FEFF is not reclassified as a file BOM.
- An Eta-recognised shebang is one `Shebang` token, from `#!` through its body,
  excluding the line terminator. Its LF/CRLF terminator is ordinary leading
  whitespace of the next token, or EOF. Reader directives remain `Directive`
  tokens; they are not shebangs merely because they start with `#!`.
- Header comments remain ordinary token-owned trivia with header association.
  File header metadata references these IDs and owns no duplicate source slice.
  EOF is `[source_size,source_size)` and owns remaining trivia only, never BOM or
  shebang bytes already owned by tokens. Empty, BOM-only and shebang-only files
  use this same model, including a shebang with no final newline.

All raw token/trivia slices partition the input bytes without gaps or overlap;
source offsets include header bytes and are never adjusted by dropping a BOM.

Node variants are `Atom`, `List` (delimiter tokens, elements, optional dot and
tail), `ReaderForm` (exact prefix and operand), `VectorLiteral`,
`BytevectorLiteral`, **`DatumComment`**, `Directive`, and `Error`. A root retains
all top-level reader items in order. Missing delimiters may be represented by
virtual zero-width recovery tokens, which are never emitted by lossless
serialization. Any unsupported dispatch item retains its entire raw extent and
an explicit unsupported status; its shape is never guessed.

Trivia ownership is distinct from *layout association*. Association metadata
references existing comment/token IDs and does not own another copy (§6.2).
The CST records all blank lines verbatim; only the formatter applies blank-line
caps. Lossless serialization never caps whitespace or reconstructs atom text
from decoded values.

### 2.3 Datum comments and reader forms

A `DatumComment` references its `#;` token and the child reader items consumed
while finding and discarding the next effective datum, including that suppressed datum.
Intervening nested datum comments, ordinary comments, and abbreviations retain
explicit children/associations. Consumption follows Eta's `skip_datum` semantics,
including nested cases such as `#; #; a b`; it is not defined as attaching trivia
to the next visible node. Suppressed data remain in the CST but do not become
active expressions or lint references. Discarded syntax follows Eta's discard
routine, not the stricter active-expression grammar: a compiler-valid ignored
region must not be rejected merely because converting it to an active S-expression
would fail. Such uncertain discarded regions remain verbatim during formatting.

```scheme
(list 1 #;(expensive-expression) 2)
```

Formatting must keep `#;` bound to `(expensive-expression)`. Quote, quasiquote,
unquote and unquote-splicing similarly retain their exact prefix and operand,
including trivia between them. Explicit `(quote x)` stays explicit; `'x` stays
an abbreviation. Dispatch syntax and reader directives retain their original
order and semantic scope, including reader state inside discarded input as
specified by Eta. Tools do not expand macro invocations for formatting.

### 2.4 Source spans, ownership and recovery

`ByteSpan = [begin, end)` uses zero-based byte offsets into one immutable source
snapshot; empty spans are allowed for recovery/insertion points. A `File` owns
its source buffer and node/token arena. Views are slices of that buffer and may
not outlive the owning `File`; asynchronous consumers retain a shared snapshot,
never views into a mutable editor buffer. A snapshot carries file identity and
an optional document version. Memory is released when its last consumer ends.

A shared source map converts byte spans to one-based CLI line/Unicode-scalar
columns and to zero-based LSP positions using the negotiated position encoding
(UTF-16 by default, UTF-8 only when negotiated). CRLF is one logical newline;
tabs are one source character, not a guessed display width. Non-BMP characters,
EOF and zero-width spans have dedicated conversion tests. Edits never split a
UTF-8 code point or CRLF pair. Invalid UTF-8 remains losslessly stored for
reporting but prevents formatting; diagnostic conversion uses a deterministic
replacement-character mapping for invalid byte runs without editing the input.

Tolerant parsing returns `ParseResult { File, diagnostics, status }`, with status
`valid`, `recovered`, or `unsupported`. Recovery preserves erroneous and remaining
bytes as concrete tokens/raw error regions, makes forward progress, and may
resume at a proven sibling boundary. Unterminated strings/comments consume an
error region through EOF; guessed structure is never eligible for formatting or
semantic lint rules. Recovery diagnostics are distinguished from authoritative
reader diagnostics (§5.1).

**Foundational invariant:** for every source snapshot within §9.5 limits,
including recoverable malformed input, `serialize_losslessly(parse(src).file) == src` byte-for-byte.
For successfully parsed input, every concrete byte must also be structurally
accounted for. This is a permanent property test, not a one-off milestone.

### 2.5 Library boundaries

- `eta_cst`: source snapshots/maps, scanning, token/trivia storage, nodes,
  tolerant parsing, lossless serialization, and a fallible `to_sexpr` bridge.
  The bridge rejects recovery/unsupported nodes and is not the independent
  formatter oracle.
- `eta_linter_lib`: engine, rule registry/specifications, analysis coverage,
  diagnostics, scope model, configuration, reporters, and validated `SourceEdit`.
- `eta_format_lib`: Doc IR, alias registry, conservative layout, rendering,
  correctness validation, and `format_source(src, opts)` returning either a
  complete result or diagnostics. No partial formatted result on failure.

---

## 3) Repository layout

The CST subtree reflects Phase 0. The linter and formatter subtrees describe
the later implementation phases.

```
eta/
├── core/
│   └── src/eta/cst/                  # shared CST + trivia
│       ├── source.h / .cpp   # immutable source and position maps
│       ├── parse.h  / .cpp   # tokens, trivia, nodes, scanning and recovery
│       ├── diagnostic.h / .cpp # native diagnostic adaptation
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
    │   │   │   ├── style/         # ETA4xx (ordinary comment markers only)
    │   │   │   ├── complexity/    # ETA5xx
    │   │   │   └── bugs/          # ETA6xx
    │   │   └── lib.h
    │   ├── rules/<CODE>.toml           # authoritative specs (§5.2)
    │   └── tests/
    │       ├── unit/
    │       ├── fixtures/<CODE>_<slug>/{input.eta,expected.json}
    │       └── golden/runner.cpp
    └── formatter/                    # eta_fmt
        ├── CMakeLists.txt
        ├── README.md
        ├── src/eta/format/
        │   ├── main_eta_fmt.cpp
        │   ├── cli/      (args)
        │   ├── doc.h     / .cpp
        │   ├── layout.h  / .cpp
        │   ├── render.h  / .cpp
        │   ├── forms.h   / .cpp     # FormAlias registry
        │   └── format_source.h / .cpp
        └── tests/
            ├── unit/
            ├── golden/<name>.{input,expected}.eta
            └── property/             # roundtrip, idempotence, comment-association
```

### Diagnostic ownership

No `ETA0xx` family reimplements reader/compiler errors. Reader recovery exists
for editor resilience; authoritative language and module diagnostics are
consumed through the ownership/translation contract in §5.1. A recovered CST is
not proof that a file is valid, and an unavailable compiler service is not
permission to reconstruct semantic checks from textual searches.

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

### Formatter — `eta_fmt`

```cmake
add_executable(eta_formatter
    src/eta/format/main_eta_fmt.cpp
    src/eta/format/cli/args.cpp)
set_target_properties(eta_formatter PROPERTIES
    OUTPUT_NAME eta_fmt
    CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(eta_formatter PRIVATE eta_format_lib)
install(TARGETS eta_formatter RUNTIME DESTINATION bin)
```

Both `eta_linter_lib` and `eta_format_lib` are separate `add_library(...
STATIC)` targets so `eta_lsp` can link them without pulling either
`main_*.cpp`.

---

## 5) Linter (`eta_lint`)

### 5.1 Analysis levels, confidence and diagnostic ownership

Rules state both their minimum analysis level and the evidence needed for a
finding. There is no heuristic substitute for a missing binding, type, module,
macro, or control-flow fact.

| Level | Available evidence | Eligible checks |
| --- | --- | --- |
| L0: lexical/CST | Exact syntax, comment associations, recognised structural forms | Naming spelling, comment markers, duplicate declarations, structural size |
| L1: scope-aware | Proven binding identities, lexical scopes, reference positions, binder semantics | Unused local bindings/parameters, lexical shadowing |
| L2: module/semantic | Compiler/shared metadata for modules, imported syntax, builtins, types, evaluation/control flow | Imports/exports, builtin shadowing, suspicious operations, unreachable execution |

Analysis returns facts as `proven` or `unknown`, together with prerequisites and
coverage. A diagnostic requires proven facts appropriate to the rule; rules are
skipped in unknown regions, and reporters expose unavailable analysis in a
coverage summary. A skip is not a clean semantic bill of health. Known lint
findings and compiler diagnostics remain reportable elsewhere in the file.

L1 walks recognised Eta binders, preserving simultaneous versus sequential and
recursive binding scopes. Quoted and datum-commented data are not executable
references; quasiquote accounts for active unquotes according to reader rules.
An unmodelled macro may introduce bindings or references: conservatively mark
its affected scope unknown and suppress dependent unused/shadowing rules.
Compiler-provided expansion/binding metadata may improve coverage only through
a non-executing service with original-source provenance. No user macro execution
or guessed symbol-use counting is permitted.

| Diagnostic class | Detection owner | Tooling behaviour |
| --- | --- | --- |
| Invalid tokens, malformed reader syntax, invalid UTF-8 | Shared reader/compiler | Surface original diagnostic; input failure status 2 |
| CST recovery or unsupported tooling syntax | `eta_cst` | Mark recovery/unsupported separately; do not present as a compiler rejection |
| Unknown modules, undefined exports, conflicting imports, cycles | Compiler module linker | Consume linker diagnostics; do not implement duplicate lint rules |
| Unresolved names, invalid special forms, compiler type errors | Compiler/shared semantic service | Consume when available; never recreate using textual name searches |
| Convention, complexity, proven unused/shadowed bindings | Linter | Emit stable ETA rule codes with declared prerequisites |
| Formatter safety invariant violation | Formatter | Internal error 70, no output/write for the affected file |

A diagnostic adapter retains `source` (`compiler`, `cst`, or `eta_lint`), original
code, half-open byte span, message, related locations and severity. Reporter
position conversion uses §2.4. If a native diagnostic has no public string code,
the adapter assigns a documented namespaced code from its native error kind;
it does not replace it with an ETA rule code. Compiler diagnostics are not
suppressed or downgraded by lint-rule configuration. Deduplicate only identical
owner/code/span findings, preferring authoritative reader diagnostics over
matching recovery diagnostics while preserving distinct recovery context.
For unlocated findings, include producer/module/message identity in the key;
a null span alone never merges unrelated diagnostics.

Before the linter MVP, Phase 0 records an exhaustive adapter inventory for every
native kind consumed, with producer/API, ownership, message/severity, location
availability, external code, and exit class (reader/input 2, semantic finding 1,
resource/operation 2, internal 70). Current starting interfaces are:

| Native interface | Adapter requirement |
| --- | --- |
| `lexer::LexError`, `parser::ParseError` / `ReaderError` | Translate native kinds/spans; retain lexer propagation and classify internal parser kinds as 70 |
| `expander::ExpandError`, `linker::LinkError` | Translate typed errors/messages; expose only through a non-executing service |
| `eta::diagnostic::Diagnostic` in `eta/diagnostic/diagnostic.h` | Preserve code, severity and related spans; verify producer phase/location rather than assuming all enum members are callable services |
| Text-only failures or unavailable semantic entry points | Preserve any reported message as an explicitly unclassified compiler diagnostic; record service/adapter coverage gaps rather than parsing prose into guessed locations |

Use `location = {file?, span?}`: a missing/default/unresolvable span is not a
fabricated `[0,0)` source location. Resolve file IDs through the producing service's
snapshot/file map; module diagnostics use the provider file when known and retain
module identity plus compiler-supplied import/related locations. Without a proven
primary location, JSON uses a null span, SARIF omits the region, pretty output
labels the file/module/project, and LSP sends a workspace notification instead of
inventing an underline. Preserve unknown native kinds/messages and report partial
adapter coverage; an unclassified error remains a status-1 compiler finding unless
the producer identifies an operation/internal failure. Unavailable services skip
dependent rules with coverage recorded. An inventory/adapter fixture must exist
for each consumed kind, including missing spans, module-global errors and unknown
kind handling; no diagnostic silently disappears.

The existing module linker already defines `ExportOfUnknownName`,
`ConflictingImport`, `UnknownModule`, and `CircularDependency`. ETA202 therefore
remains a catalogue reference to delegated compiler coverage, not an independent
rule implementation. Compiler semantic errors contribute to lint findings/status
1; failure to read or parse a file contributes status 2 (§5.4). Recovery alone
does not authorise semantic diagnostics. With malformed input, safe L0 checks
may run on intact regions, but the whole-file status remains an input failure.

### 5.2 Rule catalogue and authoritative specifications

The following catalogue is a candidate surface across six categories, not a
promise to ship unproven semantic rules. E/W/I are default error/warning/info.
Delegated entries retain their compiler severity/code. Gated rules enter a preset
only after their specification and prerequisites are verified.

| Code | Name | Default | Minimum level / prerequisite | v1 fix |
| --- | --- | --- | --- | --- |
| ETA101 | PredicateMissingQuestionMark | W | L1/L2: a proven predicate role/return contract; no name-based guessing | None |
| ETA102 | MutatorMissingBang | W | L2: proven mutation contract, not a suggestive call name | None |
| ETA103 | PrivateNotPrefixed | I | L2: module-local visibility actually resolved; `%` is a convention | None |
| ETA104 | NonKebabCase | I | L0: recognised definition name; escaped/generated names excluded | None |
| ETA105 | ReservedShadow | W | L2: reserved-form resolution at a definition site | None |
| ETA201 | UnusedImport | W | L2: import provenance, syntax uses and initialisation effects | None |
| ETA202 | ExportedSymbolUndefined | E | Delegated module-linker `ExportOfUnknownName`; no linter detector | None |
| ETA203 | DuplicateExport | W | L0: identical explicit export declarations in a recognised module | None |
| ETA204 | SelfImport | E | L2: canonical module identity; delegate if compiler already owns it | None |
| ETA205 | ImportOrder | I | L2: independently reorderable import group and stable sort keys | Conditional; safety proof required |
| ETA301 | UnusedLetBinding | W | L1: verified binder scopes and complete reference coverage | None |
| ETA302 | UnusedParameter | I | L1: verified parameter forms and complete reference coverage | None |
| ETA303 | ShadowsBuiltin | W | L2: resolved builtin identity, not symbol spelling | None |
| ETA304 | ShadowsOuterBinding | I | L1: two resolved lexical binding identities | None |
| ETA401 | CommentMarkerStyle | I | L0: ordinary line-comment association from §6.2 | Marker only |
| ETA501 | FunctionTooLong | W | L0: recognised function source extent; default 60 physical lines | None |
| ETA502 | NestingTooDeep | W | L0: recognised executable forms; default depth 6 | None |
| ETA503 | ArityTooHigh | I | L1: supported parameter grammar; default 7 required parameters | None |
| ETA504 | CyclomaticComplexity | I | L2: recognised branch semantics; no name-based `if`/`and` counts | None |
| ETA601 | IfMissingElseInValuePosition | W | L2: actual `if` semantics and proven value-required position | None |
| ETA602 | EqualsOnNonNumeric | W | L2: actual resolved `=` contract and operand type facts | None |
| ETA603 | DeadCodeAfterTail | W | L2: guaranteed non-returning control transfer and sequence context | None |
| ETA604 | DuplicateCondClause | W | L2: resolved `cond`, test structural equality and effect semantics | None |
| ETA605 | UnreachableElse | W | L2: resolved `cond` and authoritative `else`/later-clause semantics | None |

Only ETA401 concerns comment conventions; inter-token whitespace is the
formatter's responsibility. ETA105 concerns definitions shadowing reserved
syntax; ETA303 concerns resolved builtin bindings. If they describe the same
site, the rule specs select one diagnostic instead of duplicating it.

#### Rule-specific semantic gates

- **ETA201 / ETA205:** account for imports that initialise modules, introduce
  syntax, re-export names, or participate in cycles. An unused value name does
  not prove the import unnecessary, and alphabetic order does not prove safe
  reordering. Unknown effects suppress ETA201 and disable ETA205 fixes.
- **ETA301:** specify ordinary, sequential, recursive and multiple-value binders,
  plus destructuring only if Eta supports it. Recursive binding self-use and
  references introduced by macros need verified treatment; suppress uncertain
  scopes. Unused initialisers may have effects, so removal is not a v1 fix.
- **ETA302 / ETA503:** define required, optional and rest parameters only from
  Eta's supported grammar. The opt-out is a decoded identifier equal to `_` or
  beginning with `_`; it affects unused-parameter reporting only and does not
  imply macro wildcard semantics. Arity counts required positions; a rest slot
  is not an extra required parameter. Unknown signatures are skipped.
- **ETA303 / ETA304:** resolve lexical, imported and builtin identities, respecting
  local overrides. Intentional shadowing is legal where Eta permits it; these
  are configurable convention diagnostics, suppressible with directives.
- **ETA601:** specify whether omitted alternatives are legal and what value they
  produce. Define value-required contexts from Eta semantics, excluding discarded
  sequence positions. Do not infer Scheme's `if` behaviour.
- **ETA602:** verify the actual equality/type contract and any local rebinding.
  If `=` accepts the operand types, or types are unknown, emit nothing. Suggest
  `equal?` only if it exists and its contract is demonstrably appropriate.
- **ETA603:** the legacy name does not mean that a tail-position expression makes
  later text dead. Flag a later expression in a sequence only after a proven
  non-returning transfer with handler/continuation semantics accounted for.
  Calls spelled `error` or `raise` are insufficient evidence.
- **ETA604 / ETA605:** define `cond` and `else` from Eta, including legality of
  later clauses. Structural equality compares reader kinds/values ignoring
  positions, not printed strings. Repeated effectful tests do not prove a dead
  branch. Compiler-invalid clauses are delegated rather than linted again.

Each `eta/tools/linter/rules/<CODE>.toml` is the source of truth for registration,
generated documentation and fixture discovery. Its required schema is:

```toml
code = "ETA301"
name = "UnusedLetBinding"
owner = "eta_lint"                 # eta_lint or delegated compiler
status = "gated"                   # gated, enabled, delegated, deprecated
severity = "warn"                  # error, warn, info
analysis-level = "scope"           # cst, scope, semantic
rationale = "Report a binding with no resolved references."
prerequisites = ["verified-let-scopes", "complete-reference-coverage"]
metadata = ["binding-identities"]
unknown-behaviour = "skip-affected-scope"
false-positives = ["unmodelled macro references: suppress the affected scope"]
fix = "none"                      # none, safe, conditional
fix-safety = "No removal fix: the initializer can have effects."
positive-fixtures = ["ETA301_unused_let"]
negative-fixtures = ["ETA301_nested_use", "ETA301_recursive_use", "ETA301_macro_unknown"]
options = {}                       # typed option schema, defaults and constraints
presets = []                      # populated only when enabled and reviewed
```

Enabled specs require positive and negative examples, dependency/unknown cases,
exact diagnostic spans, and fix examples/safety proof when applicable. Schema
validation rejects missing fields, duplicate codes, invalid options and missing
fixture references. Rule documentation is generated from these specs and their
fixtures, not from an unspecified second set of Markdown rule files. Registry,
preset membership, documentation and fixture inventory must be reproducible from
the same checked-in specs and referenced fixtures: stable rule-code ordering,
fixed encoding, and no timestamps or checkout-dependent absolute paths. CI
regenerates artifacts and rejects differences from committed outputs (or compares
hermetic build outputs when not committed), and rejects any preset member whose
status/prerequisites have not met its enablement gate.

### 5.3 Configuration and directives

There is one `.eta-lint.toml` per invocation, with deterministic precedence:

1. Compiled defaults and the selected preset.
2. Explicit entries in `[rules]` override that preset.
3. CLI options override the resulting configuration. `--rules` replaces the set
   with explicitly requested enabled rules, using a configured non-off severity
   or the rule default; `--no-rule` then removes from it. Neither can enable
   a delegated/gated detector.

An explicit `--config` replaces discovery. Otherwise search upward from the
invocation's cwd, stopping after checking the nearest VCS root or filesystem
root; use the first `.eta-lint.toml` found. Do not discover different configs
for individual files during the same invocation. Globs are relative to the
config directory, or cwd if no config exists. Stdin uses the same discovery root;
`--stdin-filename` supplies only logical identity/module context.

```toml
[eta-lint]
include = ["stdlib/**/*.eta", "cookbook/**/*.eta"]
exclude = ["**/build/**", "**/.eta/**"]
max-warnings = 0
preset = "recommended"             # minimal, recommended, all

[rules]
ETA104 = "off"
ETA501 = { severity = "warn", max-lines = 80 }
```

Presets are concrete, versioned rule-code sets generated from approved specs.
`minimal` contains verified L0 checks; `recommended` adds approved L1/L2 checks;
`all` includes all enabled rules. None enables a gated or delegated detector.
Docs list the exact sets for each release. Unknown rule codes, invalid severities,
unknown/ill-typed options, invalid globs and explicit requests for gated rules
are configuration errors (64), detected before any file is modified. `--fix`
always requires the CLI flag; configuration cannot enable writes.

Directives are recognised only in ordinary line-comment tokens, not strings,
block comments or suppressed data:

```scheme
;; eta-lint:disable=ETA301        ; suppress next syntactic form and descendants
;; eta-lint:disable-line=ETA401   ; suppress on this physical source line
;; eta-lint:disable-file=ETA104   ; file header only, before first active form
;; eta-lint:enable=ETA301         ; cancel an inherited suppression for next form
```

Directives have token/form associations, not a second regex scan. `enable` applies
to the next form and descendants, then ends with that form; there is no implicit
"until end of enclosing form" state. Unknown codes and invalid directives produce
per-file directive diagnostics (status 2); affected files cannot be fixed. Compiler
diagnostics cannot be suppressed. Directive text and association are preserved
by formatting.

### 5.4 CLI and exit codes

```text
eta_lint [paths...]                         # recursive .eta discovery; cwd if none
eta_lint --config <path>
eta_lint --fix                              # explicit, validated safe fixes only
eta_lint --format pretty|json|sarif|github
eta_lint --rules ETA104,ETA301
eta_lint --no-rule ETA104                   # repeatable
eta_lint --exclude <glob>                   # repeatable; extends exclusions
eta_lint --max-warnings N                   # default: unlimited
eta_lint --stdin --stdin-filename foo.eta
eta_lint --quiet --no-color --jobs N
eta_lint --list-rules --explain ETA301 --version --help
```

The final line lists separate informational operations; they cannot be combined
with an analysis/write operation. Pretty is the default reporter, except
`GITHUB_ACTIONS=true` selects GitHub commands. `--quiet` hides informational
findings, not failures/coverage or errors/warnings. §6.7 defines shared discovery
and safe-write behaviour. Stdin is exclusive with paths and `--fix`; diagnostics
go to the selected reporter, not a transformed-source stdout stream.

| Exit | Meaning |
| --- | --- |
| 0 | No error findings and warnings within the limit; infos do not fail |
| 1 | Error findings (including delegated compiler semantic errors), or warning limit exceeded |
| 2 | At least one input/read/parse/write operation failed |
| 64 | Invalid usage or configuration; no processing/writes start |
| 70 | Internal error; stop scheduling further writes |

Preflight usage/configuration validation returns 64 before traversal. Once
processing begins, aggregation precedence is **70 > 2 > 1 > 0**, independent of
worker completion order. Continue unaffected files on input failure and report
all findings; a parse failure wins over another file's lint warnings. With
`--fix`, report diagnostics from re-analysis of the committed final source;
fixed findings do not count toward final thresholds. Compilation-only semantic
failures are findings, not reader parse failures. No unavailable-rule skip alone
changes the exit code; coverage is reported explicitly.

### 5.5 Auto-fix safety and edit engine

The candidate v1 fix surface stays limited to ETA205 and ETA401:

- **ETA205:** offer sorting only for a group whose ordering is proven
  unobservable under Eta's module initialisation, import/syntax and cycle rules.
  The specification must identify evidence for group safety, stable sort keys,
  comment attachment and directive boundaries. Otherwise report only a proven
  style violation, with no fix; if even group classification is unknown, skip
  the rule. There is no unconditional "sort import lines" implementation.
- **ETA401:** rewrite only the leading semicolon run of an ordinary line
  comment: `;` for end-of-line, `;;` for a standalone statement comment, `;;;`
  for an explicitly identified banner. Preserve the body bytes, terminator,
  order and association. Ambiguous banners/attachments receive no fix. Never
  match strings, block comments, datum comments or shebangs. Preserve recognised
  directive bodies/association and recalculate all resulting source spans.

`SourceEdit { [begin,end), expected_bytes, replacement, snapshot_id }` validates
bounds, UTF-8 boundaries and original bytes against the analysed buffer.
Deduplicate identical edits; reject any other overlapping edits (including
competing insertions at one offset) for the file with a fix-conflict diagnostic
and status 2. Never select a winning rule by worker order. Apply validated edits
in descending byte-offset order to a new buffer, then reparse/reanalyse it.
If semantic safety or any invariant fails, discard that file's entire edit set;
internal invariant violations return 70. A second fix pass on the same source
and configuration must produce zero edits; unrelated unfixable findings may
remain. No partially transformed file is ever written (§6.7).

Renaming, removing initialisers or AST/control-flow rewrites remain suggestions
only. The formatter never applies lint fixes or changes comment markers. The
supported combined fix pipeline and its test are specified in §8.4 and §9.3.

### 5.6 Reporting

Pretty, JSON, SARIF 2.1.0 and GitHub reporters expose the same ordered diagnostics.
Results are sorted by normalised file path, byte start, owner and code; unlocated
findings follow located findings for their file/module, ordered by code/message.
JSON positions are null when no location is known. Escape
GitHub command properties/messages correctly. SARIF maps byte spans through the
source map and is schema-validated. JSON has a versioned schema including
summary, coverage, diagnostic owner/code, byte spans, one-based display positions,
severity, message and related locations; delegated findings retain original codes.

```text
cookbook/basics/basics.eta:24:5: warning[ETA605]: unreachable cond clause
eta_lint: 0 errors, 1 warning, 0 infos in 1 file
```

That warning is emitted only after ETA605's semantic prerequisites are satisfied;
a textual `(else ...)` match alone is insufficient. `--list-rules` and `--explain`
show analysis level, availability/delegation, prerequisites, examples and fix
safety from the authoritative specifications.

---

## 6) Formatter (`eta_fmt`)

### 6.1 Four independent correctness properties

| Property | Contract | Required verification |
| --- | --- | --- |
| Source fidelity | Lossless CST serialization reproduces all original bytes before layout | Token/trivia partition and byte-equality property tests, including recovered input |
| Semantic preservation | Output has the same reader-level meaning as valid input | Existing compiler reader on both inputs, plus independent token/reader-form guards and controlled compiler/runtime tests |
| Trivia preservation | Comment kind, raw text, order, structural association, datum suppression and required metadata survive | Ordered association records and suppressed-datum structure compared through token correspondence |
| Idempotence | A second pass with the same options is byte-identical | Whole-corpus and generated/adversarial property tests |

Define reader equivalence independently of the formatter and CST bridge: invoke
Eta's existing reader/parser directly with the same initial `ReaderConfig` (§2.1)
on original and output; compare directive/state traces as well as expression trees;
compare typed atoms (including exact integer values and floating-point bit
representations), ordered list elements/dotted tails, vectors/bytevectors,
reader-prefix kind and operand, and module/import/export structures. Ignore only
source positions and explicitly documented trivia fields. Preserve raw concrete
token spellings, delimiters, directive order/scope and suppression structure in
additional checks: the compiler reader discards comments and cannot prove their
association. The CST's own `to_sexpr` is not an independent oracle. Shared lexical
recognisers still create correlated risk, so separately specified reader fixtures
and controlled compile/evaluation comparisons are mandatory (§9.2).

Before a file write or stdout result, require non-recovering input/output reader
validation using the captured `ReaderConfig`,
reader-equivalence, concrete-token preservation and trivia/suppression checks.
Malformed input is status 2; valid but unsupported syntax gets a different status-2
diagnostic. An unexpected mismatch caused by formatter output is an internal
error (70). No unsafe output is returned. Idempotence is an always-on test gate;
it may additionally be checked in diagnostic builds, but need not double every
production rendering operation. Define the complete effective option set as
`o = {width, indent, ReaderConfig, resolved newline policy}` (including LF/CRLF
choice and final-newline preservation). Idempotence means `F(F(s,o),o) = F(s,o)`;
CLI/library/LSP tests use that same complete set, not width alone.

The formatter changes whitespace only. Strings, escaped symbols, numeric
spellings, delimiter kinds, reader prefixes and comment bodies/markers are never
rewritten or reflowed. Width is a layout target: long opaque tokens/comments may
exceed it. There is no project style config or user alias/plugin system in v1;
`--max-width` and `--indent` are explicit invocation options. Encoding/newline
safety and file-selection options are process contracts, not additional styles.

### 6.2 Comment association and blank lines

Use §2.2's single token-owned trivia representation. Derive associations from
original source/token structure before layout and preserve them during rendering:

- A line comment following code on the same physical line remains an end-of-line
  comment anchored to that preceding token. It must force a line break before
  the next active token/closing delimiter, so it cannot swallow emitted syntax.
- A standalone line comment is anchored before the next sibling reader item in
  its enclosing form; if none exists, anchor it before that form's close or EOF.
  Header/shebang comments retain file-header association. Blank-line separators
  distinguish a banner/file section from a statement comment; ambiguous style
  classification does not authorise marker fixes.
- A block comment remains between the same adjacent concrete tokens, inside the
  same enclosing structure. Preserve its complete raw text, nested delimiters
  and internal line endings; do not re-indent its body.
- A datum comment is syntax and retains its suppressed datum and nested reader
  items (§2.3), not a floating comment anchor. Comments/directives inside that
  subtree cannot migrate into active code.
- Lint directives keep their target form/line association. A line-bound directive
  restricts layout as needed; if safe canonical reflow cannot preserve its
  applicability, pass through that enclosing form rather than retargeting it.

Association comparisons use comment identity by ordered original-token
correspondence, kind, exact body/marker text, enclosing structural path and
anchor side. IDs/byte offsets may change between snapshots; association must not.
Count or an unordered text bag is insufficient, especially with repeated comments.

`FormatResult` distinguishes `formatted`, `passthrough` and `refused`. A successful
result has complete output and its effective options; `passthrough` additionally
records ordered `{reason, original ByteSpan}` entries for every opaque/ambiguous
region, even if other regions were formatted. `refused` has diagnostics and no
output. Ambiguous comment association is an explicit passthrough/refusal reason.
CLI reports successful passthrough notes on stderr; `--check` still compares bytes
and may return 0, which does not assert complete canonicalisation of passthrough
regions. LSP reports a concise informational coverage message when a formatting
request leaves such regions unchanged. Release corpus tests allow only reviewed,
recorded passthrough cases; unreported omissions cannot count as supported layout.

Blank lines are preserved from recorded gaps up to one between top-level forms
and inside known `begin`/`module` bodies, zero between ordinary arguments. Do not
remove a blank line required to preserve a banner/directive association, and do
not cap opaque passthrough regions. The CST itself never applies these caps.

### 6.3 Doc IR, layout precedence and safe fallback

```text
Doc ::= Nil | Text(raw) | Line | SoftLine | HardLine
      | Nest(n, Doc) | Align(Doc) | Group(Doc)
      | IfFlat(flat, broken) | Concat(Doc, Doc)
```

`Text` contains raw preserved token text (opaque multiline text uses a verbatim
rendering path). In a flattened group, `Line` is exactly one ASCII space and
`SoftLine` is empty; in a broken group both are one output newline plus current
indentation. `HardLine` is always a newline and prevents flattening a containing
group. `Nest` adds indentation for later breaks, `Align` uses the current output
column, and a group fits only when its flattened contents fit the remaining
width without a hard line. Closing-delimiter placement is explicit in each Doc,
not an implicit renderer heuristic. All whitespace choices must preserve lexical
token boundaries; a separator cannot merge two atoms or detach a reader prefix.

Layout precedence:

1. **Known syntactic shape:** use a dedicated alias after validating its shape.
   A matching head spelling alone is not authority to rearrange structure.
2. **Ordinary structurally certain list:** use the generic rule, including unknown
   macro invocations as reader data; no macro expansion is needed.
3. **Reader-sensitive/uncertain layout:** use a structure-preserving conservative
   layout, or pass through the smallest enclosing safe reader item byte-for-byte.
   Preserve surrounding separator boundaries and report passthrough coverage.
4. **Unsupported/ambiguous reader construct without proven boundaries:** refuse
   the file (status 2) rather than inventing its interpretation.

An opaque item's bytes are stable across repeated passes. A formatter release
must support every reader construct in the Phase-0 inventory, through safe layout
or documented exact passthrough. Layout ignorance does not become a parse error.

**Generic nonempty list without comments or reader constraints:**

```text
Group(open <> head
      <> Nest(indent, concat(Line <> arg for each arg))
      <> close)
```

The first `Line` supplies the separator before the first argument. Flat output
is `(h a b c)`; when broken, every argument begins on its own line at the list's
base indentation plus `indent` (default 2). The closing delimiter follows the
last argument on that argument's final line, including nested groups. Empty
lists print their original delimiter pair; singleton lists have no argument
separator. If a trailing line comment forces the close onto a new line, dedent
that close to the list's base column. These choices get width-boundary and nested
closing-delimiter golden tests. Dotted tails and reader prefixes use dedicated
structural rules preserving every token and operand; they never enter the plain
argument rule by pretending the dot/prefix is an ordinary head.

Initial alias candidates, only where Eta supports and the shape is verified:

| Head / node | Layout |
| --- | --- |
| `defun`, `define`, `lambda`, `let`, `let*`, `letrec*` | Body indent by `indent`; signature/bindings stay on first line if they fit |
| Named `let`, `do` | Verified binder/body layout, otherwise conservative fallback |
| `cond`, `case` | One clause per line, indented by `indent` |
| `if` | Flat if it fits; broken branches indented by `2 * indent` |
| `when`, `unless` | Body indent by `indent` |
| `begin`, `module` body | One form per line; preserve up to one separating blank line |
| `module` head | Head/name followed by import/export/body clauses indented by `indent` |
| `import`, `export` | Inline if it fits; otherwise one syntactic item per line |
| `define-record-type` | Constructor/predicate/field clauses each on their own line |
| Explicit `quote` / `quasiquote` / unquote forms | Preserve explicit list notation; no conversion to shorthand |
| Reader abbreviations | Keep exact prefix spelling and bound operand; never expand to a list |
| Vector/bytevector, bracketed list, dotted list | Preserve original dispatch/delimiter/dot tokens and element order |

The registry is a code-only table. New aliases require preservation, comment
association and idempotence fixtures. Unknown macro bodies/patterns, literal
ellipsis and wildcard spellings are preserved as data. The safe fallback remains
available even when a new special form has no alias.

### 6.4 Worked example

```scheme
(
define
foo
1)
```

The lossless CST first reproduces those exact bytes. Its list has the original
open/close tokens and three atoms. A validated `define` layout fitting width 100
renders `(define foo 1)`, retaining the input's final-newline presence. Brackets,
quote shorthand or suppressed data in analogous input remain their original
reader constructs rather than being normalised to this example.

### 6.5 CLI, modes and exit codes

```text
eta_fmt [paths...]                         # atomic in-place updates; cwd if none
eta_fmt --check [paths...]                 # no writes; 1 only for differences
eta_fmt --diff [paths...]                  # ordered unified diffs; no writes
eta_fmt --stdout <file>                    # exactly one explicit file
eta_fmt --stdin --stdin-filename foo.eta    # stdout unless --check or --diff
eta_fmt --max-width N                      # positive integer; default 100
eta_fmt --indent N                         # positive integer; default 2
eta_fmt --exclude <glob>                   # repeatable
eta_fmt --no-color --jobs N
eta_fmt --version --help
```

`--check`, `--diff` and `--stdout` are mutually exclusive output modes. Stdin
is exclusive with paths and in-place mode; its filename is logical identity,
never a write target. `--stdout` requires one file, not a directory; multi-file
stdout concatenation is invalid usage (64). Buffer a complete validated stdout
result before emitting anything, so input/format failures produce no source
output. Diagnostics go to stderr in all formatter modes. `--diff` may emit
complete diffs for successful files even if another file fails; it never emits
a partial diff for the failing file. `--check` emits no formatted-source output.
There is no `--config` or rule selection.

| Exit | Meaning |
| --- | --- |
| 0 | No differences in check/diff mode, or requested formatting/output completed successfully |
| 1 | `--check` or `--diff` detected changes; never a parse/write failure |
| 2 | At least one file could not be read, parsed, safely formatted or written |
| 64 | Invalid CLI usage/options; no processing/writes start |
| 70 | Internal error, including an output correctness invariant failure |

Preflight usage errors return 64. Processing aggregation is **70 > 2 > 1 > 0**.
Continue independent files after an input failure; stop further writes on an
internal error. Earlier successful file replacements may remain committed:
atomicity is per file, not a multi-file transaction. In-place mode reports 0
when all updates succeed, even when bytes changed; any failed file remains
unchanged on pre-commit failure and contributes 2 or 70. Commit-stage/durability
failures disclose the actual or uncertain outcome (§6.7), rather than claiming
rollback. Output ordering is independent of `--jobs`.

### 6.6 Malformed input, encoding and line endings

Any recovery `Error` node, failure under the selected `ReaderConfig`, or invalid
UTF-8 refuses that
file. Valid but unsupported/ambiguous syntax is reported explicitly as tooling
coverage failure rather than a compiler syntax error. Formatting never invents
missing delimiters or drops an unparsed suffix.

Preserve a BOM only when present at byte zero; never inject one. Preserve uniform
LF or CRLF and the input's final-newline presence. If no newline exists, use LF
for newly introduced structural breaks. **Mixed LF/CRLF or unsupported bare-CR
line endings are refused with status 2 and no output/write** in v1; no silent
normalisation. This check includes newlines inside opaque strings/comments, whose
bytes remain immutable. Format style and safety tests use both LF and CRLF.

### 6.7 Shared file discovery and failure-safe writes

Both CLIs share these process rules; library APIs operate on buffers and do not
write files themselves:

- Expand explicit files and recursively enumerate directories for `.eta` files.
  Include explicitly named files regardless of suffix. Without paths, use cwd.
  Apply include/exclude patterns relative to the invocation/config root (§5.3),
  with `/` separators, `*` for one path component and `**` for recursive matches.
  Explicit files bypass include filters but still obey excludes. Formatter
  defaults exclude `.git`, `out`, `build` and `.eta` directory trees; linter uses
  those defaults plus config/CLI exclusions. No formatter config is discovered.
- Do not traverse directory symlinks/junctions. For read-only operations, explicit
  file symlinks may be resolved; mutating symlink inputs fail with status 2 so
  replacement cannot change a link or an unexpected target. Detectably multiply
  hard-linked files also refuse mutating operations (2) rather than splitting a
  link relationship; read-only operations may deduplicate those aliases. Resolve
  canonical file identity, deduplicate repeated/alias paths, honour platform case
  rules, and sort normalised paths
  lexicographically for stable processing/reporting. Document the chosen first
  display path for aliases. Permission/traversal/read errors are input failures,
  not silently skipped files; zero selected files is a successful no-op.
- Parse, render/fix, validate correctness and prepare the whole replacement buffer
  before opening any destination for replacement. If output is identical, do
  not write, change modification/creation timestamps or require write permission.
  Access-time updates from reading are subject to the OS. A read-only file
  needing a change is an operation error (2), never made writable automatically.
- Record original bytes and identity. Before commit, verify the path still names
  that file and bytes have not changed; stale snapshots fail with status 2.
  Use the platform commit protocol below. Any observed content/identity/permission
  change aborts the replacement; do not claim that path-based rechecks alone close
  every race with an independent writer.
- Write a temporary sibling on the same filesystem; flush and close it, retain
  permissions/ACLs and supported metadata, then use the platform commit protocol
  below for atomic replacement. Modification time may reflect an actual edit;
  preserve ownership,
  permissions, supported creation metadata and extended attributes where the
  platform permits. If required permissions cannot be retained, fail safely.
  Document platform-specific metadata limits; never fall back to truncating the
  original. Where atomic replacement is unavailable, return an operation error.
- Clean up temporary files on failure/cancellation. Recheck original identity and
  permissions before replacing. Pre-commit write/validation/internal failures
  leave the original untouched by the tool; commit-stage outcomes follow the
  protocol below. Do not retain open writable handles
  across unrelated analysis; no partial file transformation is committed.

#### 6.7.1 Platform commit protocols and guarantee limits

V1 supports regular files on tested local filesystems: Windows NTFS and Unix
profiles with proven same-directory atomic rename (initially Linux ext4 and macOS
APFS). The compatibility matrix records OS/filesystem/API support. Unknown,
network or unsupported identity/metadata/replace profiles refuse mutation (2);
read-only operations remain available. No truncating/copy-over fallback exists.

- **Windows:** capture volume/file ID and attributes from a read handle using
  `GetFileInformationByHandleEx`, plus required security/creation metadata. At
  commit, reopen the original with read/security access and `FILE_SHARE_READ |
  FILE_SHARE_DELETE` (without `FILE_SHARE_WRITE`); fail if that handle cannot be
  obtained, identity differs, or reread bytes/permissions changed. Keep it during
  commit to exclude ordinary share-respecting in-place writers. Create a unique
  sibling with `CREATE_NEW`, write/`FlushFileBuffers`/close it, then use
  `ReplaceFileW` without flags ignoring ACL/metadata merge failures. Preserve the
  supported DACL/ownership, creation time and named-stream metadata; attributes
  requiring special privileges cause refusal, not permission weakening. Validate
  the parent/path identity again immediately before the call. Delete sharing is
  necessary for replacement, so a concurrent final-path replacement remains a
  narrow race; the read handle does not make `ReplaceFileW` a compare-and-swap.
- **Unix:** open and retain the parent directory descriptor; read the original
  through `openat(..., O_RDONLY | O_NOFOLLOW)` and capture `fstat` device/inode,
  link count, mode/uid/gid and supported ACL/xattrs. At commit take an advisory
  lock, recheck with `fstatat` plus the opened file and reread original bytes.
  Create the sibling with `openat(..., O_CREAT | O_EXCL)`, apply required metadata,
  write/`fsync`/close, recheck the destination, and `renameat` within that directory.
  `fsync` the directory where supported for crash durability. Parent descriptors
  prevent path redirection but advisory locks protect only cooperating writers;
  `renameat` has no portable destination-identity compare-and-swap. Preserve
  mode/uid/gid and supported ACL/xattrs; birth-time portability is documented,
  not promised. Failure to retain required permissions/ownership refuses commit.

These protocols guarantee complete replacement visibility on the tested profile
and rejection of **observed** stale snapshots. They do not promise universal
race freedom against uncooperative writers replacing the path between the final
check and commit. Known uncertainty (changed/unresolvable identity, permissions,
unsupported profile or a failed required lock/check) returns 2; the documented
residual race is a bounded best-effort guarantee, not silently advertised as
stronger protection. Correctness checks are never weakened to obtain a write.

A pre-commit failure leaves the original unchanged by the tool. After a replace
or durability failure, inspect handles/path/bytes and report `not committed`,
`committed but durability unconfirmed`, or `outcome unknown`, with operation
status 2. Never blindly retry/roll back over an intervening writer or claim the
original is unchanged after a successful rename followed by failed directory
sync. Clean up owned temporary paths only after verifying their identity. Tests
inject concurrent in-place writes/path replacements, permission changes, API
failures and post-commit sync failures on each supported profile and check
reported outcome, atomic visibility, retained metadata and temporary cleanup.

CLI tests cover duplicate paths, links/junctions, deterministic traversal,
read-only unchanged/changed files, concurrent modification, failed replacements,
metadata preservation, commit/durability outcome reporting, and multi-file
partial success with aggregate failure.

---

## 7) Division of responsibility

| Concern | Owner / condition |
| --- | --- |
| Lossless source storage, byte spans, reader/comment structure | Shared `eta_cst` |
| Authoritative parse, module and compiler semantic diagnostics | Compiler/shared service; tooling adapter preserves ownership (§5.1) |
| Whitespace, indentation, safe line breaking and blank-line caps | `eta_fmt` |
| Comment text/order/association, datum suppression, reader spelling | Formatter preservation contract; CST representation |
| Comment-marker conventions | ETA401; fixes only under §5.5 |
| Naming, unused/shadowed bindings, complexity, suspicious execution | Linter rules with proven prerequisites (§5.2) |
| Import sorting | ETA205 only when reordering is proven safe; otherwise no fix |
| Atomic file replacement, traversal, process errors | Shared CLI process contract (§6.7) |
| Editor buffers, scheduling, publication, versioned edits | `eta_lsp` |

Both tools may write files in their explicit modes, but their transformations
remain separate: formatter layout never applies lint fixes, and lint fixes never
substitute for canonical formatting. Compiler ownership is independent of which
CLI reports the diagnostic. Each invocation analyses its current source snapshot.

---

## 8) Integration points

### 8.1 CMake wiring

Add to `eta/core/CMakeLists.txt`:

```cmake
add_library(eta_cst STATIC
    src/eta/cst/source.cpp
    src/eta/cst/token.cpp
    src/eta/cst/trivia.cpp
    src/eta/cst/node.cpp
    src/eta/cst/parse.cpp
    src/eta/cst/bridge_sexpr.cpp)
target_link_libraries(eta_cst PUBLIC eta_core)
target_compile_features(eta_cst PUBLIC cxx_std_23)
target_include_directories(eta_cst PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
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
target_include_directories(eta_linter_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
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
target_include_directories(eta_format_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(eta_formatter
    src/eta/format/main_eta_fmt.cpp
    src/eta/format/cli/args.cpp)
set_target_properties(eta_formatter PROPERTIES
    OUTPUT_NAME eta_fmt
    CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(eta_formatter PRIVATE eta_format_lib)
install(TARGETS eta_formatter RUNTIME DESTINATION bin)
add_subdirectory(tests)
```


All three library targets use the repository's C++23 mode, disabled module
scanning, platform warning policy, runtime-library selection and PIC policy.
Set `CXX_SCAN_FOR_MODULES OFF` and `POSITION_INDEPENDENT_CODE ON` on the static
libraries. Public includes/features propagate through target usage requirements;
implementation-only logging/configuration dependencies stay PRIVATE. Do not
modify global compiler flags or invent a second dependency/toolchain policy.
Declare the shared libraries before wiring their LSP consumers; keep all CLI
entry points out of those libraries and prevent a reverse `eta_core` dependency.
Required runtime dependencies are linked/staged using existing Eta build helpers.

Install `eta_lint` and `eta_fmt` beside existing executables, with their required
runtime dependencies, documentation and bundled rule metadata if runtime lookup
requires it. A release installation must run `--list-rules` / `--explain` without
access to the source checkout. `eta_cst` and the two tool libraries are internal
implementation APIs in v1; no standalone headers/archive SDK or stable C++ ABI
is promised. The stable surfaces are CLI behaviour, diagnostic schemas, rule
codes and documented style. API exposure can be reviewed separately after v1.

### 8.2 LSP ownership, document lifecycle and scheduling

`eta_lsp` is the sole owner of editor diagnostics and formatting. It links the
libraries directly; it never shells out to `eta_lint` / `eta_fmt` for requests:

```cmake
target_link_libraries(eta_lsp PRIVATE eta_linter_lib eta_format_lib)
```

The language client uses the server's advertised diagnostic/full/range formatting
capabilities. The same options, source maps, rule specs, safety validation and
failure behaviour apply to CLI and server libraries.

| Event / request | Required behaviour |
| --- | --- |
| `didOpen` | Store full immutable text/version, parse and schedule fast plus complete analysis |
| `didChange` | Apply ordered incremental changes using negotiated positions, advance version, cancel old work and schedule fresh analysis |
| `didSave` | Refresh complete analysis and dependent module metadata against the current snapshot; optional supplied text must agree with that snapshot |
| `didClose` | Cancel document work, clear published diagnostics and release buffer/snapshot references |
| Full formatting | Validate a captured snapshot; return a complete edit only if it is still current |
| Range formatting | Use the strict edit boundary in §8.2.1; no speculative expansion |
| Cancellation / newer version | Discard obsolete results; never publish or apply old-version edits |

Advertise incremental text synchronisation (`TextDocumentSyncKind.Incremental`),
while accepting a full replacement change when the client sends one. Apply a
notification's changes sequentially to the preceding version, never to a stale
buffer. Invalid position/version sequences cause explicit synchronisation
handling and cleared stale diagnostics, not guessed patching. An initial full
CST reparse per snapshot is acceptable; incremental protocol support does not
promise incremental parsing. Cache facts by content hash, reader/config version
and module dependency state; invalidate dependent documents on changed metadata.

Classify rules by measured cost as well as analysis prerequisites. Debounce fast
work after 200 ms of inactivity and complete work after 750 ms of inactivity;
complete analysis also runs on open/save. These defaults are benchmarked in §9.5,
not a claim that every keystroke completes linting synchronously. Each document
has its own pending version/cancellation token; concurrency is bounded and fair
across multiple open documents. Expensive analysis is cancellable and does not
block the request thread.

On a change, remove findings derived from the old version immediately. Publish
new-version fast/compiler-reader results as one current snapshot set, then replace
that set with the complete current-version results when ready. Do not retain
old slow-rule findings until save. Syntax errors clear unavailable scope/semantic
findings while preserving current reader and safe L0 results. A single publisher
merges owners, includes the document version, and checks version/config/dependency
currency before publication. Cancellation is checked during scanning, analysis,
layout and rendering; obsolete formatting returns the appropriate LSP
`ContentModified` or cancellation response and no edits.

Full formatting returns a full-document replacement (or empty edits when already
canonical), using negotiated LSP positions. Formatting options map `tabSize` to
`indent`; tabs are unsupported by the canonical space style, so `insertSpaces=false`
is rejected explicitly. `eta.format.maxWidth` supplies width; document-format
options take precedence over editor defaults where applicable. Parse failure
or a `refused` result produces a clear request failure and no edits; current
syntax diagnostics are published through the diagnostic channel. A successful
`passthrough` result may return validated edits for other regions, with an
informational coverage message as defined in §6.2. Formatting never writes the
file from the server; the client applies returned edits.

#### 8.2.1 Range edit boundary

V1 deliberately keeps strict range formatting: edits remain **only inside the
requested half-open range**. It never expands a partial selection to its enclosing
form. Use this selection/span algorithm against one immutable full-document CST:

1. Convert the request to byte boundaries with §2.4's source map. Enumerate root
   reader units, excluding BOM/shebang/header and stateful directive tokens from
   rewriting. Prefixes and datum-comment children belong to their structural unit.
2. For each unit, collect its concrete syntax and every uniquely associated ordinary
   comment. Its replaceable span is the minimal contiguous envelope of those raw
   slices, not its next token's entire leading-trivia gap. Reject the candidate if
   that envelope includes a neighbour's syntax/comment, an ambiguous association,
   or a partial prefix/comment. No shared separator is assigned to both units.
3. Leave leading outer indentation, inter-unit whitespace and trailing terminators
   outside that envelope byte-identical. Newlines inside the envelope may be
   laid out; an adjacent line-comment terminator outside it stays in the untouched
   suffix. If safe rendering would require consuming/changing that terminator,
   extending a prefix, or changing a neighbouring separator, skip the unit.
4. Select only envelopes wholly within the requested range. Preserve the exact
   outside indentation bytes and render from the original starting column; if
   that base cannot be represented safely, return no edit for the unit. Do not
   append a final newline. Use the unit's whole-document reader-state context,
   never reparse a state-sensitive slice with fresh default flags.
5. Reject overlapping candidate envelopes. Apply the remaining non-overlapping
   replacements to a copy of the snapshot and validate the **whole document**
   with the same initial `ReaderConfig`, token/comment associations and state
   trace. If the combined result fails, return no edits, not a subset selected
   by trial and error. Recheck document version before returning.

Whitespace-only and partial-form/comment/prefix selections yield no edits for
those units; complete adjacent units contained in the request may still qualify.
Malformed documents are refused. When no safe span can be proven, return no edits
with an explanatory coverage reason. Tests assert containment, no detached
comments/prefixes, unchanged bytes outside every edit and full-document correctness,
including EOF comments, CRLF, shared separators and reader-state boundaries.
More expansive selection formatting is outside v1 and requires user evidence
and a separate product-contract review, not a silent fallback.

### 8.3 VS Code extension

Use the language client's LSP formatting providers rather than registering a
second external-executable formatting path. Settings are `eta.lint.enabled`
(default true), `eta.format.maxWidth` (100), and `eta.format.indent` (2); the
extension's configured server path selects `eta_lsp`. Use VS Code's standard
`editor.formatOnSave`, not a separate competing save hook. Neither
`eta.lint.path` nor `eta.format.path` controls editor requests. Server failure is
visible to the user; there is no silent fallback to a standalone executable.

Explicit workspace tasks remain separate CLI operations:

- `Eta: Lint workspace`: `eta_lint --format pretty .`, with `$eta-lint` matching
  `file:line:col: severity[CODE]: message` (including namespaced compiler codes).
- `Eta: Format workspace`: `eta_fmt .`, an explicit workspace rewrite task.

CLI task executable locations may be configured in `tasks.json`; they have no
precedence over LSP requests. CI uses the GitHub reporter separately.

### 8.4 Linter/formatter workflow

The binaries never invoke each other. The recommended read-only checks are:

```text
eta_fmt --check stdlib cookbook demo
eta_lint --max-warnings 0 stdlib cookbook
```

The supported combined write pipeline is:

```text
eta_lint --fix [paths...]
eta_fmt [paths...]
```

Run the second command only when the first completed successfully. Each command
reparses the bytes it receives; there is no shared stale-span claim. This order
ensures final whitespace is canonical after approved marker/import edits. The
reverse sequence is safe as two standalone invocations but is **not a supported
canonicalisation pipeline** and may require a final formatting pass. Tests do
not promise the reverse order leaves formatter-clean output (§9.3).

### 8.5 CI, pre-commit and bootstrap

Add `.github/workflows/lint.yml` with `format-check` building `eta_formatter`
and running `eta_fmt --check stdlib cookbook demo`, and `lint-check` building
`eta_linter` and running `eta_lint --max-warnings 0 --format github stdlib cookbook`.
Enable canonical-byte checks after the Phase-4 bootstrap; preservation tests over
all three corpora already gate Phase 3, before a mass rewrite.

`scripts/pre-commit-eta.sh` checks staged `.eta` **index bytes** using the stdin
contracts, rather than inspecting unrelated unstaged working-tree content. It
performs formatter then linter checks without fixing or staging files and handles
quoted paths/deletions. Non-zero blocks the commit; document Windows invocation
support. Rule/module context uses the staged filename and the documented index
snapshot, never a mixture of staged and unstaged source facts.

After Phase-3 gates and style review, land a separate formatting-only commit
`chore: eta_fmt-bootstrap` for `stdlib`, `cookbook`, and `demo`. Do not combine
lint fixes or semantic changes with it. Record its hash in `docs/eta_fmt.md` and
`.git-blame-ignore-revs`. CI keeps those corpora formatter-clean thereafter;
review any lint cleanup/configuration separately.

---

## 9) Testing and verification

### 9.1 CST/reader and linter tests

Source fidelity is a permanent property suite over `stdlib`, `cookbook`, `demo`,
reader fixtures, generated syntax and recovered malformed inputs. Verify exact
byte reconstruction and that each concrete byte is owned once. Cover every
supported delimiter, dotted list, abbreviation, dispatch form, escape, numeric
spelling, escaped identifier, UTF-8 character, BOM/shebang/directive, ordinary
line comment, nested block comment and datum-comment consumption case. Include
prefixes/comments at EOF, nested discarded datums, repeated identical comments,
invalid UTF-8, unterminated strings/comments, mismatched closes and preservation
of the remaining suffix. Reader-mode/differential fixtures are mandatory for
all constructs recorded in Phase 0, not postponed to hardening. Header cases cover
BOM present/absent, BOM-only input, shebangs with/without final newlines, and
comments before the first active form, checking ownership with no gaps/duplicates.
State-trace fixtures place directives before/between/inside forms and nested datum
comments, exercise both supported quasiquote modes, and compare initial/final
state plus every directive event through token correspondence.

Each enabled rule has specification-linked positive, negative and unknown-evidence
fixtures with expected owner/code/severity/spans/coverage. Scope/semantic fixtures
cover nested scopes, sequential/recursive/multiple-value bindings, supported
parameter forms, builtin rebinding, intentional shadowing, quotes/quasiquotes,
macro uncertainty, imported syntax and module boundaries. False-positive tests
are gates, not optional examples. Delegated diagnostics get adapter/deduplication
tests and keep their original code/severity. Spec schema/fixture validation and clean regeneration of registry, preset sets,
rule docs and fixture inventory run in CTest/CI. Outputs are byte-reproducible
from the checked-in inputs, and gated/delegated detectors cannot enter presets.

Fix-capable rules cover strings, ordinary/block/datum comments, directive
attachments, LF/CRLF, competing/duplicate/overlapping edits, stale expected bytes,
UTF-8 boundaries, invalid replacement syntax and an internal failure before
commit. A second fix pass produces zero edits for the same configuration; it
need not eliminate unrelated unfixable findings. Import-sort fixtures prove
safe ordering and explicitly withhold fixes for side effects, unknown imports,
syntax dependencies and cycles. Regressions on `stdlib` and `cookbook` compare
approved expected findings/configuration without treating skipped semantic
coverage as proof of cleanliness.

### 9.2 Formatter properties and independent semantics

- Unit-test Doc IR flattening/breaks, exact head-to-first-argument separators,
  indentation, closing delimiters, nested groups, opaque multiline text and
  lexical separators at widths 100, 80, 40 and 20, including exact fit boundaries.
- Give each verified alias/fallback short/long/commented/reader-sensitive golden
  variants. Golden updates (`--bless`) require review; snapshots never substitute
  for independent correctness properties. Maintain a reviewed matrix by reader
  construct, comment relationship, alias/fallback, header/encoding and line-ending
  mode, with required boundary/pairwise/adversarial combinations. Missing matrix
  cells block coverage regardless of fixture count.
- `eta_cst_source_fidelity`: byte equality/partition on all three corpora plus
  generated valid and recovered inputs. Lossless serialization runs before layout.
- `eta_fmt_roundtrip`: original and formatted text pass the existing compiler
  reader directly with the same initial `ReaderConfig` and satisfy §6.1's
  structural equivalence; compare full directive/state traces, raw tokens and
  datum suppression independently too.
  The formatter's bridge is not the comparator.
- `eta_fmt_comment_association`: compare ordered comment records, kinds, raw text,
  enclosing paths, anchor sides and suppressed-datum relationships through token
  correspondence. Repeated identical comments must still have distinct identities.
- `eta_fmt_idempotent`: assert `F(F(s,o),o) = F(s,o)` for complete effective
  options (width, indentation, reader configuration and resolved newline policy)
  over `stdlib`, `cookbook`, `demo` and adversarial fixtures. Also run the actual
  CLI twice on temporary corpus files with identical options: the second pass
  performs zero writes and preserves modification/creation times.
- Passthrough fixtures assert outcome, reason and original byte ranges; full and
  range LSP responses expose coverage without inventing error diagnostics. Tests
  distinguish fully formatted, safely copied regions and refused documents.
- Trusted compiler/runtime fixtures compile or evaluate original and formatted
  programs in a controlled harness and compare exit status, values, stdout/stderr
  and specified observable effects. Cover reader forms, macros, modules and
  binding/equality/control-flow cases. Fix seeds, bound time/resources and isolate
  filesystem/network effects. These tests supplement reader comparisons; general
  execution equivalence is neither decidable nor asserted by fixture success.
- Malformed, unsupported or mixed-line-ending input returns its specified failure
  and no output/write; deliberate renderer corruption must trip the independent
  safety checks rather than create a plausible but changed program.

Fuzz CST, formatter and fix engine via `qa/fuzz` with grammar-aware and arbitrary
byte inputs. Within resource limits, results must be either correct validated
output or a diagnostic, never a crash, hang, source loss or unsafe write. A shared
lexer still risks correlated bugs: keep independently specified expected-reader
fixtures and regression cases for every discovered discrepancy.

### 9.3 Combined fix/format workflow

Use a recorded representative selection of 50 cookbook files plus adversarial
comment/import fixtures. For the supported `eta_lint --fix` then `eta_fmt`
pipeline, assert semantic preservation, documented marker changes only, final
formatter cleanliness, expected lint findings and a second pipeline with zero
edits/byte changes. Every command reparses its own current input. No cross-process
span cache is reused.

The reverse ordering is not a promised canonicalisation pipeline. Test individual
invocation safety, but do not assert reverse-order formatter cleanliness or use
it as evidence of the supported workflow. A final `eta_fmt` is required when a
later lint fix changes canonical layout. A failed first command stops the pipeline.

### 9.4 CLI, filesystem and LSP integration

CLI tests assert every exit code and aggregation precedence under different
worker schedules: one malformed file plus another with warnings/differences,
internal errors, failed reads/writes, read-only unchanged/changed files, invalid
configuration/directives, empty selections and partial multi-file success.
Verify atomic replacement, retained permissions/metadata, temporary cleanup,
concurrent modification, links/junctions/hard-link deduplication, exclusions,
recursive traversal, stable diagnostic/diff ordering, staged-index hook behaviour,
and stdout/stderr exclusivity. Multi-file `--stdout` is usage failure; a failed
single stdout input emits no source bytes. Cover Windows and Unix semantics.

LSP tests drive open/change/save/close, sequential incremental edits, negotiated
UTF-16/UTF-8 conversion, invalid versions, syntax-error clearing, config/module
invalidation, multiple documents, cancellation and stale result suppression.
Full/range edits pass the same preservation checks; range tests assert byte
identity outside all edit spans for whitespace/comment/prefix/partial-form ranges.
Client formatting failures never fall back to subprocesses. Installed-bundle
smoke tests run the CLI docs/rule lookup without a source checkout.

### 9.5 Reproducible performance and resource gates

Before Phase 1, commit a benchmark manifest containing corpus revision/content
hashes, exact files/options/presets, fixture generator seed and an initial reader
baseline. Record formatter/rule baselines as their first implementations become
available in Phases 1/2; they cannot be measured before those libraries exist.
Include ordinary `stdlib`/`cookbook`/`demo` files binned as small (≤4 KiB), medium
(>4–64 KiB), large (>64 KiB–1 MiB), plus 1 MiB comment-heavy fixtures and nested
forms up to the supported depth. Report each bin separately; an overall average
cannot hide large-file behaviour.

Use a documented reference machine (CPU model/core count, RAM, OS), exact compiler
and CMake settings, Release optimisation with assertions/sanitizers documented,
`--jobs 1`, and a fixed config/reader mode. Run 10 warm-up and 100 measured
iterations; report median, p95, throughput and peak memory. Separate cold/warm
process/I/O measurements from in-memory library timings. Linter reports scanning/
parsing, scope/module analysis and per-rule execution separately; formatter timing
includes required reader/token/trivia validation. Record the timer, aggregation
method and cache state. CI on different hardware is informational, not a direct
comparison to the reference machine.

On the fixed small-file corpus/reference machine, initial targets are **p95
formatter library time <2 ms/file** and **p95 rule execution <1 ms/file/rule**;
parsing/shared analysis are reported separately, never charged invisibly to every
rule. Phase 0 fixes the measurement method/reference environment and initial
budgets; Phases 1/2 record the first library measurements. Phase 5 must meet the
recorded release budgets, with any target revision explicitly reviewed and
documented rather than silently changing corpus or hardware. Correctness
validation/shared-analysis costs remain included and visible; report passthrough
coverage alongside timings rather than using unformatted regions to meet budgets.

Default per-document limits are 16 MiB input, 32 MiB formatted output, nesting
depth 256, 1,000,000 concrete tokens/nodes each, and 256 MiB tooling-owned memory.
Check limits before invoking recursive reader/layout operations and while building
arenas/docs. Exceeding a limit produces a resource diagnostic/status 2, not
truncated output or a guessed tree. Shared library APIs use the same documented
limits and cancellation token. Test just below/above limits, comment-heavy and
malicious nesting, and cleanup of cancelled snapshots. The reference LSP stress
test requires p95 cancellation response ≤50 ms, with bounded worker concurrency
and no publication after cancellation. Include peak memory and cancellation
latency in the recorded benchmark report.

---

## 10) Documentation deliverables

1. `docs/eta_lint.md`: installation, analysis/coverage limitations, configuration
   precedence, exact preset sets, directives, CLI/reporters, exit aggregation,
   file handling and safe-fix workflow.
2. `docs/eta_lint_rules.md`: generated from `rules/<CODE>.toml` and referenced
   fixtures (§5.2), including delegated/gated status, prerequisites, negative
   examples and fix-safety conditions. Generator/schema checks run in CI.
3. `docs/eta_fmt.md`: CLI/modes/exits, source and preservation guarantees,
   passthrough/unsupported behaviour, line endings/encoding, atomic writes,
   LSP ownership/range limits, bootstrap hash and the distinction between no
   project style config and supported invocation options.
4. `docs/eta_fmt_style.md`: exact Doc break/closing-delimiter rules, aliases,
   conservative fallbacks and before/after examples; review before bootstrap
   and freeze canonical output rules at 1.0.
5. Architecture appendix and tool READMEs: CST ownership/lifetime/reader inventory,
   diagnostic ownership, build dependencies, internal library API status,
   adding a rule/alias and debugging preserved-source/layout failures.
6. `editors/vscode/README.md`: LSP settings, standard format-on-save, lifecycle
   diagnostics, cancellation, strict range boundaries and explicit CLI tasks.
7. Benchmark manifest/report and compatibility matrix: machine/build/corpus,
   timing breakdown, memory/depth/cancellation limits, supported platforms and
   reader constructs. Document deferred rule prerequisites and metadata limits.

---

## 11) Phased delivery and gates

A later phase cannot compensate for a failed earlier safety gate. The MVP does
not gain extra rules or editor features to work around uncertain semantics.

### Phase 0 — Shared foundation

**Status: complete (2026-10-11).** The Windows MSVC Release `eta_all` build
and all 15 CTest targets pass. The strengthened CST differential suite passes
10 cases and 14,229 assertions, including all 191 present corpus files,
supported reader constructs, malformed recovery and seeded arbitrary bytes.
The [recorded baseline](../benchmarks/tooling_baseline.json) includes all size
bins, memory accounting and cancellation measurements; maximum cancellation
p95 is 1.9866 ms against the 50 ms gate. Clang/libFuzzer execution remains a
platform-specific validation task; its target is wired, while the deterministic
property suite runs on MSVC.

Implementation and audit: [lossless reader foundation](../cst_reader.md),
[native diagnostic inventory](../tooling_diagnostics.md),
[benchmark manifest](../benchmarks/tooling_manifest.json), and
[rule schema](../../eta/tools/linter/rules/schema.json).
The audit records compiler BOM rejection and incomplete UTF-8 validation in
comments/bar identifiers; these inputs retain all bytes and remain ineligible
for formatting. The current checkout has no `demo` source directory, which the
manifest records explicitly.

1. Audit and document actual reader/module/binding behaviour with source references
   and independent fixtures. Record selected reader configurations/state traces and
   unsupported constructs.
2. Implement the shared lossless scanning view, immutable source/token/trivia/CST
   representation, structural datum comments/reader prefixes, spans/source maps,
   tolerant recovery and fallible S-expression bridge (§2).
3. Establish the per-kind diagnostic/location/exit adapter inventory and rule-spec
   schema. Record
   resource limits and the reproducible benchmark manifest/baseline (§9.5).

**Gate:** lossless serialization and unique byte ownership pass on **all of
`stdlib`, `cookbook`, `demo`**, every supported reader construct and malformed
recovery/adversarial fixtures. Reader-view differential tests, datum consumption,
comment association, exact BOM/shebang ownership, reader-state trace agreement,
Unicode position conversion and limit handling pass.
Unresolved reader/CST discrepancies block formatting those constructs. Phase 1
starts only after these contracts are demonstrated, not merely sketched.

### Phase 1 — Formatter MVP

1. Doc IR/rendering, exact generic layout, safe fallback and reader-sensitive rules;
   only a small verified alias set (`define`, `lambda`, ordinary `let`).
2. CLI stdin/stdout/check/diff/in-place, width/indent, deterministic failure statuses
   and safe writes. Keep the existing formatter MVP surface.
3. Seed golden fixtures plus independent reader/token/trivia preservation and
   idempotence suites, including suppressed data and unknown macro syntax.

**Gate:** representative basics and all reader fixtures format safely and
idempotently; malformed/unsupported input never writes, and unknown forms have a
proven fallback. Test the formatted copy with `--check`, not an assumption that
original fixtures were already canonical. No correctness reliance on the CST
bridge alone.

### Phase 2 — Linter MVP

1. Engine/registry/specs, diagnostic adapter/coverage, pretty/JSON CLI and exit codes.
2. Seed L0 rules ETA104, ETA203, ETA401 and ETA501. Add the straightforward ETA301
   scope-aware rule only after ordinary-let binding/reference semantics are proven;
   otherwise defer it visibly rather than inventing a replacement semantic rule.
3. Positive/negative/unknown cases, scope/macro uncertainty tests, tool README
   and linter guide skeleton. Advanced module/type/control-flow rules stay gated.

**Gate:** fixtures report justified findings with correct ownership/spans, no
false positives in their negative/unknown cases, and deterministic multi-file
statuses. A missing semantic service produces recorded coverage limitations.
An MVP may ship the four verified seed rules while ETA301 remains gated.

### Phase 3 — Verified coverage, configuration and fixes

1. Add supported form aliases and a reviewed coverage matrix (about 80 golden
   fixtures is a planning target, not a release quota). Verify
   fidelity, independent reader semantics, token/comment/suppression preservation
   and idempotence across **all of `stdlib`, `cookbook`, `demo`** before bootstrap.
2. Add remaining linter rules only as their metadata/semantic prerequisites pass;
   configuration/presets, directives, SARIF/GitHub, rule listing/explanation.
3. Validate ETA401 fixes; enable ETA205 sorting only where reordering is proven
   safe. Complete fix-conflict, filesystem and supported-pipeline tests.

**Gate:** all corpus properties and filesystem/CLI/spec-schema tests pass; a
formatted copy of every corpus is `--check` clean. Enabled rules have positive,
negative and unavailable-evidence fixtures; required coverage-matrix cells and
artifact-regeneration checks pass; gated/delegated entries are documented
and excluded from implemented-rule counts. Lint corpus expectations/configuration
are reviewed. No unproven rule or fix is required just to satisfy a numeric quota.

### Phase 4 — Editor, CI and separate bootstrap

1. LSP direct-library integration, lifecycle/versioned diagnostics, debounce/idle
   analysis, cancellation, concurrent documents and strict range edit boundaries.
2. VS Code LSP settings/tasks and standard format-on-save; no subprocess fallback.
3. Review style guide, then land a separate formatter-only bootstrap of `stdlib`,
   `cookbook`, `demo`; record blame-ignore hash. Keep lint fixes separate.
4. Enable full-corpus format/lint CI and staged-index pre-commit checks; review
   lint configuration/cleanup separately to keep `stdlib`/`cookbook` clean.

**Gate:** LSP integration and staged-file tests pass; actual checked-in corpora
are formatter-clean and approved lint configuration meets the documented CI
threshold. Preservation coverage on stdlib was already established in Phase 3.

### Phase 5 — Hardening and 1.0

1. Extend fuzz/adversarial reader, fix, filesystem and concurrent-editor coverage;
   foundational datum/comment/reader correctness is already required in Phase 0/1.
2. Meet documented performance, memory/depth and cancellation budgets; review
   cross-platform safe-write/metadata compatibility and installed bundles.
3. Stabilise CLI/report schemas, rule-code meanings and style; review internal
   library APIs without promising a public C++ SDK/ABI.
4. Finish docs and publish enabled/delegated/deferred rule and reader coverage.

**Gate:** every acceptance criterion in §13 passes. Semantic features that remain
gated are explicitly outside the released rule set; they cannot be advertised
as implemented or enabled by a preset. No release gate silently substitutes
fixture count, successful bootstrap or rule count for preservation evidence.

---

## 12) Risks and explicit decisions

| Risk | Decision / mitigation |
| --- | --- |
| Lossless scanner drifts from compiler reader | Shared lexical recognisers, explicit scanning views, independent expected-reader/differential fixtures; disagreements block coverage |
| Shared parser assumptions hide formatter bugs | Existing-reader oracle plus raw-token/suppression guards and controlled semantic harness (§6.1, §9.2) |
| Comments/reader prefixes change context | Unique trivia ownership and preserved structural associations; raw fallback or refusal |
| Macro/module uncertainty creates false positives | Per-rule prerequisites, unknown scopes, non-executing metadata and visible skipped coverage |
| Import sorting changes initialisation | No automatic reordering without an Eta-specific safety proof; conditional ETA205 only |
| Editor diagnostics/edits race document changes | Immutable versioned snapshots, cancellation, one publisher, stale-result rejection |
| CLI mutation damages files | Whole-buffer validation, per-file atomic replace, unchanged-file no-op and concurrent-change checks |
| Bootstrap diff obscures language changes | Separate formatting-only commit after all-corpus preservation gates; recorded ignore-rev |
| Performance targets are irreproducible | Fixed corpus/machine/build manifest with p95 and cost breakdown before implementation |
| Internal library API freeze exceeds scope | V1 supports CLI/report/style contracts; CST/tool library C++ APIs remain internal |

CRLF/BOM and mixed endings follow §6.6; no contradictory normalisation path.
`.eta-lint.toml` is the fixed v1 config location; the formatter has invocation
options but no project style file. Reader forms, nested comments and suppressed
data are early safety requirements, not late polish. Only the fix-then-format
pipeline promises final canonical bytes. Large generated notebook files can be
excluded explicitly; exclusions are recorded in corpus/benchmark manifests and
cannot silently remove difficult cases from acceptance.

Published rule codes retain their meaning. A name-only documentation change may
retain its code; a semantic change allocates a new code and deprecates the old
one. Deferred entries have explicit prerequisites/status, not speculative defaults.
Canonical style changes after 1.0 require a reviewed compatibility/version policy.

---

## 13) Combined v1 acceptance criteria

All gates below are required; Phase-0/3 corpus verification is not delayed until
the formatter bootstrap. The initial candidate catalogue does not impose a
minimum number of unproven rules.

- [ ] CMake builds targets `eta_linter` / `eta_formatter` producing `eta_lint` /
      `eta_fmt`; installed executables and runtime dependencies work without a
      checkout. Three internal static libraries exist; LSP links the tool libraries.
- [x] Reader inventory/modes, lossless tokens and unique trivia ownership,
      datum-comment/prefix structure, spans, lifetimes and recovery are documented
      and tested, including BOM/shebang byte ownership, selected reader configurations
      and directive/state traces, plus adversarial/malformed inputs within limits.
- [ ] Source fidelity, independently checked reader semantics, concrete-token and
      ordered comment/suppression association preservation, and byte idempotence
      pass over **all of `stdlib`, `cookbook`, `demo`**, reader fixtures and fuzz cases.
- [ ] Every supported alias and conservative fallback has golden/property coverage;
      malformed/unsupported/mixed-line-ending input produces no unsafe output/write.
      Valid unknown macro syntax is preserved without expansion. Coverage-matrix
      cells pass; passthrough/refused outcomes, reasons and spans are exposed.
- [ ] Every enabled rule has a validated authoritative specification, analysis
      prerequisites, positive/negative/unknown cases, exact spans and documented
      fix safety. Gated/delegated rules are visible and not counted as implemented.
- [ ] Compiler/CST/linter diagnostic ownership, original codes/severity translation,
      recovery distinction and deduplication pass adapter tests; unknown semantics
      cause coverage limits, not fabricated findings. Per-kind location/exit mappings
      cover unlocated/module/global/unknown diagnostics without silent loss.
- [ ] Linter pretty/JSON/SARIF/GitHub output is deterministic and conformant; SARIF
      and versioned JSON are validated. CLI help/version/list/explain work.
- [ ] Configuration discovery/precedence/preset sets and directives are deterministic;
      invalid codes/options/severities/directives have the specified failures.
- [ ] Exit codes and **70 > 2 > 1 > 0** processing aggregation pass multi-file tests;
      formatter code 1 is differences only. Usage/config preflight is 64.
- [ ] Discovery/exclusions/duplicates/links, single stdout input, read-only inputs,
      unchanged no-op, failed/stale writes, atomic replace, metadata and temporary
      cleanup and commit/durability outcome reporting meet §6.7.1 on tested
      Windows/Unix profiles without claiming universal concurrent-writer race freedom.
- [ ] ETA401 preserves comment bodies/newlines/association; ETA205 offers fixes only
      for proven-safe groups. Validated non-overlapping source edits never commit
      partial transformations; a second fix pass produces no edits.
- [ ] Supported fix-then-format pipeline is semantically safe, formatter-clean and
      idempotent; reverse ordering is not advertised as canonicalisation.
- [ ] LSP open/change/save/close, incremental text sync, negotiated position encoding,
      stale/cancelled result suppression and concurrent document tests pass.
      Range edits remain inside the requested range and preserve all outside bytes.
- [ ] VS Code uses one LSP ownership path for diagnostics/full/range formatting,
      with standard format-on-save and explicit standalone CLI tasks only.
- [ ] A separate formatter bootstrap commit is recorded. CI passes
      `eta_fmt --check stdlib cookbook demo` and
      `eta_lint --max-warnings 0 stdlib cookbook` under the reviewed release config;
      staged-index pre-commit checks use the same contracts.
- [ ] User/rule/style/editor/build/API-ownership documentation and generated rule
      catalogue are present and agree with the released enabled/deferred set. Registry,
      presets, rule docs and fixture inventory regenerate reproducibly from specs.
- [ ] Reproducible benchmark manifest/report and approved p95 release budgets pass
      on the documented reference corpus/machine; parse/analysis/rule/render costs
      are distinguished. Memory, source/output/depth limits and cancellation gates pass.
- [ ] V1 retains the two executables, shared lossless CST and internal libraries,
      CLI-only width/indent style options, conservative fixes and phased scope;
      no formatter project config, macro execution or public C++ ABI is implied.
