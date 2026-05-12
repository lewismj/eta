# Import & `ETA_MODULE_PATH` Strategy

Status: **draft / proposal**
Owners: runtime + packaging
Scope: `(import …)` resolution, `ETA_MODULE_PATH`, native-sidecar discovery
Related code:
- `eta/core/src/eta/interpreter/module_path.h` (`ModulePathResolver`)
- `eta/core/src/eta/package/{discovery,resolver,manifest,lockfile}.h`
- `eta/core/src/eta/native/sidecar_loader.{h,cpp}`
- `eta/session/src/eta/session/driver.h`
  (`ensure_bundled_sidecars_loaded`, `ensure_package_sidecars_loaded`)
- `eta/cli/src/eta/cli/main_eta.cpp` (`join_module_path_entries`)
- `scripts/install.{ps1,sh}` and `editors/vscode/package.json`
  (settings: `eta.modulePath`, `eta.lsp.modulePath`)

---

## 1. Problem statement

Today `ETA_MODULE_PATH` (and the matching `--path` CLI / `eta.modulePath` editor
setting) is a PATH-style list of *raw source roots*. The resolver does the
following in order (`ModulePathResolver::from_args_or_env_at`):

1. Discover the nearest `eta.toml` walking up from the start dir, then add:
   - workspace member `src/` (and `target/release/`) directories,
   - `.eta/modules/<name>-<version>/{src, target/release}` driven by `eta.lock`,
   - the active package's `src/` (or its root as a fallback).
2. Append every directory listed in `--path` / `ETA_MODULE_PATH` verbatim.
3. Append `ETA_STDLIB_DIR` (CMake-injected dev path) and the bundled stdlib
   directory next to the executable.

Native sidecars are loaded by a separate pipeline
(`SidecarLoader` + `NativeLoadContext`) that is **only** wired into the active
package's lockfile closure or a hardcoded list of bundled stdlib packages
(`{log, stats, torch, nng}` in `driver.h`).

This breaks on real layouts. Concretely:

- The repo has **collections without their own manifest**:

  ```
  packages/
    stdlib/native/{log,nng,stats,torch}/eta.toml   (each is a package)
    db/native/{duckdb}/eta.toml                    (collection root, no manifest)
    ml/native/{lightgbm}/eta.toml                  (collection root, no manifest)
    example/hello-world/eta.toml
  ```

  There is no `eta.toml` at `packages/`, `packages/db/`, or
  `packages/db/native/`. They are *category directories*, not workspaces.

- A user that just wants `(import db.duckdb)` from a script outside any of these
  packages must today either (a) be inside an `eta.toml` whose lockfile pulls
  `eta-duckdb` in, or (b) hand-add the exact `packages/db/native/duckdb/src`
  directory to `ETA_MODULE_PATH`. Neither composes — adding a new sub-package
  under `packages/db/` requires editing every consumer's environment.

- Native sidecars under `packages/db/native/duckdb` are **never** loaded by
  ambient discovery. The driver only auto-loads bundled-stdlib sidecars via a
  hardcoded list. So `(import db.duckdb)` would resolve the source but fail at
  runtime when `%duckdb-open` is called.

- `eta.modulePath` in the VS Code extension and the installer-set
  `ETA_MODULE_PATH` both target a *single* directory (the bundled stdlib),
  which gives users no obvious extension point for additional package
  collections.

We need a single, predictable model where each `ETA_MODULE_PATH` entry can be:

