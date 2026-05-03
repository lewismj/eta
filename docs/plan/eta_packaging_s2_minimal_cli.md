## Eta Packaging S2 Minimal `eta` CLI

This note records the S2 "minimal `eta` umbrella CLI" implementation from
`docs/plan/eta_packaging_plan.md`.

### Landed scope

- Added a new `eta` umbrella binary at `eta/cli/`.
- Implemented S2 subcommands:
  - `eta new <name> [--bin|--lib]`
  - `eta init [--bin|--lib]`
  - `eta tree [--depth N]`
  - `eta run [--profile <release|debug>] [--bin NAME] [--example NAME] [file.eta] [-- args...]`
- `eta run` behavior in S2:
  - file mode (`eta run file.eta`) degrades to `etai file.eta`
  - manifest mode discovers `eta.toml`, resolves path dependencies, builds a
    deterministic module path, and invokes `etai` with that path
- Added explicit `NotYetImplemented(stage=...)` handling for out-of-scope
  subcommands in this stage.
- Replaced `eta_cli_test` smoke coverage with S2 CLI coverage for:
  - project scaffolding layout (`new`, `init`)
  - deterministic dependency tree output (`tree`)
  - no-manifest `run` compatibility with `etai`

### S2 test gate

The S2 gate is satisfied when all of these are green:

- `eta_core_test`
- `eta_test`
- `eta_pkg_test`
- `eta_cli_test`
