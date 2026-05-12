# Import Refactor Plan (`ETA_MODULE_PATH` + Stage 12 Closure)

Status: **draft / proposed**
Owners: runtime + packaging + CLI
Primary outcome: package imports should "just work" when `ETA_MODULE_PATH` includes one or more top-level package directories.

---

## 1. Why this plan exists

This plan consolidates:

1. Import/module-path behavior needed for seamless package resolution.
2. Remaining Stage 12 work from `docs/plan/driver_refactor_plan.md`.
3. Corrections from review of `docs/plan/import_strategy.md` where ownership and sequencing were inconsistent with the driver refactor.

It is intentionally user-contract-first: users should not need to manually add each package `src/` directory to get imports working.

---

## 2. User-facing contract (must hold)

1. `ETA_MODULE_PATH` may contain many entries (platform separator: `;` on Windows, `:` on POSIX).
2. A user can add a top-level package directory (for example `<repo>/packages`) and imports from packages under that tree resolve automatically.
3. This behavior is consistent across `etai`, `etac`, `eta_repl`, `eta_test`, `eta_lsp`, `eta_dap`, and `eta_jupyter`.
4. Native sidecars discovered from path entries load through the same ABI and containment checks as lockfile-discovered sidecars.
5. Resolution is deterministic: path-entry order is stable and duplicate-package behavior is documented.

Example intent:

```text
ETA_MODULE_PATH=C:\src\eta\packages;D:\team\eta-packages
```

With this, imports such as `(import db.duckdb)` and `(import ml.lightgbm)` should resolve without adding each package subdirectory explicitly.

---

## 3. Corrections to apply from review findings

1. Keep import/path sidecar orchestration out of `Driver` internals; ownership remains in `NativeSidecarManager` (Stage 9 boundary).
2. Use one explicit sidecar sequencing policy and document it once (no contradictory ordering text).
3. Include missing test coverage for precedence, ambiguity, and cross-platform path normalization.
4. Explicitly include `eta_jupyter` in tooling parity for module-path behavior.
5. Include all Stage 12 items, not only module-path semantics.

---

## 4. Canonical `ETA_MODULE_PATH` semantics

Each path entry is typed as one of:

- `dir+<path>`: literal source root (legacy behavior).
- `pkg+<path>`: single package root (`eta.toml` at root).
- `pkgs+<path>`: package collection root (scan bounded depth for package roots).
- no sigil: auto mode.

Auto-mode rules:

1. Missing path -> ignored (verbose diagnostic only).
2. `<path>/eta.toml` exists -> `pkg+`.
3. Otherwise, if package manifests exist under collection shapes (for example `<path>/*/eta.toml`, `<path>/*/native/*/eta.toml`, `<path>/*/eta/*/eta.toml`) -> `pkgs+`.
4. Otherwise -> `dir+`.

Collection scan rules:

1. Bounded recursion depth: 3.
2. Stop descending a branch once a directory with `eta.toml` is found.
3. Canonicalize paths before dedupe.
4. Cache scan results per resolver instance; invalidate with root mtime change.

Source-root expansion for discovered packages:

1. layout-resolved bytecode directory (if present),
2. `src/` (if present),
3. package root fallback (legacy).

Search precedence (first match wins):

1. Active project context (workspace + lockfile closure + active package).
2. Parsed `ETA_MODULE_PATH`/`--path` entries in user order.
3. `ETA_STDLIB_DIR`.
4. Bundled stdlib near executable.

Duplicate package names:

1. First in precedence order wins for import resolution.
2. Emit stable diagnostic code for duplicates.

---

## 5. Native sidecar loading model

Single canonical pass order:

1. lockfile-selected sidecars (active project context),
2. path-discovered sidecars (from `pkg+`/`pkgs+` expansion),
3. bundled stdlib sidecars.

Requirements:

