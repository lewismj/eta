# Next Steps

1. [Workspace Plan](plan/workspace_plan.md)
   - Extend packaging to support workspace roots with `[workspace] members = [...]`, shared lockfile/module materialization, and workspace-aware CLI/tooling behavior.

2. [Native Sidecar Plan](plan/native_sidecar_plan.md)
   - Move C++-backed capabilities from hard-linked builtins to package-managed native sidecars with explicit ABI/loading, deterministic resolution, and staged migration.

3. [Lint & Formatter Plan](plan/lint_formatter_plan.md)
   - Design and implement a linter and code formatter for Eta, integrated into the `eta` CLI and LSP server, to enforce style consistency and catch common issues.

4. [Profiler Plan](plan/profiler_plan.md)
   - Add a first-class profiler (sampling + instrumenting) to the Eta runtime with `eta prof` CLI, `std/prof` API, and speedscope export so users can see where time is spent.


