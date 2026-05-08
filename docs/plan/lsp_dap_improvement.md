# LSP / DAP doc-registry improvement plan
Status: proposed
Owner: tooling
Scope: `eta/core`, `eta/session`, `eta/tools/lsp`, `eta/tools/dap`, `eta/qa`
## Problem
Hover, completion, and signature-help documentation for special forms and
builtins is currently hardcoded in tool/session code instead of being derived
from language/runtime metadata.
Known hardcoded symbol metadata locations:
| Location | What it holds |
|---|---|
| `eta/tools/lsp/src/eta/lsp/lsp_server.cpp:1118` | Hover docs for special forms and selected builtins, including an `#ifdef ETA_HAS_NNG` block |
| `eta/tools/lsp/src/eta/lsp/lsp_server.cpp:1320` | Completion details for special forms |
| `eta/tools/lsp/src/eta/lsp/lsp_server.cpp:1377` | Completion metadata for static builtins, including another `#ifdef ETA_HAS_NNG` block |
| `eta/tools/lsp/src/eta/lsp/lsp_server.cpp:2103` | Signature-help metadata for builtins/special forms, including another `#ifdef ETA_HAS_NNG` block around `lsp_server.cpp:2162` |
| `eta/session/src/eta/session/driver.h:711` | Smaller overlapping hover-doc map, already drifted from the LSP map |
| `eta/session/src/eta/session/driver.h` completion path | Keyword/builtin completion metadata assembled separately from LSP completion |
Issues:
1. **Duplication / drift** — LSP hover, LSP completion, LSP signature help,
   session hover, and session completion do not share one source of truth.
2. **Wrong layer** — LSP/DAP know about builtin names, arities, categories,
   and optional NNG-specific symbols even though this information belongs in
   core/runtime metadata.
3. **Conditional compilation leakage** — `#ifdef ETA_HAS_NNG` appears inside
   `eta/tools/lsp/` purely to keep editor metadata aligned with configured
   runtime features.
4. **No reuse** — DAP can enumerate builtin slots but has no shared symbol-doc
   API; Jupyter/session and editor extensions cannot reuse LSP docs.
5. **Stdlib invisibility** — docs in `docs/stdlib/*.md` and comments/docstrings
   in `stdlib/std/*.eta` are not surfaced to editor/debugger tooling.
## Goal
Create a **single, core-owned source of truth for language symbol metadata**
consumed by:
* LSP hover
* LSP completion
* LSP signature help
* session/Jupyter hover and completion
* DAP custom symbol-doc requests
* future VS Code/Jupyter inspector integrations
The first milestone is to remove hardcoded special-form and builtin metadata
from `eta/tools/` and `eta/session/`. Stdlib doc extraction is a later, separate
phase.
## Key design decision
Builtin docs must not live only in runtime `register_*_primitives()` sites.
The current workspace already has:
`eta/core/src/eta/runtime/builtin_names.h`
This file is the analysis-only builtin registration source used by LSP/tools
that need names and arities but do not execute primitive functions:
```cpp
runtime::BuiltinEnvironment builtins;
runtime::register_builtin_names(builtins);
```
Therefore builtin docs, categories, and signatures must be attached to the same
metadata source that powers `register_builtin_names()`, not only to runtime
implementation registration sites. Runtime primitive registration can validate
against this metadata, but canonical docs must be available without executing
runtime registration.

Before moving existing table entries, classify every hardcoded LSP/session
symbol as one of:

* reader/expander special form or syntax alias
* runtime builtin already registered through `builtin_names.h`
* stdlib/prelude binding
* macro or language alias outside the reader
* obsolete/unknown entry

