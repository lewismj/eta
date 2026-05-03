## Eta Packaging S3 Runtime Resolver Integration

This note records the S3 "runtime resolver integration" implementation from
`docs/plan/eta_packaging_plan.md`.

### Landed scope

- Extended `ModulePathResolver::from_args_or_env` with project discovery:
  - walks up from current working directory to find `eta.toml`
  - injects `<project>/src` first
  - injects `<project>/.eta/modules/<name>-<version>/{target/release,src}`
    in `eta.lock` order when lockfile is present
  - keeps env/CLI roots and stdlib fallbacks after project-local roots
- Updated module lookup precedence:
  - per root, resolver now checks `.etac` before `.eta`
  - preserves first-hit semantics in default mode
- Added strict shadow scan mode:
  - `ModulePathResolver` now tracks `strict_shadow_scan`
  - import resolution fails with a clear diagnostic when multiple module files
    match in strict mode
  - exposed via `etai --strict-shadows`
  - exposed in umbrella CLI passthrough via `eta run --strict-shadows`
- Updated runtime import loading:
  - `Driver` now dispatches resolved imports by extension (`.etac` uses
    `run_etac_file`, `.eta` uses `run_file`)
  - strict-shadow failures are surfaced as diagnostics during import loading
- Added S3 coverage in tests:
  - `module_path_tests`: project-root/module-root injection and `.etac` lookup
    preference
  - `eta_cli_test`: default first-hit behavior and strict-shadow failure mode

### S3 test gate

The S3 gate is satisfied when all of these are green:

- `eta_core_test`
- `eta_test`
- `eta_pkg_test`
- `eta_cli_test`
