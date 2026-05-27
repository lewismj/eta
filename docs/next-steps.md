# Next Steps

1. [Lint & Formatter Plan](plan/lint_formatter_plan.md)
   - Design and implement a linter and code formatter for Eta, integrated into the `eta` CLI and LSP server, to enforce style consistency and catch common issues.

2. Use of HiGHS - keep the built-in solvers and add a package for the HiGHS linear programming solver.

3. (Minor) We have a mechanism for incorporating external libraries into Eta at runtime.
   To support this, there is a generic `NativeObject` heap kind type. Some `stdlib` packages still define their own
   native object kinds; those should be replaced with `NativeObject`.
