# eta_lint user guide

`eta_lint` is the Eta linter CLI. It reads `.eta-lint.toml` when present,
resolves the active rule set, and reports findings in one of four output
formats:

- `pretty` - human-readable terminal output
- `json` - machine-readable diagnostics and coverage notes
- `sarif` - SARIF 2.1.0 for code-scanning integrations
- `github` - GitHub command annotations

## Common options

- `--list-rules` prints the active registry with status, severity, analysis
  level and preset membership.
- `--explain ETA401` prints the reviewed metadata, prerequisites, false-positive
  notes, fix policy and fixture inventory for one rule.
- `--preset {minimal|recommended|strict|all}` selects the compiled rule set.
- `--config PATH` loads one `.eta-lint.toml` file.
- `--rules CODE[,CODE...]` enables only the listed rules.
- `--no-rule CODE[,CODE...]` disables rules after preset resolution.
- `--format {pretty|json|sarif|github}` selects the reporter.
- `--stdin` lints standard input instead of files.
- `--quiet` suppresses info-level diagnostics in pretty output.

## Directives

The linter recognises `eta-lint` directives only in ordinary line comments.
Supported forms are:

```scheme
;; eta-lint:disable=ETA301
;; eta-lint:disable-line=ETA401
;; eta-lint:disable-file=ETA104
;; eta-lint:enable=ETA301
```

- `disable` suppresses the next syntactic form and its descendants.
- `enable` cancels an inherited suppression for the next syntactic form and its
  descendants.
- `disable-line` suppresses diagnostics on the directive's physical source line.
- `disable-file` suppresses the whole file, but only when it appears before the
  first active form.

Unknown rule codes or malformed directives produce directive diagnostics and a
configuration-style exit status of 2.

## Configuration

The fixed config file format uses TOML-like sections:

```toml
[eta-lint]
preset = "recommended"
max-warnings = 10
include = ["stdlib/**"]
exclude = ["**/generated/**"]

[rules]
ETA104 = "off"
```

- `preset` becomes the default compiled preset when the CLI did not specify one.
- `max-warnings` raises the exit class once the warning count exceeds the limit.
- `include` and `exclude` add discovery filters.
- `[rules]` currently accepts `off` to disable a specific registered rule.

The CLI merges config values after parsing command-line options and before rule
resolution, so explicit CLI rule selections still take effect.

## Reporter behavior

- `pretty` preserves stable file ordering and prints coverage notes.
- `json` emits the full diagnostic and coverage payload.
- `sarif` emits a SARIF 2.1.0 document with one run.
- `github` emits annotation lines suitable for workflow logs.

## Current rule set

See `eta_lint_rules.md` for the reviewed catalogue.

