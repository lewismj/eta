# Packages

Eta package workflows are driven by `eta.toml` and `eta.lock`.

## Core commands

```console
eta add <pkg> --path <dir>
eta remove <pkg>
eta update
eta build
eta test
eta vendor
eta clean --all
```

## Workspace selection flags

These flags are supported by workspace-aware commands (`tree`, `build`, `test`,
`bench`, `run`, `vendor`, `clean`, `update`, `add`, `remove`, `install`):

```console
--workspace
-p, --package <name>
--exclude <name>
--manifest-path <path>
```

Rules:

- `--workspace` selects all workspace members.
- `-p/--package` selects one or more named workspace members.
- `--exclude` is valid only with `--workspace`.
- `--manifest-path` points to an explicit package or workspace `eta.toml`.
- Single-target commands (`run`, `add`, `remove`, `install`) require exactly
  one selected package.

Default selection behavior:

- from a workspace member directory: default target is the current member;
  `--workspace` expands to all members.
- from a workspace root:
  - `default-members` (if configured) are used for aggregate commands,
  - otherwise rooted workspaces default to the root package,
  - otherwise virtual workspaces default to all members for aggregate commands.
- from a workspace non-member directory:
  - aggregate commands follow workspace-root defaults,
  - single-target commands require `-p/--package`.

## Dependency source forms

```toml
[dependencies]
local_dep = { path = "../local_dep" }
git_dep = { git = "https://example.com/repo.git", rev = "0123456789abcdef0123456789abcdef01234567" }
tar_dep = { tarball = "../dep.tar.gz", sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" }
```

`rev` must be a full 40-character git commit id. `sha256` must be a 64-character hex digest.

## Native sidecar metadata

Package manifests can declare native sidecar artifacts:

```toml
[native]
kind = "sidecar"
abi = "eta-native-v1"
id = "example_native"
entry = "eta_register_extension_v1"

[[native.targets]]
triple = "x86_64-pc-windows-msvc"
artifact = "native/windows-x64/eta_example_native.dll"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
```

Rules:

- `[native]` is valid only for manifests that also declare `[package]`.
- `kind`, `abi`, `id`, and `entry` are required.
- Each `[[native.targets]]` row requires `triple`, relative `artifact`, and `sha256`.
- Duplicate target triples in one package are rejected.

When present, lockfile package rows may also include:
`native_id`, `native_abi`, `native_entry`, `native_target_triple`,
`native_artifact_relpath`, and `native_sha256`.
The resolver selects the `[[native.targets]]` row matching the current target
triple when writing these lockfile fields.

## Runtime sidecar loading

`etai`, `eta_repl`, and `etac` load sidecars before compile/run.

Package context behavior:

- Discover package/workspace context from the active start directory.
- Read `eta.lock`, build dependency closure from the active package, and select
  closure-reachable packages with complete `native_*` lockfile fields.
- Resolve `native_artifact_relpath` against each package root, enforce
  containment, verify `native_sha256`, then load in lockfile order.

Non-package behavior:

- If no active package context (or no package lockfile) is available, runtime
  attempts bundled stdlib sidecar discovery under `packages/stdlib/native/*`
  on resolver/installation paths.
- Bundled stdlib manifests currently cover first-party sidecars
  (`log`, `stats`, `torch`, `nng`).

Notes:

- Sidecars are loaded by manifest/lockfile metadata and artifact presence.
- Imports resolve normally; when an imported module depends on sidecar-backed
  primitives, those primitives must already be loadable through the current
  package or bundled context.

## Layout

`eta build` writes artifacts to `.eta/target/<profile>/` for standalone packages.
In workspace mode, member artifacts use a shared layout:
`.eta/target/<profile>/<member-name>/...` under the workspace root.
`eta vendor` materializes dependencies under `.eta/modules/` in lockfile order
(workspace commands use the workspace root by default).
For non-workspace dependencies that carry native metadata, materialization
verifies `native_sha256` before commands proceed.

## Tooling integration

- `eta_repl`, `eta_lsp`, `eta_dap`, and `eta_jupyter` are workspace-aware:
  - from a workspace member, tooling resolves modules using the workspace root
    lockfile/modules roots plus workspace member source roots,
  - from a workspace root, tooling loads member roots from
    `[workspace].members` even when `eta.lock` is absent.
- LSP publishes package metadata diagnostics:
  - `eta-manifest` for `eta.toml` parse/validation/resolution issues.
  - `eta-lockfile` for `eta.lock` parse/validation issues.
- `eta/lockfile/explain` now includes workspace context fields
  (`context`, `workspaceManifestPath`, `packageManifestPath`, `lockfilePath`)
  when running in workspace mode.
- DAP launch defaults to `profile = "debug"` so source-level debugging keeps
  debug spans, while `eta build`/`eta run` continue to default to release.
