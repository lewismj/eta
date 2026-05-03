## Eta Packaging S6 Dependency Workflows

This note records the S6 "dependency workflows" implementation from
`docs/plan/eta_packaging_plan.md`.

### Landed scope

- Extended manifest dependency support:
  - `[dependencies]` and `[dev-dependencies]` now accept:
    - `path = "..."`
    - `git = "...", rev = "<40-hex>"`
    - `tarball = "...", sha256 = "<64-hex>"`
- Added deterministic manifest writer support in `eta::package` so CLI mutation
  (`add`, `remove`) can round-trip `eta.toml` safely.
- Extended resolver support:
  - new `resolve_dependencies(...)` options include:
    - optional dev-dependency inclusion
    - lockfile-backed module lookup for non-path sources
    - dependency locator callback for materializing non-path sources
  - `resolve_path_dependencies(...)` remains as a compatibility wrapper.
- Implemented S6 umbrella CLI subcommands:
  - `eta add`
  - `eta remove`
  - `eta update`
  - `eta build`
  - `eta test`
  - `eta bench`
  - `eta vendor`
  - `eta install`
  - `eta clean`
- Implemented lockfile-to-module materialization flow:
  - `.eta/modules/<name>-<version>/...` is synchronized from `eta.lock`
  - path dependencies are copied from source roots
  - git and tarball dependencies are cached under `~/.eta/cache/modules/`
    and materialized into project-local module roots
- Added canonical package smoke fixture:
  - `packages/example/hello-world/`
  - includes `eta.toml`, `src/hello_world.eta`, and `tests/smoke.test.eta`
- Expanded CLI and package tests for S6 workflows:
  - add/remove manifest + lockfile mutation
  - build artifact generation
  - test command execution
  - vendor materialization
  - clean lifecycle behavior
  - resolver callback path for non-path dependency kinds

### S6 test gate

The S6 gate is satisfied when all of these are green:

- `eta_core_test`
- `eta_test`
- `eta_pkg_test`
- `eta_cli_test`
