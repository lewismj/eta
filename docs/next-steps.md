# Next Steps

1. [Lint & Formatter Plan](plan/lint_formatter_plan.md)
   - Design and implement a linter and code formatter for Eta, integrated into the `eta` CLI and LSP server, to enforce style consistency and catch common issues.
   
2. Use of HiGHS – A high-performance linear programming solver, retain the builtin ones, but create a package for HiGHS.

3. (Minor) We have a mechnism for incorporating external libraries into Eta at runtime.
   To support this, there is a generic `NativeObject` heap kind type. Some of the 'standard lib' packages currently 
   have their own type, that should be replaced with this. Those are no longer needed.