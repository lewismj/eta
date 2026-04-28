# Eta VS Code Extension

The Eta VS Code extension is the recommended way to develop in Eta. It bundles
syntax highlighting, a Language Server (`eta_lsp`), a Debug Adapter
(`eta_dap`), a Test Explorer integration (`eta_test`), and several live
runtime inspection panels (heap, disassembly, child processes).

This page is the **single source of truth** for the extension and covers:

1. [Installation](#installation)
2. [Configuration](#configuration)
3. [Binary Discovery Order](#binary-discovery-order)
4. [Launch Configuration Reference](#launch-configuration-reference)
5. [Editing & Language Features](#editing--language-features)
6. [Debugging Workflow](#debugging-workflow)
7. [Runtime Inspection Panels](#runtime-inspection-panels)
8. [Test Explorer Integration](#test-explorer-integration)
9. [Useful Commands](#useful-commands)
10. [Troubleshooting](#troubleshooting)

---

## Installation

The release installer (`install.sh` / `install.cmd` / `install.ps1`) installs
the `.vsix` automatically when VS Code is detected on `PATH`. To install
manually:

```bash
code --install-extension editors/eta-lang-<version>.vsix
```

After installing, open any `.eta` file. The extension activates automatically
and starts `eta_lsp` in the background.

---

## Configuration

Open VS Code settings (`Ctrl+,` → search **Eta**) and set the binary paths if
they are not already on `PATH`:

```jsonc
{
  // Per-tool overrides (highest priority)
  "eta.lsp.serverPath":     "/path/to/bin/eta_lsp",
  "eta.dap.executablePath": "/path/to/bin/eta_dap",
  "eta.test.runnerPath":    "/path/to/bin/eta_test",

  // ETA_MODULE_PATH used when launching the LSP / DAP / test runner
  "eta.modulePath": "/path/to/stdlib",

  // Additional directories (or full executable paths) to search.
  // Supports ${workspaceFolder} substitution.
  "eta.binaries.searchPaths": [
    "${workspaceFolder}/build/eta/interpreter",
    "${workspaceFolder}/build/eta/lsp",
    "${workspaceFolder}/build/eta/dap",
    "${workspaceFolder}/build/eta/test_runner"
  ]
}
```

![Eta extension settings in VS Code](img/vsx/eta_lsp_config.png)

---

## Binary Discovery Order

The extension resolves `eta_lsp`, `eta_dap`, and `eta_test` in this order:

1. **Per-tool explicit settings** — `eta.lsp.serverPath`,
   `eta.dap.executablePath`, `eta.test.runnerPath`.
2. **`bin/`** inside the extension install directory (used by the bundled
   release installer).
3. **Workspace build output directories** (e.g. `build/eta/lsp`,
   `build/eta/dap`).
4. **Additional paths** from `eta.binaries.searchPaths`.
5. **`PATH`** lookup.

`eta.binaries.searchPaths` accepts directories *or* full executable paths and
supports `${workspaceFolder}` substitution.

---

## Launch Configuration Reference

The Eta debugger contributes a `launch.json` schema with the following
properties:

| Property      | Type             | Default              | Description |
|---------------|------------------|----------------------|-------------|
| `program`     | string           | (required)           | Path to the `.eta` (or `.etac`) file to run. |
| `args`        | string[]         | `[]`                 | Command-line arguments forwarded to the program. |
| `cwd`         | string           | `${workspaceFolder}` | Working directory for the spawned process. |
| `env`         | `{string:string}`| `{}`                 | Extra environment variables. |
| `modulePath`  | string           | `eta.modulePath`     | Per-session override for `ETA_MODULE_PATH`. |
| `stopOnEntry` | boolean          | `false`              | Pause at the first instruction. |
| `etac`        | boolean          | `false`              | Pre-compile with `etac` and run the resulting bytecode. |
| `console`     | enum             | `debugConsole`       | `debugConsole` \| `integratedTerminal` \| `externalTerminal`. |
| `trace`       | boolean          | `false`              | Adds `--trace-protocol` to `eta_dap` for DAP message logging. |

A minimal `launch.json` entry:

```jsonc
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "eta",
      "request": "launch",
      "name": "Run current Eta file",
      "program": "${file}",
      "stopOnEntry": false,
      "console": "integratedTerminal"
    }
  ]
}
```

---

## Editing & Language Features

Provided by `eta_lsp`:

- Syntax highlighting and snippets for `.eta` files.
- Live diagnostics (lex / parse / expand / link / analyze).
- Hover, completion, signature help.
- Go to definition, find references, rename.
- Document outline / workspace symbols.

---

## Debugging Workflow

1. Open an `.eta` file.
2. Click the gutter to set one or more breakpoints.
3. Press **F5** (or run **Eta: Debug Eta File**).
4. Use the standard step controls:
   - **F10** Step Over
   - **F11** Step In
   - **Shift+F11** Step Out
   - **F5** Continue

![Eta debug session](img/vsx/eta_debug_session.png)

The Variables, Watch, Call Stack, and Debug Console panels behave as for any
other VS Code language. Expressions typed in the Debug Console are evaluated
in the current frame by `eta_dap`.

---

## Runtime Inspection Panels

During an Eta debug session the Debug sidebar exposes dedicated views for
**Memory**, **Disassembly**, and **Child Processes**.

![Eta debug side panels](img/vsx/eta_side_panels.png)

### Heap Inspector

Run **Eta: Show Heap Inspector** for a live WebView with:

- Memory usage gauge.
- Object-kind counts and bytes.
- Cons pool utilization.
- GC root categories (Stack, Globals, Frames) with module-grouped globals and
  object drill-down.

![Eta heap inspector](img/vsx/eta_heap_inspector.png)

### Disassembly Views

The extension supports both an inline side panel (current function, with a
live PC marker) and a full-document view of every loaded function.

![Eta disassembly in side panel](img/vsx/eta_disassembly_side_panel.png)

![Eta full disassembly document](img/vsx/eta_full_disassembly.png)

### Child Processes

When debugging actor / message-passing code, the **Child Processes** panel
lists every spawned process with PID, endpoint, and live/exited status.

---

## Test Explorer Integration

When the workspace contains `*.test.eta` files, the extension registers an Eta
test controller that drives `eta_test --format tap`.

Available run profiles:

- **Run** — execute tests and report pass/fail.
- **Debug** — launch tests under `eta_dap`.
- **Coverage** — when supported by the installed `eta_test` build.

The TAP parser maps YAML diagnostics into rich `TestMessage`s, including:

| YAML key   | Mapped to |
|------------|-----------|
| `severity` | Message severity. |
| `at`       | Clickable `TestMessage.location` (file + line). |
| `expected` | Expected output of the assertion. |
| `actual`   | Actual output of the assertion. |
| `message`  | Human-readable failure message. |

Runner stdout/stderr is streamed into the test output channel as it arrives,
not buffered until process exit, so long-running tests show progress live.

![Eta test runner in VS Code](img/vsx/eta_test_runner.png)

---

## Useful Commands

| Command                                    | Purpose |
|--------------------------------------------|---------|
| `Eta: Run Eta File`                        | Run the active Eta file. |
| `Eta: Debug Eta File`                      | Launch the active file under the Eta debugger. |
| `Eta: Run Tests in Current File`           | Run tests for the active `*.test.eta` file. |
| `Eta: Show Heap Inspector`                 | Open the live heap inspection WebView. |
| `Eta: Show Disassembly`                    | Show disassembly for the current function. |
| `Eta: Show Disassembly (All Functions)`    | Show disassembly for every loaded function. |
| `Eta: Refresh Memory`                      | Refresh the Memory tree view. |
| `Eta: Refresh Disassembly`                 | Refresh the Disassembly tree view. |
| `Eta: Refresh Child Processes`             | Refresh child-process state in the debug view. |

---

## Troubleshooting

**LSP does not start / "Could not locate eta_lsp".**
Set `eta.lsp.serverPath` explicitly, or add the build output directory to
`eta.binaries.searchPaths`. Check the **Eta Language Server** output channel
for stderr from `eta_lsp`.

**Debugger reports "module not found".**
Set `eta.modulePath` (or the per-session `modulePath` in `launch.json`) to
your `stdlib/` directory. The release installer points this at the bundled
stdlib automatically.

**Need to capture a DAP transcript.**
Enable `"trace": true` in your `launch.json` configuration — this passes
`--trace-protocol` to `eta_dap`, which logs every DAP message to stderr.

**Tests are not discovered.**
The Test Explorer only registers files matching `**/*.test.eta`. Ensure
`eta.test.runnerPath` resolves to a working `eta_test` binary; run it once
manually with `--format tap` to confirm.

---

## See Also

- [Quick Start](../../quickstart.md) — installing Eta and running your first program.
- [Build from Source](../../build.md) — producing local `eta_lsp` / `eta_dap` /
  `eta_test` binaries that the extension can pick up.
- [Bytecode & VM](bytecode-vm.md) — context for the Disassembly view.
- [Runtime & GC](runtime.md) — context for the Heap Inspector view.
- [Message Passing & Actors](message-passing.md) — context for the Child
  Processes view.