Only true reader/expander constructs should move to reader-owned
special-form metadata. Runtime primitives that currently appear in LSP
"keyword" tables, such as `apply`, AD/tape, CLP, logic, or NNG symbols, belong
in builtin metadata if they are configured builtins. Signature-help-only entries
that do not correspond to configured builtins should be removed or deferred to a
future stdlib/prelude metadata phase.
## Recommended architecture
### Core-owned docs primitives
Place shared documentation primitives in `eta/core`, not `eta/session`, so core
reader/runtime metadata can depend on them without introducing a dependency
cycle.
Proposed files:
```text
eta/core/src/eta/docs/doc_entry.h
eta/core/src/eta/docs/markdown.h
eta/core/src/eta/reader/special_form_docs.h
eta/core/src/eta/runtime/builtin_metadata.h
```
`session`, `lsp`, `dap`, and QA tests can all include these core headers.
### `DocEntry`
`eta/core/src/eta/docs/doc_entry.h`:
```cpp
#pragma once
#include <string_view>
namespace eta::docs {
enum class DocKind {
    SpecialForm,
    Macro,
    Builtin,
    StdlibBinding,
};
struct DocEntry {
    std::string_view name;
    std::string_view signature;
    std::string_view summary;
    DocKind          kind;
    std::string_view details {};
    std::string_view category {};
    std::string_view module {};
};
} // namespace eta::docs
```
`eta/core/src/eta/docs/markdown.h` should provide inline rendering helpers:
```cpp
namespace eta::docs {
inline std::string render_markdown(const DocEntry& entry);
inline std::string render_builtin_markdown(const eta::runtime::BuiltinMetadata& builtin);
} // namespace eta::docs
```
If build/link complexity is undesirable for the first PR, keep rendering helpers
header-only/inline. If this grows beyond simple formatting, introduce a small
core docs library and wire it through CMake explicitly.
### Special-form metadata
Special forms are reader/expander concepts, not builtins. Keep their metadata in
core near the reader/expander implementation:
`eta/core/src/eta/reader/special_form_docs.h`:
```cpp
#pragma once
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include "eta/docs/doc_entry.h"
namespace eta::reader {
inline constexpr std::array<eta::docs::DocEntry, /*N*/> kSpecialFormDocs = {
    // define, lambda, if, begin, let, cond, syntax-rules, ...
};
inline std::span<const eta::docs::DocEntry> special_form_docs() {
    return kSpecialFormDocs;
}
inline std::optional<eta::docs::DocEntry> lookup_special_form_doc(std::string_view name) {
    for (const auto& e : kSpecialFormDocs) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}
} // namespace eta::reader
```
This removes special-form tables from both LSP and session code. Do not place
runtime primitives here simply because they previously lived in an LSP
"keyword" map; `apply`, `values`, `call/cc`, AD/tape, CLP, logic, and NNG
entries must be classified before being moved.
### Builtin metadata
Create canonical builtin metadata in core/runtime:
`eta/core/src/eta/runtime/builtin_metadata.h`:
```cpp
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
namespace eta::runtime {
struct BuiltinMetadata {
    std::string_view name;
    uint32_t         arity;
    bool             has_rest;
    std::string_view category;
    std::string_view signature;
    std::string_view summary;
};
inline constexpr std::array<BuiltinMetadata, /*N*/> kBuiltinMetadata = {
    // +, -, cons, map, torch/tensor, ... configured optional blocks ...
};
inline std::span<const BuiltinMetadata> builtin_metadata() {
    return kBuiltinMetadata;
}
inline std::optional<BuiltinMetadata> lookup_builtin_metadata(std::string_view name) {
    for (const auto& b : kBuiltinMetadata) {
        if (b.name == name) return b;
    }
    return std::nullopt;
}
} // namespace eta::runtime
```
`builtin_metadata()` must be the configured build's builtin list. Optional
features such as NNG are gated inside this core/runtime metadata source, not in
LSP/DAP/session code.

