# Eta VS Code Extension

This page documents the VS Code extension runtime configuration for Eta.

## Binary Discovery

The extension resolves `eta_lsp`, `eta_dap`, and `eta_test` in this order:

1. Per-tool explicit settings (`eta.lsp.serverPath`, `eta.dap.executablePath`, `eta.test.runnerPath`).
2. `bin/` inside the extension install directory.
3. Workspace build output directories.
4. Additional paths from `eta.binaries.searchPaths`.
5. `PATH`.

`eta.binaries.searchPaths` accepts directories or full executable paths and
supports `${workspaceFolder}` substitution.

## Launch Configuration

The Eta launch schema supports:

- `program` (required)
- `args` (string array)
- `cwd` (working directory, default `${workspaceFolder}`)
- `env` (string map)
- `modulePath` (session override for `ETA_MODULE_PATH`)
- `stopOnEntry` (boolean)
- `etac` (boolean)
- `console` (`debugConsole` | `integratedTerminal` | `externalTerminal`)
- `trace` (boolean, adds `--trace-protocol` to `eta_dap`)

## Test Explorer Notes

The Test Controller now parses TAP YAML diagnostics beyond `message`, including:

- `severity`
- `at` (mapped to clickable `TestMessage.location`)
- `expected` / `actual` (forwarded as expected/actual output when present)

Runner stdout/stderr is also streamed into test output as it arrives instead of
only being emitted at process exit.
