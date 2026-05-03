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

## Dependency source forms

```toml
[dependencies]
local_dep = { path = "../local_dep" }
git_dep = { git = "https://example.com/repo.git", rev = "0123456789abcdef0123456789abcdef01234567" }
tar_dep = { tarball = "../dep.tar.gz", sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" }
```

`rev` must be a full 40-character git commit id. `sha256` must be a 64-character hex digest.

## Layout

`eta build` writes artifacts to `.eta/target/<profile>/`. `eta vendor` materializes dependencies under `.eta/modules/` in lockfile order.

## Tooling integration

- `eta_repl`, `eta_lsp`, `eta_dap`, and `eta_jupyter` discover package roots by
  walking upward to `eta.toml` and then applying `eta.lock` module roots.
- LSP publishes package metadata diagnostics:
  - `eta-manifest` for `eta.toml` parse/validation/resolution issues.
  - `eta-lockfile` for `eta.lock` parse/validation issues.
- DAP launch defaults to `profile = "debug"` so source-level debugging keeps
  debug spans, while `eta build`/`eta run` continue to default to release.