1. a literal source root (today's behavior),
2. a single package root (a directory containing `eta.toml`),
3. a collection of package roots (a directory whose immediate children — or a
   conventional `native/` / `eta/` sub-tree — contain `eta.toml` files).

…and for every package discovered through (2) or (3), both the module source
roots **and** any `[native]` sidecars are wired up automatically, identically to
how lockfile-resolved dependencies work today.

---

## 2. Goals & non-goals

### Goals

- `(import db.duckdb)` and `(import std.stats)` should "just work" when
  `ETA_MODULE_PATH` (or `--path`) points at a sensible parent directory, with
  no per-package wiring.
- Native sidecar packages discovered through the path are loaded with the same
  containment + ABI checks as lockfile-resolved sidecars.
- Existing behavior is preserved: bare directories on `ETA_MODULE_PATH` continue
  to work, and lockfile-driven resolution remains authoritative when present.
- Resolution remains deterministic (sorted collection traversal, documented
  precedence) and finite (bounded recursion depth, cached scans).
- The strategy is uniform across `etai`, `eta_repl`, `etac`, `eta_test`,
  `eta_lsp`, `eta_dap`, `eta_jupyter`, and the build helpers.

### Non-goals

- Replacing the lockfile resolver. Path-discovered packages do **not** generate
  or consume `eta.lock`; they are an "ambient" overlay used for development,
  scripts, and the bundled stdlib.
- Network fetching / version solving from the path. Discovery is local-only;
  the existing `git`/`tarball` dependency machinery remains unchanged.
- Changing the on-disk package layout of existing packages.

---

## 3. Layout conventions (formalized)

A **package directory** is any directory that contains `eta.toml` with a
`[package]` table. Its module sources live under `src/` (preferred) or directly
under the package root (legacy fallback retained today by
`add_package_layout_dirs`). Its compiled artifacts live under `target/release/`.
Native packages additionally declare `[native]` and `[[native.targets]]` rows
and ship their shared libraries at `<package_root>/<artifact-relpath>` (rules
already enforced by `resolve_native_sidecars`: relative path, no escape, host
triple match, optional `sha256`).

A **collection directory** is any directory that does *not* itself contain
`eta.toml` but contains package directories one or two levels below it.
Two recognised shapes (both already used in-tree):

```
<col>/<pkg>/eta.toml                 # flat: e.g. packages/example/hello-world
<col>/native/<pkg>/eta.toml          # native subgroup: e.g. packages/db/native/duckdb
<col>/eta/<pkg>/eta.toml             # pure-eta subgroup (reserved)
```

Discovery is **bounded to depth 3** from a collection root and stops at the
first directory that contains `eta.toml` (so nested workspaces are not
recursively flattened). This keeps the existing repo layout valid without
changes and gives third parties a documented place to drop new packages.

---

## 4. Path-entry grammar

Each entry on `ETA_MODULE_PATH` / `--path` becomes a typed entry:

| Sigil           | Meaning                                                 |
| --------------- | ------------------------------------------------------- |
| *(none)*        | **Auto** — see resolution rules below                   |
| `dir+<path>`    | Literal source root (today's behavior, no scanning)     |
| `pkg+<path>`    | Single package root (must contain `eta.toml`)           |
| `pkgs+<path>`   | Collection root (scan to depth 3 for `eta.toml` files)  |

`dir+`, `pkg+`, `pkgs+` are explicit and machine-friendly. They are optional —
the **Auto** rule is what ordinary users get and is sufficient for almost every
case:

1. If `<path>` does not exist → ignore (warn at `--verbose`).
2. If `<path>/eta.toml` exists → treat as `pkg+`.
3. Else if any of `<path>/*/eta.toml`, `<path>/*/native/*/eta.toml`,
   `<path>/*/eta/*/eta.toml` exist → treat as `pkgs+`.
4. Otherwise → treat as `dir+` (legacy behavior; the path is added verbatim as
   a source root, exactly as today).

This makes today's installer (`ETA_MODULE_PATH=…/stdlib`) keep working: that
directory becomes a `dir+` entry because there is no `eta.toml` directly under
it. To opt the entire `packages/` tree in, a user writes:

```text
ETA_MODULE_PATH=C:\src\eta\packages
```

and the resolver auto-scans it as a collection.

The PATH separator stays platform-conventional (`;` on Windows, `:` elsewhere)
as today (`from_path_string`). Sigils are case-sensitive and lowercase.

---

## 5. Resolution semantics

### 5.1 Source roots

For every package discovered through a `pkg+` / `pkgs+` (or auto-detected
equivalent) entry the resolver injects, in order:

1. `<package_root>/target/release` (only if it exists),
2. `<package_root>/src` (only if it exists),
3. `<package_root>` (legacy fallback if neither of the above exists).

This is exactly what `add_package_layout_dirs` already does for lockfile-driven
packages and ensures bytecode (`.etac`) wins over source as configured by
`prefer_source_`.

### 5.2 Search precedence

Final resolver `dirs_` is built in this order (first match wins):

1. **Active project context** (today's behavior): workspace member roots,
   `.eta/modules/<pkg>-<ver>/...` from `eta.lock`, then the active package's
   own `src/`.
2. **Configured entries** (the parsed `--path` / `ETA_MODULE_PATH`), in user
   order, with each entry expanded as in §5.1.
3. **`ETA_STDLIB_DIR`** (CMake build-tree fallback for developer binaries).
4. **Bundled stdlib next to the executable** (`bundled_stdlib_dir()`), itself
   expanded via the **Auto** rule so the bundled `packages/stdlib/native/*`
   collection is picked up uniformly (replaces the hardcoded
   `{log, stats, torch, nng}` list in `driver.h`).

Within (1) the lockfile is authoritative, so a `pkgs+` entry that reintroduces
a package name already pinned by the lockfile is **shadowed** for source
resolution (matching current `add_unique_dir` deduping by canonical path) but
its sidecars are still considered for the ambient pool below.

### 5.3 Duplicate package names

When a `pkgs+` scan or a combination of entries yields two packages with the
same `name`, the first occurrence in path order wins for source resolution.
A diagnostic (`eta-modulepath/duplicate-package`) is emitted via the LSP
`eta-manifest` channel and via `etai --verbose` in CLI mode. This mirrors the
existing `package_seen.insert(...)` guard in
`ensure_bundled_sidecars_loaded` but extends it to user-supplied roots.

### 5.4 Bounded scan + caching

Each `pkgs+` entry is scanned once per resolver instance. Results are memoised
keyed by the canonical entry path so that LSP/DAP long-running processes do
not re-walk `packages/` on every keystroke. A simple `mtime` watch on the entry
root invalidates the cache when new packages appear. Recursion depth is capped
at 3 (`<col>/<pkg>` and `<col>/{native|eta}/<pkg>`), and any directory holding
an `eta.toml` terminates descent for that branch.

---

## 6. Native sidecar discovery

Today there are two sidecar-loading paths in `driver.h`:

- `ensure_package_sidecars_loaded`: lockfile-driven, bound to one active
  package context.
- `ensure_bundled_sidecars_loaded`: hardcoded list of stdlib package names
  scanned under bundled `packages/stdlib/native/<dir>`.

We introduce a third, **path-driven**, that runs after the bundled pass when
no lockfile context selected sidecars (or as a complement when the active
context is a workspace virtual root):

```text
for each pkg+/pkgs+ entry on ETA_MODULE_PATH:
    for each discovered package root:
        if manifest has [native]:
            select target for host triple
            verify artifact relpath stays inside package_root
            verify file exists
            optionally verify sha256 (skipped when zeroed, like today)
            push NativeSidecarSpec into the ambient pool
```

The ambient pool then runs through the **same**
`resolve_native_sidecars` + `SidecarLoader::load` codepath used by the lockfile
loader. This guarantees:

- ABI strings still gate compatibility (`eta-native-v1`).
- Containment + checksum invariants are preserved.
- The extension registry treats path-loaded and lockfile-loaded sidecars
  identically (same `make_registered_sidecar_primitive` wiring).

Conflict policy (matches lockfile pass):

- If a sidecar with the same `id`/`abi` is already registered, the second is
  rejected with `RegistryConflict` and a `eta-modulepath/sidecar-conflict`
  diagnostic.
- If a builtin sidecar placeholder exists for the package (as routed by
  `runtime::builtin_native_sidecar_package(...)` during
  `register_all_primitives(...)`), it is overwritten via
  `builtins_.overwrite_func`, exactly as today.

Bundled stdlib sidecars stop using the hardcoded
`builtin_sidecar_dirs = { "log", "stats", "torch", "nng" }` array — they fall
out of the auto-`pkgs+` expansion of `bundled_stdlib_dir()`. Backwards
compatibility for non-stdlib builtins (`is_log_primitive_name`, …) is
preserved as a *placeholder registration* layer; if the path scan registers a
real sidecar for that package, the placeholders are overwritten as today.

---

## 7. Implementation plan

The work splits into four reviewable changes:

### 7.1 `ModulePathResolver` entry parsing (eta/core)

- Add `enum class ModulePathEntryKind { Dir, Package, Collection }`.
- Replace `from_path_string` with a parser that recognises the `dir+` /
  `pkg+` / `pkgs+` sigils and yields `ModulePathEntry { kind, path }`.
- Add `expand_entries(...)` that turns the typed list into:
  - `std::vector<fs::path> source_roots`
  - `std::vector<DiscoveredPackage> packages` (name, manifest, package root,
    optional `[native]` view)
- Cap collection traversal depth at 3 and stop at the first `eta.toml`.
- Memoise results per canonical entry path for the lifetime of the resolver.
- Keep `from_args_or_env_at` as the single entry point used by every tool.

### 7.2 Driver wiring (eta/session)

- Add `ensure_path_sidecars_loaded()` that consumes
  `ModulePathResolver::discovered_packages()` and feeds them through the
  existing `resolve_native_sidecars` + `SidecarLoader::load` pipeline.
- Call order in `ensure_package_sidecars_loaded`:
  1. lockfile pass (unchanged),
  2. path pass (new),
  3. bundled pass (now derived from `pkgs+` over `bundled_stdlib_dir()`).
- Drop `builtin_sidecar_dirs` — the bundled scan re-uses the same code path
  by treating the bundled stdlib root as a `pkgs+` entry.
- Diagnostics: route duplicates / conflicts through `diag_engine_` with
  stable codes (`eta-modulepath/duplicate-package`,
  `eta-modulepath/sidecar-conflict`, `eta-modulepath/invalid-entry`).

### 7.3 CLI / tooling surface

- `eta_repl`, `etai`, `etac`, `eta_test`, `eta_dap`, `eta_lsp` already funnel
  through `ModulePathResolver::from_args_or_env*`. No call-site changes are
  needed beyond those tools picking up the new behavior.
- `eta` driver (`main_eta.cpp::join_module_path_entries`) gains awareness of
  the sigils so that `eta build`/`eta test` propagate path entries verbatim.
- `editors/vscode/package.json`: document `eta.modulePath` accepts sigils;
  no schema change needed — the extension just forwards the string.
- Installer scripts (`scripts/install.{ps1,sh}`): keep setting
  `ETA_MODULE_PATH` to the bundled stdlib (treated as `dir+` today, will be
  treated as `dir+` still by Auto rule because the bundled stdlib root has
  no `eta.toml` directly under it). No installer change required.
- Test runner: replace its bespoke sidecar fixture wiring with a `pkgs+`
  entry pointing at the generated fixture root.

### 7.4 Documentation & QA

- Update `docs/guide/packages.md`:
  - rewrite the "Runtime sidecar loading" section to describe the three
    passes (lockfile / path / bundled),
  - document the path grammar and the Auto rule,
  - call out duplicate / conflict diagnostics.
- Update `docs/quickstart.md` and `TLDR.md` to mention the recommended
  developer setting:
  `ETA_MODULE_PATH=<install>/stdlib;<repo>/packages` (or POSIX `:`).
- Add `eta/qa/test/src/module_path_resolver_tests.cpp` cases:
  - auto-detect `pkg+` from a directory containing `eta.toml`,
  - auto-detect `pkgs+` from a `packages/db/`-shaped tree,
  - explicit `dir+` keeps current literal behavior,
  - depth-3 cap enforced,
  - duplicate package names: first wins + diagnostic,
  - bundled stdlib auto-expansion replaces the hardcoded list.
- Add `eta/qa/test/src/native_sidecar_loader_tests.cpp` cases for the path
  pass: containment escape, sha mismatch, host-triple miss, ABI mismatch.
- Add a cookbook recipe: dropping a new package under `packages/ml/native/`
  and consuming it from a stand-alone script via `ETA_MODULE_PATH=packages`.

---

## 8. Worked examples

### 8.1 Repo developer

```powershell
$env:ETA_MODULE_PATH = "$pwd\packages;$pwd\stdlib"
etai cookbook\ml\torch.eta
```

- `packages` is auto-detected as `pkgs+` (no `eta.toml`, but has
  `*/eta.toml` and `*/native/*/eta.toml` underneath).
- The resolver discovers `eta-duckdb`, `eta-lightgbm`, every
  `packages/stdlib/native/*` package, and every `packages/example/*` package.
- Source roots from each `src/` are added; native sidecars are loaded
  through the path pass with full integrity checks.
- `(import db.duckdb)` resolves to
  `packages/db/native/duckdb/src/db/duckdb.eta`; `%duckdb-open` is bound to
  the `eta_duckdb.dll` loaded via `eta.duckdb.sidecar`.

### 8.2 End user with installed Eta

Installer sets `ETA_MODULE_PATH=<install>/stdlib`. The bundled stdlib
directory ships package roots under
`<install>/stdlib/packages/stdlib/native/*` (or wherever the installer stages
them), and the resolver's bundled-stdlib step uses the `pkgs+` expansion to
load them — identical to repo developer behavior, no extra env-var needed.

### 8.3 Adding a third-party package collection

A team publishes `acme-eta-pkgs` as a directory of packages. Users add:

```bash
export ETA_MODULE_PATH="$HOME/acme-eta-pkgs:$ETA_MODULE_PATH"
```

The Auto rule treats it as `pkgs+`, every package's `src/` joins the
resolver, and any native artifacts they ship are loaded with sha256
verification when present.

---

## 9. Open questions

- Should `pkgs+` follow workspace `eta.toml` files (i.e. a collection entry
  that itself is a `[workspace]`) and re-use the workspace member resolver?
  Proposal: yes — if a discovered `eta.toml` has `[workspace]`, recurse via
  `resolve_workspace_members` instead of expanding manually.
- Do we want a `--path-trace` flag that prints the final ordered list of
  source roots and discovered sidecars? Proposed yes; cheap to add and
  invaluable for support.
- Should we expose a programmatic listing through LSP
  (`eta/modulePath/explain`) similar to `eta/lockfile/explain`? Proposed
  follow-up once §7.1–7.3 land.

