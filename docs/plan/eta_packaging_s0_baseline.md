## Eta Packaging S0 Baseline

This note records the S0 "contract freeze + harness" implementation from
`docs/plan/eta_packaging_plan.md`.

### Landed scope

- Added ModulePathResolver regression coverage for:
  - `from_args_or_env` CLI-over-env precedence.
  - compile-time stdlib fallback inclusion.
- Added prelude loading regression coverage for:
  - missing `prelude.eta` handling.
  - idempotent `load_prelude()` behavior (no module re-execution).
- Added `.etac` load/execute regression coverage for:
  - successful module execution with auto-loaded imports.
  - clear diagnostic on missing imported modules.
- Added new placeholder harness targets:
  - `eta_pkg_test`
  - `eta_cli_test`

### S0 test gate

The S0 gate is satisfied when all of these are green:

- `eta_core_test`
- `eta_test`
- `eta_pkg_test`
- `eta_cli_test`
