# VS Code lint + format completion patch plan

[Back to README](../README.md) ·
[VS Code Extension README](../editors/vscode/README.md) ·
[Lint + Format master plan](plan/lint_formatter_plan.md)

> **Status.** This document defines a **single implementation step** that closes out
> VS Code-facing linting and formatting behavior for Eta. The formatter and linter
> engines already exist and are linked into `eta_lsp`; this patch finishes the
> user-facing command surface and integrated workspace behavior.

---

## 1) Goal

Ship one cohesive update that makes linting and formatting feel complete inside
VS Code.

After this patch, the extension must expose and support:

- `Eta: Format Document`
- `Eta: Lint File`
- `Eta: Lint Workspace`

These commands must be integrated with the existing `eta_lsp` ownership model,
not implemented as unrelated fallback code paths.

---

## 2) Current state

The current implementation already provides the hard parts:

- `eta_lsp` links `eta_format_lib` and `eta_linter_lib` directly.
- Document formatting already exists via:
  - `textDocument/formatting`
  - `textDocument/rangeFormatting`
- Lint diagnostics already exist via document validation and
  `collect_lint_diagnostics(...)`.
- The VS Code extension already passes lint/format settings through
  `initializationOptions`.

What is missing is the explicit command surface and a first-class integrated way
for users to manually trigger file/workspace linting from VS Code.

---

## 3) Product decision

Implement this as **one patch** spanning the VS Code extension and the LSP.

### Why one patch

Doing this in pieces would leave the feature feeling half-done:

- adding only `Format Document` would improve discoverability but not finish linting;
- adding only `Lint File` would still leave workspace linting as a separate task-only path;
- adding only task wrappers would conflict with the existing LSP ownership model.

The right completion point is a single delivery where:

1. formatting has an explicit command;
2. linting has an explicit file command;
3. linting has an explicit workspace command;
4. all three are documented and tested together.

---

## 4) Required user-visible behavior

### 4.1 `Eta: Format Document`

Command palette entry that formats the active Eta document using the existing LSP
formatting path.

Required behavior:

- Works only for active `eta` documents.
- Invokes VS Code's normal formatting pipeline so the server remains the single
  formatting implementation.
- Surfaces normal formatter failures through the existing LSP/editor behavior.
- Does not shell out to `eta_fmt`.

### 4.2 `Eta: Lint File`

Command palette entry that forces revalidation/linting of the active Eta file.

Required behavior:

- Works for open `eta` documents.
- Re-runs the same compiler + lint diagnostic path used for live diagnostics.
- Republishes diagnostics for the current file.
- Returns quickly and never writes the file.
- Does not shell out to `eta_lint`.

### 4.3 `Eta: Lint Workspace`

Command palette entry that lints the workspace through the integrated tooling path.

Required behavior:

- Discovers Eta source files inside the open workspace folders.
- Uses the LSP/linter library path, not a standalone `eta_lint` subprocess.
- Publishes diagnostics for each linted file using the existing diagnostic channel.
- Clears stale lint results for previously linted files that now produce no findings.
- Reports summary progress/result information to the user.
- Never writes files.

---

## 5) Architecture decision

### 5.1 Formatting command remains client-side

`Eta: Format Document` should be implemented entirely in the VS Code extension by
calling the standard VS Code command for document formatting.

Reason:

- formatting already works through normal LSP capabilities;
- this keeps one formatting path;
- no new server command protocol is required for formatting.

### 5.2 Lint commands become explicit LSP-triggered operations

`Eta: Lint File` and `Eta: Lint Workspace` should be implemented via new LSP
commands / custom requests handled by `eta_lsp`.

Reason:

- lint logic already lives in the server;
- diagnostics are already published there;
- keeping lint orchestration server-side avoids a second client-only lint engine;
- workspace lint can reuse server-side path, resolver, and diagnostic publication logic.

### 5.3 Task-based linting remains optional, not authoritative

Existing CLI task/problem-matcher behavior may remain for explicit external task
usage, but it is no longer the primary answer for “lint this file/workspace” in
VS Code.

The integrated commands become the preferred path.

---

## 6) Concrete implementation scope

### 6.1 VS Code extension changes

### File: `editors/vscode/package.json`

Add new commands:

- `eta.formatDocument`
- `eta.lintFile`
- `eta.lintWorkspace`