1. All passes use the same `resolve_native_sidecars` and loader contracts.
2. ABI compatibility checks remain mandatory.
3. Artifact path containment checks remain mandatory.
4. Optional checksum validation behavior remains consistent with current semantics.
5. Conflict policy is deterministic and diagnosed with stable diagnostic codes.

Ownership:

1. `NativeSidecarManager` owns sidecar discovery/orchestration policy.
2. `Driver` remains composition root and forwarding surface only.

---

## 6. Stage 12 integration (full scope)

### 6.1 Stage 12.1: `std.prelude` messaging cleanup

1. Audit user-facing diagnostics, fallback messages, and module-name checks.
2. Remove stale instructions telling users to load/import `std.prelude`.
3. Keep compatibility behavior where required, but use neutral bootstrap wording.
4. Add focused diagnostic tests.

### 6.2 Stage 12.2: module-path semantics and package-root entries

1. Implement/document canonical entry grammar and auto-detection rules.
2. Ensure top-level package directory behavior works for imports without per-package wiring.
3. Add tests for precedence, ambiguity, duplicate names, and path normalization across Windows/POSIX style input.
4. Ensure all tools listed in section 2 share the same resolver entrypoint behavior.

### 6.3 Stage 12.3: layout-token cleanup (`release`/`target`)

1. Audit executable/artifact discovery and module-root expansion for hardcoded layout literals.
2. Introduce centralized layout-resolution utility/config API.
3. Route import and sidecar path composition through this utility.
4. Keep compatibility defaults so current installs/builds continue to work.

### 6.4 Stage 12.4: `eta --version` contract

1. Implement `eta --version` behavior as documented.
2. Add CLI contract tests for output presence and format.
3. Ensure behavior is stable across default and packaged runtime layouts.

---

## 7. Implementation workstreams

### A) Core resolver (`eta/core`)

1. Typed module-path entry parsing + auto detection.
2. Collection discovery, dedupe, caching, diagnostics.
3. Stable export of discovered package metadata for sidecar manager consumption.

### B) Sidecar orchestration (`eta/core/native` + session wiring)

1. Path-discovered sidecar pass integrated into `NativeSidecarManager`.
2. Remove reliance on hardcoded bundled sidecar package lists.
3. Keep `Driver` API as thin forwarding only.

### C) CLI/tools parity (`eta/cli`, tool mains, editor docs)

1. Ensure `--path`/`ETA_MODULE_PATH` propagation preserves typed entries.
2. Confirm parity for `etai`, `etac`, `eta_repl`, `eta_test`, `eta_lsp`, `eta_dap`, `eta_jupyter`.
3. Document recommended developer path setups for multi-root usage.
4. Implement/test `eta --version`.

### D) Docs + QA

1. Update package/import docs with canonical semantics and examples.
2. Add matrix tests for:
   - top-level package directory import success,
   - multi-entry path behavior and precedence,
   - duplicate-package ambiguity diagnostics,
   - sidecar conflict diagnostics,
   - cross-platform path normalization cases,
   - stale `std.prelude` diagnostics absent.

---

## 8. Acceptance criteria

1. Adding a top-level package directory to `ETA_MODULE_PATH` enables imports from packages under that directory without per-package path edits.
2. Sidecars for those imported packages load through the unified managed pipeline with existing safety checks.
3. No user-facing diagnostics instruct users to import/load `std.prelude`.
4. Runtime-critical path handling does not depend on ad hoc hardcoded `release`/`target` tokens.
5. `eta --version` is implemented and covered by a contract test.
6. Behavior is coherent across `etai`, `etac`, `eta_repl`, and `eta_jupyter` (plus other tools sharing resolver entrypoints).

---

## 9. Rollout order

1. Land resolver typing + discovery + tests.
2. Land sidecar-manager integration and remove hardcoded bundled list behavior.
3. Land layout-token centralization and migrate call sites.
4. Land diagnostics wording cleanup (`std.prelude`) and tests.
5. Land `eta --version` plus CLI contract tests.
6. Update docs and examples last, after behavior is verified.