For PR 1, prefer header-only metadata with `inline constexpr` tables and `inline`
lookup helpers so no CMake link changes are needed. In that shape, feature macros
such as `ETA_HAS_NNG` are evaluated in the consuming target. If this metadata is
later moved into a compiled `.cpp`, feature ownership must move to a shared
generated config header or PUBLIC core compile definitions so `eta_core`, LSP,
DAP, session, interpreter, and tests all see the same configured builtin list.
The current workspace builds the main tools with NNG linked/defined, so do not
claim NNG-disabled behaviour until a disabled configuration is actually wired and
tested.
Update `eta/core/src/eta/runtime/builtin_names.h` so
`register_builtin_names()` loops over `builtin_metadata()`:
```cpp
inline void register_builtin_names(BuiltinEnvironment& env) {
    for (const auto& b : builtin_metadata()) {
        env.register_builtin(
            std::string(b.name),
            b.arity,
            b.has_rest,
            PrimitiveFunc{});
    }
}
```
This preserves the current analysis-only flow while removing duplicated builtin
name/arity/category tables from tools.
### Relationship to `BuiltinSpec`
Do **not** make runtime implementation registration sites the only source of
docs. There are two acceptable options:
#### Option A — Keep `BuiltinSpec` unchanged initially
Use `BuiltinMetadata` for docs and keep `BuiltinSpec` as execution/install
metadata:
```cpp
struct BuiltinSpec {
    std::string name;
    uint32_t arity;
    bool has_rest;
    PrimitiveFunc func;
};
```
This minimizes PR 1 risk.
#### Option B — Add optional doc fields to `BuiltinSpec`
If useful later, extend `BuiltinSpec` with copied metadata:
```cpp
struct BuiltinSpec {
    std::string  name;
    uint32_t     arity;
    bool         has_rest;
    PrimitiveFunc func;
    std::string_view category  {};
    std::string_view signature {};
    std::string_view summary   {};
};
```
Even then, the canonical source should remain `builtin_metadata()`, with
`register_builtin_names()` copying metadata into the environment.
Runtime `register_*_primitives()` sites should validate name/arity/order against
`builtin_metadata()` but should not become the only place docs are authored.
`BuiltinEnvironment` already has patch-mode validation that checks name, arity,
and rest flags while runtime primitive functions are installed into
pre-registered slots. Once `register_builtin_names()` is driven by
`builtin_metadata()`, PR 2 should extend and test that existing validation rather
than inventing a separate validation path from scratch.
### Unified lookup helpers
Avoid a single global `lookup(name)` that hides configuration context. Prefer
layered lookups:
```cpp
namespace eta::docs {
inline std::optional<DocEntry> lookup_language_doc(std::string_view name);
inline std::optional<DocEntry> doc_from_builtin_metadata(const eta::runtime::BuiltinMetadata& b);
inline std::string render_markdown(const DocEntry& entry);
inline std::string render_builtin_markdown(const eta::runtime::BuiltinMetadata& b);
} // namespace eta::docs
```
If kept, `lookup_language_doc(name)` should remain a thin special-form/syntax
lookup. Builtins are resolved from `eta::runtime::lookup_builtin_metadata(name)`
so optional features track the configured builtin metadata.
A later stdlib phase can add:
```cpp
std::optional<DocEntry> lookup_stdlib_doc(std::string_view qualified_or_short_name);
```
## Consumer changes
### LSP hover
Replace the hardcoded `keyword_docs` map in
`eta/tools/lsp/src/eta/lsp/lsp_server.cpp` with layered lookups.
The current `LspServer` does **not** have a `driver_` member, so do not use
`driver_.builtins().specs()` in LSP snippets.
Use core metadata directly:
```cpp
if (auto e = eta::reader::lookup_special_form_doc(word)) {
    return json::object({
        {"contents", json::object({
            {"kind", "markdown"},
            {"value", eta::docs::render_markdown(*e)},
        })},
    });
}
if (auto b = eta::runtime::lookup_builtin_metadata(word)) {
    return json::object({
        {"contents", json::object({
            {"kind", "markdown"},
            {"value", eta::docs::render_builtin_markdown(*b)},
        })},
    });
}
```
Then keep existing document-local symbol hover fallback unchanged.
### LSP completion
Replace both hardcoded completion tables in `handle_completion()`:
* special forms around `lsp_server.cpp:1320`
* static builtins around `lsp_server.cpp:1377`
with loops over shared metadata:
```cpp
for (const auto& e : eta::reader::special_form_docs()) {
    if (!seen.insert(std::string(e.name)).second) continue;
    items.push_back(json::object({
        {"label", std::string(e.name)},
        {"kind", 14},
        {"detail", e.category.empty() ? "Keyword" : std::string(e.category)},
        {"documentation", json::object({
            {"kind", "markdown"},
            {"value", eta::docs::render_markdown(e)},
        })},
    }));
}
for (const auto& b : eta::runtime::builtin_metadata()) {
    if (!seen.insert(std::string(b.name)).second) continue;
    std::string detail = std::string(b.category) + " (arity " + std::to_string(b.arity);
    if (b.has_rest) detail += "+";
    detail += ")";
    items.push_back(json::object({
        {"label", std::string(b.name)},
        {"kind", 3},
        {"detail", detail},
        {"documentation", json::object({
            {"kind", "markdown"},
            {"value", eta::docs::render_builtin_markdown(b)},
        })},
    }));
}
```
No `#ifdef ETA_HAS_NNG` should remain in `eta/tools/lsp/`; NNG symbols appear
only if `builtin_metadata()` includes them for the configured build.
### LSP signature help
Replace the hardcoded `builtin_sigs` table in `handle_signature_help()` with
metadata lookups:

