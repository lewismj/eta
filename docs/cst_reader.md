# Lossless reader foundation (Phase 0)

`eta_cst` is an internal static library, built by `eta_all` and linked to
`eta_core`. It implements immutable snapshots, source maps, concrete reader
items, tokens/trivia, structural nodes, recovery, diagnostic adapters and the
S-expression bridge. It is not installed as a public SDK. No CST operation
executes macros, loads modules or discovers native sidecars.

The authoritative contract is [the lint/format plan](plan/lint_formatter_plan.md).
Internal APIs are [parse.h](../eta/core/src/eta/cst/parse.h),
[source.h](../eta/core/src/eta/cst/source.h),
[bridge_sexpr.h](../eta/core/src/eta/cst/bridge_sexpr.h) and
[diagnostic.h](../eta/core/src/eta/cst/diagnostic.h).

## Actual reader inventory

| Construct | Source recogniser / independent fixture |
| --- | --- |
| Parenthesised/bracketed lists and dotted tails | `Parser::parse_list`; `reader_inventory`, malformed delimiter/dot cases |
| `#(...)`, `#u8(...)`, integer bytes 0 through 255 | `Lexer::consume_sharp`, `Parser::parse_vector/parse_byte_vector`; radix/invalid-byte cases |
| Quote, backtick, comma, comma-at | `Parser::parse_abbreviation`; strict/non-strict and nested state cases |
| Case-insensitive short/long booleans | `Lexer::consume_boolean`; `#t`, `#TRUE`, `#false` |
| Character names, delimiter characters, semicolon-terminated hex scalars | `Lexer::consume_char/consume_hex_until_semicolon`; space/non-BMP cases |
| String escapes `a b t n r " \\ x...;` | `Lexer::consume_string`; unknown escapes and literal LF rejected |
| Bar identifiers, escaped characters and hex escapes | `Lexer::consume_bar_identifier`; raw and decoded identity retained independently |
| Bare identifiers, `+`, `-`, `...`, special `Nil` | `Lexer::consume_bare_identifier_or_number`, `Parser::parse_atom` |
| Decimal integer/float/exponent, radix and exactness prefixes, infinity/NaN | Shared numeric recognisers; at most two prefixes; exactness creates no rational/complex type; decimal integer overflow may become double |
| Line/nested block comments | `Lexer::next_raw_item/skip_block_comment`; exact trivia ownership |
| Nested datum comments | `Lexer::skip_datum`; `#; #; a b c` leaves only `c` active |
| Directives and first-line shebang | `Lexer::consume_directive_or_shebang`; fold-case/no-fold-case and non-shebang directives remain no-ops |
| Top-level module recognition | `Parser::recognize_module`; shared normalization to `ModuleForm` |

Source definitions are [lexer.cpp](../eta/core/src/eta/reader/lexer.cpp) and
[parser.cpp](../eta/core/src/eta/reader/parser.cpp); independent fixtures are in
[cst_tests.cpp](../eta/qa/test/src/cst_tests.cpp), plus the existing lexer/parser tests.
`next_raw_item()` exposes the existing lexical recognisers. `scan_next()` observes
every item consumed by `next_token()`, including the exact end of each discard.
Compiler clients keep the same skip-trivia token stream.

Discarded syntax follows the existing permissive discard routine: dot placement,
non-byte bytevector elements and some mismatched nested delimiters are accepted
there although active parsing rejects them. These regions retain concrete
children and are marked verbatim. Lexer failure preserves the unscanned suffix
as an error region through EOF. Structural recovery records missing delimiters
as virtual zero-width insertions and resumes at proven sibling boundaries.
Unsupported Scheme dispatch forms, datum labels, rational/complex numbers and
syntax abbreviations gain no second tooling grammar; reader errors remain authoritative.

Two reader limitations block formatting and are explicitly tested:

