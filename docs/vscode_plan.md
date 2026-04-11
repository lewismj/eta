# VS Code Extension — Improvement Plan

[← Back to README](../README.md)

---

## Current State (v0.2.0)

The Eta VS Code extension (`editors/vscode/`) provides:

| Feature | Implementation |
|---------|---------------|
| **Syntax highlighting** | TextMate grammar (`eta.tmLanguage.json`) — core forms, bindings, control flow, modules, macros, records, sugars, advanced control |
| **Language configuration** | Brackets, comments (`; …`, `#| … |#`), auto-closing pairs, indentation rules |
| **LSP client** | Connects to `eta_lsp` for diagnostics, hover, go-to-definition, completion |
| **DAP client** | Debug adapter via `eta_dap` — breakpoints, stepping, run |
| **Heap Inspector** | WebView panel — memory gauge, object-kind stats, GC roots tree, object drill-down |
| **GC Roots tree** | Tree data provider in the Debug sidebar |
| **Disassembly view** | Virtual document with PC-line highlighting |
| **Commands** | Run File, Show Heap Inspector, Show Disassembly, Show Disassembly (All), Refresh GC Roots, Inspect Object From Tree |
| **Build** | esbuild bundling, `vsce package`, CMake target `eta_editor_package`, `build-release.ps1` integration |

---

## Identified Gaps

1. **TextMate grammar — missing keyword groups**
   - Exception handling: `raise`, `catch`
   - Logic / unification: `logic-var`, `unify`, `deref-lvar`, `trail-mark`, `unwind-trail`, `copy-term`
   - CLP constraints: `%clp-domain-z!`, `%clp-domain-fd!`, `%clp-get-domain`
   - AD / tape builtins: `tape-new`, `tape-start!`, `tape-stop!`, `tape-var`, `tape-backward!`, `tape-adjoint`, `tape-primal`, `tape-ref?`, `tape-ref-index`, `tape-size`, `tape-ref-value`, `dual?`, `dual-primal`, `dual-backprop`, `make-dual`
   - Torch builtins (`torch/*`, `nn/*`, `optim/*` prefixed)
   - Common I/O and built-in procedures: `display`, `write`, `newline`, `println`, `error`, `map`, `for-each`, `cons`, `car`, `cdr`, `list`, `length`, `append`, `reverse`, etc.

2. **No code snippets** — users must type all forms from memory.

3. **Language configuration gaps** — no folding markers, no `onEnterRules` for better auto-indent.

4. **Package.json metadata gaps** — no `keywords`, redundant `onDebug` activation event, no snippet contribution.

5. **No extension README.md** — VS Code marketplace / GitHub shows nothing.

6. **No `textDocument/documentSymbol`** — Outline view is empty (requires C++ LSP work).

7. **No `textDocument/references` or `textDocument/rename`** — requires C++ LSP work.

8. **No semantic token support** — all highlighting is grammar-based (C++ LSP work).

9. **No extension tests** — no automated test suite.

---

## Phase 1 — Immediate (TypeScript / JSON / Markdown)

These changes require **no C++ modifications** and can be implemented entirely in the `editors/vscode/` tree.

### 1.1 Expand TextMate Grammar

**File:** `syntaxes/eta.tmLanguage.json`

Add new entries inside the `special-form-head` → `patterns` array:

| Group | Keywords | Scope |
|-------|----------|-------|
| Exception handling | `raise`, `catch` | `keyword.control.exception.eta` |
| Logic / unification | `logic-var`, `unify`, `deref-lvar`, `trail-mark`, `unwind-trail`, `copy-term` | `keyword.control.logic.eta` |
| Common builtins | `display`, `write`, `newline`, `println`, `error`, `cons`, `car`, `cdr`, `list`, `length`, `append`, `reverse`, `map`, `for-each`, `not`, `abs`, `min`, `max`, `modulo`, `remainder` | `support.function.builtin.eta` |
| Type predicates | `null?`, `pair?`, `number?`, `boolean?`, `string?`, `symbol?`, `procedure?`, `integer?`, `char?`, `vector?`, `zero?`, `positive?`, `negative?`, `eq?`, `eqv?`, `equal?`, `list?`, `port?`, `logic-var?`, `ground?` | `support.function.predicate.eta` |
| AD / tape | `make-dual`, `dual?`, `dual-primal`, `dual-backprop`, `tape-new`, `tape-start!`, `tape-stop!`, `tape-var`, `tape-backward!`, `tape-adjoint`, `tape-primal`, `tape-ref?`, `tape-ref-index`, `tape-size`, `tape-ref-value` | `support.function.ad.eta` |
| CLP | `%clp-domain-z!`, `%clp-domain-fd!`, `%clp-get-domain` | `support.function.clp.eta` |