Add command palette/menu entries with `resourceLangId == eta` gating where
appropriate.

Recommended titles:

- `Eta: Format Document`
- `Eta: Lint File`
- `Eta: Lint Workspace`

Optional but recommended:

- add editor title/context menu items for file-local commands;
- keep workspace lint command globally available when a workspace is open.

### File: `editors/vscode/src/extension.ts`

Register handlers:

- `eta.formatDocument`
  - validate active Eta editor;
  - invoke `editor.action.formatDocument`;
  - do not implement a second formatting code path.

- `eta.lintFile`
  - validate active Eta editor;
  - call an LSP command/request for the document URI;
  - show a lightweight status message on completion/failure.

- `eta.lintWorkspace`
  - call an LSP command/request for all workspace folders;
  - show progress UI (`window.withProgress(...)`);
  - surface a summary result.

The extension should not attempt to implement file walking or duplicate lint logic
if the LSP can own it.

---

### 6.2 LSP server changes

### File: `eta/tools/lsp/src/eta/lsp/lsp_server.h`

Add declarations for new command handling, for example:

- execute-command dispatcher support;
- command handlers for:
  - lint current file
  - lint workspace

Also add any helper declarations needed for workspace file discovery and stale
lint-diagnostic cleanup.

### File: `eta/tools/lsp/src/eta/lsp/lsp_server.cpp`

#### A. Advertise command support

Extend initialize capabilities to include an `executeCommandProvider` with the
new command IDs.

Suggested command IDs:

- `eta.lsp.lintFile`
- `eta.lsp.lintWorkspace`

#### B. Dispatch `workspace/executeCommand`

Add request handling for `workspace/executeCommand`.

Required routing:

- `eta.lsp.lintFile` → validate one URI
- `eta.lsp.lintWorkspace` → validate all discovered workspace Eta files

#### C. Reuse the existing document validation path

`Lint File` must reuse `validate_document(uri)` rather than introduce a parallel
lint-only path.

That guarantees:

- compiler diagnostics remain present;
- lint findings are merged exactly as today;
- there is still one authoritative editor diagnostic path.

#### D. Add workspace file discovery and batch validation

Implement workspace lint by discovering Eta files rooted at the known workspace
folders / manifest roots and invoking the existing validation pipeline for each.

Recommended scope for v1 of this patch:

- lint all `*.eta` files under open workspace folders;
- skip known build/vendor/cache directories where appropriate only if the current
  extension/LSP already has a deterministic precedent;
- process in stable sorted path order.

If workspace roots are already tracked through manifest/workspace resolution,
prefer using those roots rather than ad-hoc client-side globbing.

#### E. Clear stale diagnostics

Workspace lint must track which URIs were visited during the last manual workspace
lint pass and clear diagnostics for previously linted URIs that are no longer in
scope or no longer have findings.

This prevents “manual workspace lint” from leaving orphaned diagnostics behind.

#### F. Return a summary object

Return a structured summary to the client, e.g.:

- files visited
- files with diagnostics
- elapsed time or completion status
- whether cancellation occurred

The extension can display a friendly summary from this result.

---

### 6.3 Documentation changes

### File: `editors/vscode/README.md`

Update the extension docs to describe:

- the three new commands;
- that editor linting is LSP-owned;
- that manual lint file/workspace commands are integrated and do not shell out;
- that task/problem matcher flows remain explicit CLI/task workflows, not the
  primary editor integration path.

### Optional cross-reference updates

If needed, add a brief note in:

- `docs/eta_lint.md`
- `docs/eta_fmt.md`

but the VS Code README should be the primary end-user documentation surface.

---

## 7) Behavior contract details

### 7.1 Single ownership rule

After this patch:

- editor formatting is owned by `eta_lsp` via normal LSP formatting;
- editor lint diagnostics are owned by `eta_lsp` via validation + lint merge;
- manual lint commands trigger that same server-owned path.

There must not be two unrelated editor code paths that can disagree.

### 7.2 Failure behavior

- Missing active Eta editor for file-local commands → friendly warning message.
- Missing LSP client/server → explicit user-visible error.
- Workspace lint on no workspace → friendly warning message.
- Server-side errors → surfaced to the user; no silent fallback to CLI binaries.

