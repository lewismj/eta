# Eta Workspace Plan

[Back to README](../../README.md) ·
[Packaging System](../packaging.md) ·
[Package Commands](../guide/packages.md) ·
[Next Steps](../next-steps.md)

---

## 1) Objective

Add first-class workspace support to Eta packaging so multiple packages can be
managed, resolved, built, and tested as one unit from a top-level
`eta.toml`.

Primary outcome:

1. one workspace manifest model,
2. one deterministic workspace lockfile,
3. workspace-aware `eta` CLI flows and tooling behavior.

---

## 2) Original intent

This plan implements the reserved workspace design noted in
`docs/plan/eta_packaging_plan.md`:

> Workspace: A top-level directory whose `eta.toml` declares
> `[workspace] members = [...]`, a la Cargo workspaces.

Workspaces were intentionally deferred after S0-S7; this document defines the
implementation path.

---

## 3) Scope and non-goals

### 3.1 In scope

1. workspace manifest grammar (`[workspace]`),
2. workspace root + member discovery,
3. workspace lockfile contract (`eta.lock` at workspace root),
4. workspace-aware resolution/materialization,
5. CLI command behavior from workspace root and member directories,
6. REPL/LSP/DAP/Jupyter workspace context wiring.

### 3.2 Out of scope (initial workspace release)

1. registry/signing changes (packaging S8),
2. native sidecar policy,
3. full Cargo-equivalent feature set (`workspace.dependencies`,
   `workspace.package` inheritance, publish orchestration),
4. distributed remote build/cache behavior.

---

## 4) Current state and gaps

## 4.1 Current behavior (May 2026)

1. `eta.toml` is package-centric (`[package]`, `[compatibility]`,
   `[dependencies]`, `[dev-dependencies]`).
2. CLI discovery (`find_manifest_path`) finds nearest `eta.toml` only; no
   workspace context classification.
3. Resolver/lockfile operate for a single root package graph.
4. `manifest.cpp` accepts unknown sections as "other", but still requires
   `[package].name/version/license` and `[compatibility].eta`.
5. A workspace-only root manifest therefore fails package parse today.

## 4.2 Gap to close

Eta needs a manifest document model that can represent:

1. package-only,
2. workspace-only (virtual root),
3. package + workspace in same root manifest.

---

## 5) Workspace model

## 5.1 Definitions

1. **Workspace root**: directory containing `eta.toml` with `[workspace]`.
2. **Virtual workspace**: workspace root with no `[package]`.
3. **Rooted workspace**: workspace root with both `[workspace]` and `[package]`.
4. **Member package**: package directory selected by `[workspace].members`.
5. **Selected members**: members targeted by a command (`default-members`,
   `--workspace`, `-p`).

## 5.2 Manifest schema (initial)

Workspace root:

```toml
[workspace]
members = ["packages/*", "apps/myapp"]
exclude = ["packages/experimental/*"]
default-members = ["apps/myapp"]
```

Validation rules:

1. `members` required, non-empty array of strings,
2. glob expansion deterministic (sorted canonical paths),
3. each selected member must contain a valid package `eta.toml`,
4. no duplicate canonical paths,
5. no duplicate package names across members.

Optional rooted workspace form:

```toml
[package]
name = "root_tools"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[workspace]
members = ["packages/*"]
```

Rule: if root has `[package]`, `"."` is an implicit member unless explicitly
excluded.

## 5.3 Member manifest contract

Member manifests remain regular package manifests. No member-specific workspace
keys are required in v1.

---

## 6) Lockfile and materialization model

## 6.1 Single lockfile

Workspace root owns `eta.lock` for all selected members.

Rules:

1. `eta.lock` lives at workspace root,
2. member-local lockfiles are not written in workspace mode,
3. dependency resolution is global across selected members.

## 6.2 Lockfile representation

Keep current lockfile schema version unless a new field is required.
Represent each member package as a lockfile package row with source:

1. `source = "workspace+<relative-path>"` for members,
2. existing source kinds for external deps (`path+`, `git+`, `tarball+`, `root`).

## 6.3 Shared materialization roots

Workspace shared roots:

1. `<workspace>/.eta/modules/` (shared dep materialization),
2. `<workspace>/.eta/target/` (shared build artifacts).

Member commands executed inside workspace still use workspace `.eta` root for
dedup and determinism.

---

## 7) Resolver and discovery

## 7.1 Manifest document parser

Introduce a manifest document layer in `eta::package`:

1. `ManifestDocument` with optional `package` and optional `workspace`,
2. `read_manifest_document(path)` for CLI discovery workflows,
3. existing `read_manifest(path)` remains package-only helper and errors when
   no package section exists.

## 7.2 Workspace discovery

New discovery routine:

1. walk up from CWD,
2. detect nearest package manifest,
3. detect nearest workspace root that includes that package path,
4. compute active context: standalone package vs workspace(member/root).

## 7.3 Workspace resolution

Add workspace-level resolver entrypoint:

1. enumerate selected members,
2. resolve each member graph with shared resolver options,
3. union package graph deterministically,
4. reject ambiguous package name collisions.

---

## 8) CLI contract

## 8.1 New selection flags

For `tree`, `build`, `test`, `bench`, `run`, `vendor`, `clean`, `update`:

1. `--workspace` (all members),
2. `-p, --package <name>` (one or more selected members),
3. `--exclude <name>` (with `--workspace`),
4. `--manifest-path <path>` (explicit root/member manifest).

## 8.2 Default selection policy

From workspace root:

1. if `default-members` set: operate on that set,
2. else if rooted workspace: default to root package only,
3. else (virtual workspace): default to all members for `build/test/bench/tree`,
   require explicit `-p` for member-specific commands (`run`, `add`, `remove`).

From member directory:

1. default target is current member package,
2. `--workspace` expands to all workspace members.

## 8.3 Command notes

1. `eta add/remove`: modifies selected member manifest, not workspace root.
2. `eta update`: updates workspace lockfile from selected set (default all).
3. `eta vendor`: materializes shared workspace modules by default.
4. `eta run`: requires a single selected package target.

---

## 9) Build and artifact layout

Workspace build output convention:

1. `.eta/target/<profile>/<member-name>/...` for member outputs,
2. per-member primary artifact naming unchanged (`<module>.etac`),
3. install logic can target a specific member (`eta install -p <name>`).

This avoids artifact collisions for similarly named entry modules.

---

## 10) Tooling integration

## 10.1 REPL

1. from workspace root: preload selected member/module roots,
2. from member dir: preload current member + shared deps.

## 10.2 LSP

1. workspace root detection per document,
2. diagnostics can indicate member ownership for `eta.toml` and `eta.lock`,
3. `eta/lockfile/explain` extended with member context.

## 10.3 DAP and Jupyter

1. launch resolution selects member package and workspace roots,
2. default debug profile behavior unchanged.

---

## 11) Migration strategy

1. Existing single-package projects continue unchanged.
2. Users opt in by adding `[workspace]` root manifest and member manifests.
3. During transition, if workspace context is invalid, CLI prints clear fixup
   diagnostics and suggests `--manifest-path`.

---

## 12) Staged roadmap

### W0 - Contract freeze and fixtures

1. add workspace fixtures in `eta/qa/pkg_test` and `eta/qa/cli_test`,
2. freeze current single-package behavior tests.

Gate: no regressions in current packaging tests.

### W1 - Manifest document and workspace parse

1. add `ManifestDocument` and workspace section parser,
2. keep package parser compatibility,
3. add validation errors for malformed workspace manifests.

Gate: parser tests for package-only, workspace-only, rooted workspace.

### W2 - Discovery and context classification

1. implement workspace-aware manifest discovery,
2. classify command context (standalone/member/workspace-root).

Gate: CLI tests for context detection from nested directories.

### W3 - Workspace resolver and lockfile wiring

1. implement workspace graph union,
2. write/read workspace lockfile,
3. shared modules root materialization.

Gate: deterministic lockfile output and stable resolver order.

### W4 - CLI command semantics and flags

1. add `--workspace`, `-p/--package`, `--exclude`, `--manifest-path`,
2. enforce single-target commands (`run`, `add`, `remove`),
3. update help/usage text.

Gate: end-to-end CLI tests from workspace root and member dirs.

### W5 - Build/test/install path updates

1. workspace target path layout,
2. member-scoped install selection,
3. vendor/clean behavior at workspace root.

Gate: artifact layout tests and install smoke tests.

### W6 - Tooling integration

1. wire workspace context to REPL/LSP/DAP/Jupyter,
2. add manifest/lock diagnostics for workspace mode.

Gate: tooling integration tests remain green.

### W7 - Docs and migration notes

1. update `docs/guide/packages.md`,
2. add workspace examples in cookbook packaging docs,
3. release-note and next-steps updates.

Gate: docs and examples reflect shipped behavior.

---

## 13) Testing strategy

1. **Manifest tests**:
   - workspace grammar validation,
   - rooted vs virtual workspace parsing.
2. **Resolver tests**:
   - member expansion, duplicate-name errors, deterministic graph order.
3. **CLI tests**:
   - command selection semantics (`--workspace`, `-p`, default-members),
   - `build/test/run/vendor` from root/member paths.
4. **Integration tests**:
   - workspace lockfile consistency,
   - shared `.eta/modules` and `.eta/target` behavior.
5. **Cross-platform CI**:
   - Windows/Linux/macOS workspace fixtures.

---

## 14) Risks and mitigations

1. **Root/member ambiguity**:
   - Mitigation: explicit context classification and clear error messages.
2. **Lockfile churn in monorepos**:
   - Mitigation: deterministic ordering and member-targeted update paths.
3. **Package name collisions across members**:
   - Mitigation: hard error during workspace resolution.
4. **Command UX complexity**:
   - Mitigation: conservative defaults and explicit selection flags.
5. **Backward compatibility regressions**:
   - Mitigation: preserve package-only path and keep existing parser helper API.

---

## 15) Acceptance criteria

Workspace support is complete when:

1. `[workspace]` manifests are parsed and validated,
2. workspace root/member discovery works deterministically,
3. one workspace `eta.lock` governs all selected members,
4. `eta build/test/run/vendor/update` operate correctly in workspace mode,
5. tooling uses workspace context where applicable,
6. single-package projects continue to behave exactly as before.

