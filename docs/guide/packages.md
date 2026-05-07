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

## Layout

`eta build` writes artifacts to `.eta/target/<profile>/` for standalone packages.
In workspace mode, member artifacts use a shared layout:
`.eta/target/<profile>/<member-name>/...` under the workspace root.
`eta vendor` materializes dependencies under `.eta/modules/` in lockfile order
(workspace commands use the workspace root by default).

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
