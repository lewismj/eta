## Eta Packaging S7 Tooling Integration

This note records the S7 "tooling integration" implementation from
`docs/plan/eta_packaging_plan.md`.

### Landed scope

- Module-path discovery can now be anchored to an explicit filesystem start
  directory via `ModulePathResolver::from_args_or_env_at(...)`.
  - This allows tools to resolve package roots from the active file/workspace,
    not only from process CWD.
- `eta_repl` now accepts `--strict-shadows` and passes strict shadow policy
  through the resolver stack (parity with `etai` / `eta run --strict-shadows`).
- LSP workspace integration:
  - Workspace resolver roots are refreshed from the active document URI by
    walking upward to `eta.toml`.
  - Dependency resolution uses package-local `.eta/modules/...` roots from
    `eta.lock` when available.
  - New custom request `eta/lockfile/explain` reports resolver roots and
    selected module candidate for a requested module name.
  - Manifest and lockfile diagnostics are now published through LSP:
    - `eta-manifest` diagnostics for manifest parse/validation and resolver
      failures.
    - `eta-lockfile` diagnostics for lockfile parse/validation failures.
- DAP workspace/profile integration:
  - Launch supports `profile` (`debug` or `release`), defaulting to `debug`.
  - Adapter output now includes launch profile in startup logs.
  - Resolver roots are now anchored from the launched program path so package
    dependencies resolve from the program workspace even when adapter CWD is
    elsewhere.
- Jupyter startup resolver initialization now explicitly anchors discovery at
  kernel startup CWD through `from_args_or_env_at(...)`.
- VS Code debug defaults:
  - Launch config supports `profile` with default `debug`.
  - Built-in "Run Eta file" / "Debug Eta file" commands set `profile: "debug"`.
  - Launch request tracing now logs program + profile.

### S7 test coverage additions

- `module_path_tests`:
  - explicit-start discovery test for `from_args_or_env_at(...)`.
- `lsp_tests`:
  - `eta/lockfile/explain` resolver response over package fixtures.
  - manifest diagnostics (`eta-manifest`) publication.
  - lockfile diagnostics (`eta-lockfile`) publication.
- `dap_tests`:
  - launch default profile regression (`profile=debug`).
  - program-workspace dependency resolution from launch path when adapter CWD
    is outside the package.
- `driver_jupyter_test`:
  - startup resolver discovery from package `eta.lock` materialized dependency
    roots.

### S7 test gate

The S7 gate is satisfied when all of these are green:

- `eta_core_test`
- `eta_test`
- `eta_pkg_test`
- `eta_cli_test`

