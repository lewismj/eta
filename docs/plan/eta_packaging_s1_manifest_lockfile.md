## Eta Packaging S1 Manifest + Lockfile Core

This note records the S1 "manifest + lockfile core (path deps only)"
implementation from `docs/plan/eta_packaging_plan.md`.

### Landed scope

- Added `eta::package::Manifest` parsing and validation for `eta.toml`:
  - required fields: `[package].name`, `[package].version`,
    `[package].license`, `[compatibility].eta`
  - deterministic dependency ordering
  - S1 dependency format enforcement: `[dependencies]` entries must be
    inline tables using `path = "..."`
- Added `eta::package::Lockfile` parsing and deterministic serialization
  for `eta.lock` schema `version = 1`.
- Added path-only dependency graph resolution:
  - recursive manifest loading through local `path` dependencies
  - dependency key and package name consistency checks
  - duplicate package name and cycle detection
  - lockfile materialization from resolved graph
- Replaced `eta_pkg_test` smoke test with S1 coverage for:
  - manifest parse/validation failures
  - deterministic lockfile output
  - path-dependency graph resolution and lockfile generation

### S1 test gate

The S1 gate is satisfied when all of these are green:

- `eta_core_test`
- `eta_test`
- `eta_pkg_test`
- `eta_cli_test`