### 7.3 No subprocess fallback

This patch must not introduce:

- shelling out to `eta_lint` for editor commands;
- shelling out to `eta_fmt` for editor formatting;
- a hidden “if LSP fails, try CLI” behavior.

That would regress the architecture already established in `eta_lsp`.

---

## 8) Tests required in the same patch

This patch is only complete if tests land in the same step.

### 8.1 Extension tests

Add/update VS Code extension tests to cover:

- command registration for:
  - `eta.formatDocument`
  - `eta.lintFile`
  - `eta.lintWorkspace`
- gating/behavior when no Eta editor is active;
- command invocation path for format document;
- client request/command dispatch for lint file and lint workspace.

Likely files:

- `editors/vscode/test/suite/extension.test.ts`
- or adjacent dedicated command tests if cleaner.

### 8.2 LSP tests

Add server tests covering:

- `workspace/executeCommand` handling for `eta.lsp.lintFile`;
- `workspace/executeCommand` handling for `eta.lsp.lintWorkspace`;
- stable workspace traversal;
- correct publication/refresh of diagnostics;
- stale-diagnostic clearing for manual workspace lint runs.

Existing LSP tests should also confirm no regression to:

- standard live diagnostics;
- full formatting;
- range formatting.

### 8.3 Integration expectation

A practical integration test should verify this flow:

1. Open an Eta file with a lint issue.
2. Invoke `Eta: Lint File`.
3. Diagnostics appear for that file.
4. Fix file contents.
5. Invoke `Eta: Lint File` again.
6. Diagnostics update/clear correctly.

And for workspace lint:

1. Workspace contains multiple Eta files with issues.
2. Invoke `Eta: Lint Workspace`.
3. Diagnostics appear across files.
4. Fix one file.
5. Invoke `Eta: Lint Workspace` again.
6. Fixed file diagnostics clear; others remain accurate.

---

## 9) Implementation order inside the single patch

The patch should be developed in this order, but shipped together as one step.

1. **LSP command plumbing**
   - add execute-command support;
   - add lint-file and lint-workspace handlers;
   - add workspace traversal and stale-diagnostic cleanup.

2. **VS Code command surface**
   - add command contributions;
   - register handlers;
   - wire formatting to standard editor formatting command;
   - wire lint commands to LSP commands.

3. **Tests**
   - LSP tests first;
   - extension tests second;
   - integration-style checks last.

4. **Docs**
   - update extension README in same patch.

This remains one shipped step even if developed in that internal order.

---

## 10) Acceptance criteria

The patch is done only when all of the following are true:

- [ ] VS Code command palette shows `Eta: Format Document` for Eta editing.
- [ ] VS Code command palette shows `Eta: Lint File`.
- [ ] VS Code command palette shows `Eta: Lint Workspace`.
- [ ] `Eta: Format Document` uses the existing LSP formatting path.
- [ ] `Eta: Lint File` re-runs the current integrated validation/lint pipeline.
- [ ] `Eta: Lint Workspace` lints workspace Eta files without invoking CLI binaries.
- [ ] Manual lint commands publish diagnostics through the normal LSP channel.
- [ ] Stale diagnostics are cleared correctly on repeat workspace lint runs.
- [ ] No fallback subprocess path is introduced for editor commands.
- [ ] Extension docs describe the new commands and ownership model.
- [ ] Automated tests cover command registration and command behavior.

---

## 11) Non-goals for this patch

To keep this truly single-step and finishable, the patch should **not** also try to:

- add lint auto-fix code actions;
- add `Eta: Format Workspace`;
- add standalone `eta_fmt` / `eta_lint` executable selection for editor requests;
- redesign existing problem matcher/task flows;
- change linter rule behavior or formatter semantics.

Those can be separate follow-up work if needed.

---

## 12) Recommendation

Implement this as one integrated patch touching:

- `editors/vscode/package.json`
- `editors/vscode/src/extension.ts`
- `eta/tools/lsp/src/eta/lsp/lsp_server.h`
- `eta/tools/lsp/src/eta/lsp/lsp_server.cpp`
- extension/LSP tests
- `editors/vscode/README.md`

That is the smallest complete step that genuinely closes out VS Code linting and
formatting behavior without fragmenting ownership or leaving lint workspace as a
second-class task-only feature.