```cpp
if (auto b = eta::runtime::lookup_builtin_metadata(func_name)) {
    label = std::string(b->signature);
}
if (label.empty()) {
    if (auto e = eta::reader::lookup_special_form_doc(func_name)) {
        label = std::string(e->signature);
    }
}
```

Then keep the existing document-local, prelude, and module-path signature
fallbacks unchanged. Do not blindly import signature-help-only entries such as
names that are not configured builtins; either remove obsolete entries or defer
them to a later stdlib/prelude metadata design.
### Session driver
Update `eta/session/src/eta/session/driver.h::hover_at()`:
```cpp
[[nodiscard]] std::string hover_at(const std::string& symbol) const {
    if (symbol.empty()) return {};
    if (auto e = eta::reader::lookup_special_form_doc(symbol)) {
        return eta::docs::render_markdown(*e);
    }
    if (auto b = eta::runtime::lookup_builtin_metadata(symbol)) {
        return eta::docs::render_builtin_markdown(*b);
    }
    // Existing module/global lookup remains unchanged.
}
```
Also update session completion to use `special_form_docs()` and
`builtin_metadata()` instead of local hardcoded keyword/builtin completion data.
The current `Driver::CompletionResult` carries candidate strings only, so PR 1
should preserve that API and source names from shared metadata. Returning rich
completion details/documentation in Jupyter/session requires a separate API
extension.
This is an intentional behaviour improvement for session/Jupyter hover coverage;
it is not strictly byte-for-byte behaviour preservation for `Driver::hover_at()`.
### DAP
Do not overload the standard DAP variable `"value"` field with documentation.
That field is the displayed runtime value in DAP clients and must remain stable.
Add a custom request instead:
```text
eta/symbolDoc
```
The request is gated behind an Eta-specific capability advertised during DAP
initialization. It accepts a symbol name and returns markdown from the same
special-form / builtin metadata used by LSP and session hover.

Suggested initialize capability:

```json
{
  "supportsEtaSymbolDocRequest": true
}
```

Suggested request arguments:

```json
{
  "symbol": "map"
}
```

Suggested successful response body:

```json
{
  "found": true,
  "name": "map",
  "kind": "markdown",
  "category": "Higher-order",
  "signature": "(map proc list)",
  "documentation": "**map** ..."
}
```

Unknown symbols should return a successful DAP response with `found: false`
instead of treating absence of documentation as a protocol error.
Optional later enhancement: add an Eta-specific `documentation` or
`etaDocumentation` field to custom environment responses, but do not rely on
standard `presentationHint.attributes` to display markdown in clients.
### Jupyter kernel and VS Code extension
Once `Driver::hover_at()` and completion use core metadata, Jupyter/session
users pick up the same docs automatically. VS Code can reuse LSP hover normally
and call DAP `eta/symbolDoc` only while debugging.
## Migration steps
### PR 1 — Core metadata skeleton + LSP/session de-hardcoding
This PR is mostly internal refactor, but it intentionally improves
`Driver::hover_at()` coverage because the session will start seeing the fuller
shared metadata table.
* Add `eta/core/src/eta/docs/doc_entry.h`.
* Add `eta/core/src/eta/docs/markdown.h` with inline markdown rendering helpers.
* Add `eta/core/src/eta/reader/special_form_docs.h` with the classified true
  special-form/syntax docs currently hardcoded in LSP hover/completion/signature
  help.
* Add `eta/core/src/eta/runtime/builtin_metadata.h` with the builtin names,
  arities, rest flags, categories, signatures, and summaries currently split
  across `builtin_names.h` and LSP completion/hover/signature-help tables.
* Classify existing LSP/session entries before moving them so runtime builtins,
  true reader forms, stdlib/prelude bindings, and obsolete entries do not get
  collapsed into one misleading registry.
* Change `eta/core/src/eta/runtime/builtin_names.h::register_builtin_names()`
  to loop over `builtin_metadata()`.
* Change LSP hover and completion to consume `special_form_docs()` and
  `builtin_metadata()`.
* Change LSP signature help to consume `signature` fields from the same metadata
  and keep existing local/prelude/module fallback tiers.
* Change session hover and completion to consume the same metadata.
* Delete hardcoded LSP/session special-form and builtin symbol metadata tables.
* Keep existing document-local, prelude, and module-path completion tiers
  unchanged.
