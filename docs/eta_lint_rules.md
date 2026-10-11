# eta_lint rule catalogue

This page lists the reviewed rules currently registered by `eta_lint`.
Enabled rules participate in presets and fixtures; gated rules are documented
but excluded from the enabled-rule count.

| Code | Name | Status | Severity | Analysis | Fixtures |
| --- | --- | --- | --- | --- | --- |
| ETA104 | NonKebabCase | enabled | info | cst | positive, negative, dependency, unknown |
| ETA203 | DuplicateExport | enabled | warning | cst | positive, negative, dependency, unknown |
| ETA301 | UnusedLetBinding | gated | warning | scope | none yet |
| ETA401 | CommentMarkerStyle | enabled | info | cst | positive, negative, dependency, unknown |
| ETA501 | FunctionTooLong | enabled | warning | cst | positive, negative, dependency, unknown |

## Notes

- ETA104, ETA203, ETA401 and ETA501 are the currently enabled seed rules.
- ETA301 remains gated until binding/reference semantics are proven.
- No numeric rule-count promise substitutes for fixture validation or corpus
  preservation checks.

