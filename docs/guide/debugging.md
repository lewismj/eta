# Debugging — LSP, DAP, and the VS Code Extension

[← Back to Language Guide](../language_guide.md)

Eta ships two cooperating processes for IDE integration:

| Binary       | Protocol                            | Purpose                                  |
| :----------- | :---------------------------------- | :--------------------------------------- |
| `eta_lsp`    | Language Server Protocol            | Diagnostics, completion, hover, navigation |
| `eta_dap`    | Debug Adapter Protocol              | Breakpoints, stepping, inspection        |

The VS Code extension wires both into the editor and adds custom
visualisations. See [`vscode.md`](./reference/vscode.md) for installation.

---

## Language Server (`eta_lsp`)

| Capability                | Notes                                                |
| :------------------------ | :--------------------------------------------------- |
| Diagnostics               | Parse and macro-expansion errors                     |
| Hover                     | Function signatures, docstrings (when present)       |
| Go-to definition          | Cross-module                                         |
| Find references           | Symbol usage across the workspace                    |
| Document / workspace symbols | `define`, `defun`, `define-syntax`, exports       |
| Auto-completion           | Bindings in scope plus imported names                |
| Signature help            | Parameter list of the call under the cursor          |

The server respects `ETA_MODULE_PATH` and any `eta.modulePath` setting
in the workspace.

---

## Debug Adapter (`eta_dap`)

`eta_dap` exposes the running VM over DAP. From VS Code, choose
**Run → Start Debugging** with the *Eta* configuration.

### Supported features

- **Breakpoints** — line and conditional
- **Stepping** — step over / into / out
- **Stack traces** — with file/line and function names
- **Variable inspection** — locals, upvalues, evaluated expressions
- **Watch expressions** — evaluated in the paused frame
- **Pause / continue** — interrupt a running VM
- **Stop on exception** — catches `runtime.*` and user-tagged raises

### `launch.json` example

```json
{
  "type": "eta",
  "request": "launch",
  "name": "Run current file",
  "program": "${file}",
  "stopOnEntry": false,
  "modulePath": ["${workspaceFolder}/stdlib"]
}
```

---

## VS Code panels

The extension adds three custom views:

| View              | What it shows                                                       |
| :---------------- | :------------------------------------------------------------------ |
| Heap Inspector    | Live heap objects grouped by type, with structural drill-down       |
| Disassembly View  | Bytecode of the current frame with the program counter highlighted  |
| GC Roots Tree     | All registered GC roots — value stack, frame stack, intern table, finalizers |

These views update on every DAP `stopped` event.

---

## Diagnosing common problems

| Symptom                                   | Likely cause                                            |
| :---------------------------------------- | :------------------------------------------------------ |
| Stack overflow on linear recursion        | Lost TCO — see [Tail Calls](./tail-calls.md)            |
| Actor `recv!` blocks forever              | Peer never sent; or `nng-close` was missed              |
| `runtime.unbound` after rename            | Stale `.etac` cache — recompile with `etac`             |
| Macro emits unexpected code               | Inspect `etac --dump-expand file.eta`                   |
| Heap grows unbounded                      | Long-lived closure capturing a growing list — Heap Inspector helps locate the root |

---

## Related

- [`vscode.md`](./reference/vscode.md)
- [`runtime.md`](./reference/runtime.md), [`bytecode-vm.md`](./reference/bytecode-vm.md)
- [Bytecode & Tools](./bytecode-and-tools.md)
- [Testing](./testing.md)