Tests:
* `lsp_hover_known_special_form_returns_markdown`
* `lsp_hover_known_builtin_returns_markdown`
* `lsp_hover_unknown_symbol_returns_null`
* `lsp_completion_uses_builtin_metadata`
* `lsp_signature_help_known_special_form_uses_metadata`
* `lsp_signature_help_known_builtin_uses_metadata`
* `lsp_signature_help_unknown_symbol_uses_existing_fallback_or_null`
* `driver_hover_known_special_form_returns_markdown`
* `driver_hover_known_builtin_returns_markdown`
* `driver_completion_uses_metadata_names`
* `doc_metadata_has_no_duplicate_special_forms`
* `doc_metadata_has_no_duplicate_builtins`
### PR 2 — Runtime metadata validation
* Reuse and extend existing `BuiltinEnvironment` patch-mode validation so runtime
  primitive registration order/name/arity/rest flags are checked against
  `builtin_metadata()`.
* Add an `undocumented_builtins()` or `missing_builtin_docs()` helper for QA.
* Decide whether to keep `BuiltinSpec` unchanged or copy metadata fields into it
  from `builtin_metadata()`.
* Add QA coverage that every configured builtin has non-empty category,
  signature, and summary, with any intentional exceptions explicitly whitelisted.
Tests:
* `all_configured_builtins_have_docs`
* `runtime_builtin_registration_matches_metadata`
* `nng_metadata_matches_configured_build` when both enabled and disabled
  configurations are actually supported in CI
### PR 3 — DAP integration
* Advertise an Eta-specific DAP capability for symbol docs.
* Add custom request `eta/symbolDoc`.
* Implement lookup through `lookup_special_form_doc()` and
  `lookup_builtin_metadata()`.
* Add DAP QA tests for capability advertisement, known special form, known
  builtin, and unknown symbol (`found: false`).
* Optionally add Eta-specific documentation fields to custom environment
  responses, but do not modify standard DAP `"value"` semantics.
### PR 4 — Stdlib docs (separate design)
Treat stdlib doc extraction as a separate design/implementation after hardcoded
special-form and builtin metadata have been removed.
Open questions for that design:
* exact docstring syntax (`;;@doc`, metadata form, or docstring after `define`)
* parser/reader impact
* how docs map to fully qualified module names
* how generated docs interact with `docs/stdlib/*.md`
* CMake/build dependency for generated `stdlib_docs.inc`
* conflict rules when source comments and markdown docs disagree
Possible implementation:
* Add a doc-comment convention to `.eta` stdlib files.
* Extend `scripts/build_stdlib_etac.py` or add a sibling generator to emit
  `stdlib_docs.inc`.
* Add `lookup_stdlib_doc()` as a fallback for qualified and imported stdlib
  symbols.
## Acceptance criteria
* No hardcoded special-form or builtin symbol metadata tables remain under
  `eta/tools/` or `eta/session/`. This includes hover docs, completion details,
  signature-help metadata, builtin categories, signatures, arities, and
  optional-feature lists.
* `eta/tools/lsp/` contains no `#ifdef ETA_HAS_NNG` used for hover, completion,
  or signature-help symbol metadata.
* `eta/core/src/eta/runtime/builtin_names.h::register_builtin_names()` is driven
  by `builtin_metadata()`.
* LSP hover, completion, and signature help use the same special-form and builtin
  metadata as session hover/completion.
* DAP docs are exposed through `eta/symbolDoc` with an advertised Eta-specific
  capability and a stable success/unknown-symbol response shape; standard DAP
  variable `"value"` remains runtime value only.
* Adding or removing a configured builtin requires updating the canonical
  builtin metadata source and the runtime implementation/registration, with QA
  validation catching drift between them.
* QA tests for duplicate doc entries, builtin documentation coverage, LSP hover,
  LSP completion, LSP signature help, session hover/completion names, and DAP
  `eta/symbolDoc` pass.
## Risks / non-goals
* PR 1 does not redesign DAP or stdlib docs.
* PR 1 should avoid a new linked `.cpp` registry unless CMake is updated for all
  consumers. Prefer header-only/inline metadata for the first migration.
* Header-only metadata must use `inline constexpr` tables and `inline` helpers;
  non-inline declarations require a compiled registry and CMake changes.
* NNG-disabled metadata behaviour is a future configuration concern unless and
  until the build grows a tested NNG-disabled target/configuration.
* Runtime `register_*_primitives()` sites are not the canonical docs source;
  they validate against canonical metadata.
* Stdlib docstring extraction is explicitly outside the initial de-hardcoding
  milestone.