- The existing compiler lexer rejects an initial UTF-8 BOM. The CST still owns
  exactly `[0,3)` as `FileBom`, including BOM-only input, and retains the reader
  rejection. It never strips the header before validation.
- The existing compiler does not validate UTF-8 in every comment/bar identifier.
  Compiler-accepted invalid bytes receive a separate CST unsupported diagnostic,
  not a claimed compiler rejection. All bytes remain preserved.

Changing those acceptance policies belongs to the shared language reader.

## Configuration, traces and compiler facts

`ReaderConfig` explicitly selects `EtaCurrent` and `strict_quasiquote`. There
are no other configurable lexical flags in the current reader. Every fresh
pass starts at BOF. [CompilationSession](../eta/session/src/eta/session/compilation_session.cpp)
and [LSP](../eta/tools/lsp/src/eta/lsp/lsp_server.cpp) use the parser's non-strict
default. Strict mode rejects abbreviated unquote outside quasiquote. The current
parser does not decrement quasiquote depth while reading a comma operand; the CST
follows this exact policy, including quoted and discarded regions.

Every token records before/after BOF, shebang eligibility, strict-mode and quasiquote state, plus a
path of enclosing opener/prefix IDs including discarded input. A fresh original
reader validates states against the concrete stream. Disagreement is unsupported
tooling input. Cross-snapshot trace comparison matches token kind/raw spelling,
discard/prefix boundaries, states and paths, ignoring changed byte offsets and
whitespace. No-op directives still have entries.

The [expander](../eta/core/src/eta/reader/expander.cpp) lowers simultaneous `let`
to lambda application (initializers outside the new scope), `let*` to nested
bindings, `letrec` to allocated bindings followed by assignments and `letrec*`
to nested single-binding `letrec` forms. Later `letrec*` binders are outside
earlier initializer scopes; tooling must not assign broader Scheme scope
independently. Named `let` becomes a recursive procedure.
Lambda supports fixed, dotted-rest and rest-only formals; duplicate/reserved
binders are compiler errors. Leading internal definitions can become `letrec`
according to `ExpanderConfig`. `define-syntax` changes macro scope in expansion
order and can hide references/binders, so unmodelled macro-affected scope is unknown.

Source `if` accepts two or three arguments: the expander fills an omitted
alternate with an empty `(begin)` so the core analyzer receives three arguments.
`Emitter::emit_begin` emits no expression instructions for an empty sequence;
tooling does not invent a nil/unspecified value contract from Scheme semantics.
This behaviour must be accounted for before enabling ETA601. `cond` requires
at least one nonempty clause; `else` must be last and have a body. Arrow clauses
bind the test result once before calling their procedure. Invalid `else`
placement is delegated compiler coverage. The
[semantic analyzer](../eta/core/src/eta/semantics/semantic_analyzer.cpp) consumes
expanded core forms, not raw derived forms. Parameters/locals are immutable;
globals/imports follow compiler-owned assignment rules.
[ModuleLinker](../eta/core/src/eta/reader/module_linker.cpp) consumes expanded
forms, preserves import provenance and owns import/export/cycle errors.
Independent semantic fixtures already live in `expander_tests`, `semantics_tests`,
`module_linker_tests` and `eval_tests`. These APIs are not automatically available
as a non-executing source/provenance service; later lint rules must record missing coverage.

The builtin `=` is a variadic numeric comparison with at least two arguments;
its shared comparison helper rejects non-numeric arguments and handles TapeRef
values under the active differentiation policy. `equal?` is a separate deep
equality primitive for supported values. A future ETA602 finding therefore
needs proven operand types and the actual builtin binding; spelling alone is
insufficient. See `core_primitives_arithmetic{,_helpers}.{cpp,h}` and
`core_primitives_sequences.cpp`, with existing runtime/evaluation fixtures.

## Ownership, source positions and bridge

