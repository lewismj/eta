# Linter specification and CLI surface

Phase 0 supplies [the rule schema](rules/schema.json) and
[validator](../../../scripts/check_lint_rule_specs.py), alongside
[the shared CST/diagnostic library](../../../docs/cst_reader.md).
Phase 2 delivered the engine and `eta_lint` executable; Phase 3 adds reviewed
coverage, preset selection, configuration discovery, rule listing/explanations,
directives and additional reporter formats. ETA301 remains gated until its
semantic prerequisites are proven.

Each future rule lives in `rules/ETAxxx.toml`. Required fields describe rationale,
analysis level, metadata/prerequisites, unknown-information behaviour,
false-positive limits, fix safety, typed options and preset membership.
Fixture references resolve to `.eta`/`.json` pairs in
`eta/qa/test/fixtures/linter`; diagnostics carry exact byte spans.
Enabled rules require positive/negative and dependency/unknown fixtures and
verified prerequisites. Fixable rules require expected fixed-source fixtures.
Delegated compiler rules never join lint presets or register independent checks.

Use `eta_lint --list-rules` to inspect the active catalogue and
`eta_lint --explain ETA401` for the reviewed metadata, prerequisites and fixture
matrix. `--preset` selects the compiled rule set, `--config` loads a single
`.eta-lint.toml` file, and `[rules]` can disable specific codes. Supported output
formats are pretty, JSON, SARIF and GitHub command annotations.

Directive comments use `eta-lint:disable`, `eta-lint:disable-line`,
`eta-lint:disable-file` and `eta-lint:enable` in ordinary line comments only.

See [the linter user guide](../../../docs/eta_lint.md) and
[the rule catalogue](../../../docs/eta_lint_rules.md) for the reviewed CLI and
rule inventory.

Run `python scripts/check_lint_rule_specs.py` and
`python scripts/tests/test_lint_rule_specs.py`; CTest runs both automatically.
`--output FILE` produces deterministically ordered registration/preset input
without timestamps or checkout paths.
