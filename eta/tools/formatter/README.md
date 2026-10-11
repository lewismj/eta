# Formatter implementation

`eta_format_lib` owns buffer formatting; `eta_formatter` builds `eta_fmt` and
owns argument parsing, deterministic publication and filesystem operations.
`eta_tooling_files` is the shared CLI file layer for future lint integration.
Neither static library is installed as a public C++ SDK.

The formatter builds an arena-based Doc IR from immutable CST nodes, renders it,
then checks output with the existing reader and token/comment/state guards.
Raw Text views remain owned by the captured file until rendering finishes.
Doc rendering uses an explicit stack; limits and cancellation remain fallible.

To add an alias, extend the code-only shape table in `forms.cpp`, add
short/long/commented and reader-sensitive golden fixtures, and exercise reader
preservation and idempotence. Phase 3 now recognises the common special-form
heads (`defun`, `define`, `lambda`, `let`, `let*`, `letrec*`, `begin`,
`module`, `if`, `when`, `unless`, `cond`, `case`, `do`, `import`, `export`,
and `define-record-type`) when their shapes are verified. A new form does not
require an alias: generic structural layout remains available without macro
expansion.

Debug mismatches through `validate_output()`; deliberate corruption tests
exercise that production guard. Golden fixture changes require review and are
never an alternative to independent reader properties. The CLI tests run on
temporary corpus copies and verify that the second pass performs no writes.

See [CLI and filesystem guarantees](../../../docs/eta_fmt.md),
[style](../../../docs/eta_fmt_style.md), and
[the implementation plan](../../../docs/plan/lint_formatter_plan.md).