The shared immutable file owns its source and arenas. IDs are snapshot-local;
source views require that owner to remain alive. Only tokens own trivia ranges;
EOF owns the final gap. Nodes retain token ranges, child IDs, optional dot/tail
and virtual missing closers. Virtual tokens emit no bytes. Shebang excludes
LF/CRLF; its terminator belongs to the next token's whitespace. Header, end-of-line,
leading, dangling, reader-operand and datum-operand associations refer to existing
comment IDs and never own another source slice.

Serialization verifies the complete unique byte partition, then emits token-owned
trivia and raw token slices. It retains spelling, blank lines and line endings.
The bridge walks CST structure, suppresses directives/discarded data, reuses atom
and bytevector decoding and shared module recognition, and rejects recovered or
unsupported files. `read_original()` is a separate fresh-reader oracle.
Differential comparison covers reader kinds/values, numeric variant, NaNs and signed zero.

Source positions use logical LF/CRLF lines, one character per tab and explicit
Unicode-scalar, UTF-16 or UTF-8 encoding. CLI adds one to scalar line/columns;
LSP stays zero-based. Sparse scalar/UTF-16 checkpoints bound conversion work on
long valid lines; source-map construction polls cancellation and is included
in the memory budget. Reverse conversion rejects split scalars; edit boundaries
also reject split CRLF. Invalid runs map deterministically to one replacement
character without editing input. EOF and zero-width spans are supported.
Bridge compiler spans preserve byte columns; reporters use the shared source map.

## Limits, specifications and verification

Defaults: 16 MiB input, 32 MiB output, depth 256, 1,000,000 concrete tokens
including EOF, 1,000,000 nodes and 256 MiB owned memory. Source/line maps are
bounded before allocation; arenas use a counting resource. Diagnostics and
ordinary lexer/reader/bridge/output allocations use conservative explicit
reservations. Memory exhaustion can precede a count limit. Resource failure and
cancellation return exit class 2 without partial results. Shared cancellation
is polled between items/nodes and every 4096 scanner bytes. Snapshots remain
cancellable after parse and release arenas with their last owner.

[Diagnostic inventory](tooling_diagnostics.md) is generated from every consumed
native kind, including producer/API, message/location, code and exit class.
CTest verifies the inventory and exercises per-kind adapters, missing/related
spans, unknown kinds and unlocated module deduplication.
[Rule schema](../eta/tools/linter/rules/schema.json) validates required fields,
typed option defaults, fixtures with exact spans, prerequisites and preset gates.
Enabled specs additionally require dependency/unknown cases and verified
prerequisites; fixable specs require fixed-source cases. Phase 0 enables no rules.
The schema tooling uses Python 3.11 or newer (`tomllib`); CMake enforces this
version requirement without a fallback TOML parser.

The corpus gate covers all present `.eta` files under `stdlib`, `cookbook` and
`demo`, supported constructs, malformed/adversarial cases and seeded arbitrary
bytes. This working tree has no `demo` directory; the manifest records zero demo
files. `eta_fuzz_cst` exposes the same invariants to Clang/libFuzzer; MSVC runs
the deterministic property suite.

[Manifest](benchmarks/tooling_manifest.json) freezes every corpus hash, generated
fixture seed and timing selection. [Baseline](benchmarks/tooling_baseline.json)
records the reference machine/compiler/build, ten warmups and 100 measured
iterations, per-file/bin median/p95/throughput, process/owned-reserved memory and
cancellation latency. Process/I/O timings are separate from resident-library
timings. Formatter/rule baselines begin in Phases 1/2; different hardware is informational.
For operations below the Windows timer's resolution, a zero median is recorded
explicitly and throughput is null rather than an invented finite measurement.

```powershell
python scripts/tooling_benchmark.py --check
python scripts/tooling_benchmark.py --record out/msvc-release/eta/qa/bench/eta_reader_bench.exe
```

Intentional corpus changes require an explicit `--freeze` and a new baseline.