Use `(?<=\\()\\s*` prefix and `(?=[\\s()\\[\\]])` suffix for head-position matching. Use `support.function.*` scopes for library builtins (so themes distinguish them from keywords).

### 1.2 Add Code Snippets

**New file:** `snippets/eta.json`

Snippets for the most common forms:

| Prefix | Description |
|--------|-------------|
| `module` | Full module skeleton with import and begin |
| `defun` | Function definition |
| `define` | Variable definition |
| `lambda` | Anonymous function |
| `let` | Parallel local bindings |
| `let*` | Sequential local bindings |
| `letrec` | Recursive local bindings |
| `if` | Conditional expression |
| `cond` | Multi-way conditional |
| `case` | Datum dispatch |
| `when` / `unless` | One-armed conditionals |
| `define-record-type` | Record type definition |
| `import` | Import form |
| `catch` / `raise` | Exception handling |

Register in `package.json` → `contributes.snippets`.

### 1.3 Improve Language Configuration

**File:** `language-configuration.json`

1. Add `folding.markers` for block comments (`#| … |#`).
2. Add `onEnterRules` for auto-indent after open parens.

### 1.4 Clean Up package.json Metadata

**File:** `package.json`

1. Add `"keywords": ["eta", "lisp", "scheme", "functional", "logic-programming", "unification"]`.
2. Remove redundant `"onDebug"` from `activationEvents`.
3. Register the snippets contribution.

### 1.5 Create Extension README.md

**New file:** `README.md`

Contents: title, feature list, requirements, settings table, quick-start guide, known limitations.


---

## Phase 2 — Near-term (C++ LSP Additions)

These require changes to `eta/lsp/src/eta/lsp/lsp_server.cpp` and `lsp_server.h`.

### 2.1 `textDocument/documentSymbol`

Implement to return `DocumentSymbol[]` for `define`, `defun`, `define-record-type`, `define-syntax`, and `module` forms. Enables the **Outline** view and **breadcrumbs**.

### 2.2 LSP Keyword Completions Update

Add missing reserved keywords to the completion list: `raise`, `catch`, `logic-var`, `unify`, `deref-lvar`, `trail-mark`, `unwind-trail`, `copy-term`.

### 2.3 `textDocument/references`

Scan the current file (and imported modules) for all usages of the symbol under the cursor.

### 2.4 `textDocument/rename`

Build on references support for safe renaming of local bindings and module-scoped definitions.

---

## Phase 3 — Future

### 3.1 Semantic Token Provider

Implement `textDocument/semanticTokens/full` in the C++ LSP for richer, context-aware highlighting (distinguish macros vs. builtins vs. user functions vs. logic variables).

### 3.2 Extension Tests

Add a `src/test/` directory with VS Code integration tests using `@vscode/test-electron`. Test activation, grammar scoping, snippet insertion, and DAP session lifecycle.

### 3.3 Formatter / Indent Engine

Either a TypeScript-side formatter or LSP `textDocument/formatting` for consistent Lisp-style indentation.

### 3.4 Signature Help

`textDocument/signatureHelp` in the LSP server, leveraging the existing arity metadata from builtin registration.

---

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Builtin scopes | `support.function.*` | Themes colour them differently from `keyword.control.*` special forms |
| Snippet count | ~16 core forms | Keep minimal; domain snippets (torch, logic) deferred |
| Folding | Marker-based (block comments) | Paren-based folding is handled by VS Code's bracket matching |
| `onDebug` removal | Remove from activationEvents | Already covered by `onDebugResolve:eta` |

