# Eta — Quick Install & Run

The fastest way to get started is to download a pre-built release, run the installer, and try the examples.

Note: the DAP server is still early in development, will add features and fixes soon, but you can 
still run set breakpoints and run through the examples, as shown below.

---

## 1. Download & Unpack

Download the latest [release](https://github.com/lewismj/eta/releases/tag/v0.0.1) for your platform:

| Platform | Archive                          |
|----------|----------------------------------|
| Windows x64 | `eta-v0.0.9-win-x64.zip`         |
| Linux x86_64 | `eta-v0.0.9-linux-x86_64.tar.gz` |

Unzip the archive into a directory of your choice.

---

## 2. Run the Installer

The installer adds the `bin/` directory to your `PATH`, sets the `ETA_MODULE_PATH` environment variable so the runtime can find the standard library, and installs the VS Code extension (if VS Code is detected).

**Windows (PowerShell / Command Prompt):**
```console
cd eta-v0.0.9-win-x64
.\install.cmd
```

**Linux / macOS:**
```console
cd eta-v0.0.9-linux-x86_64
chmod +x install.sh && ./install.sh
```

Example output (Windows):
```console
C:\tmp\eta-v0.0.9-win-x64>.\install.cmd
+==============================================================+
|  Eta Installer (Windows)                                     |
+==============================================================+

  bin     : C:\tmp\eta-v0.0.9-win-x64\bin
  stdlib  : C:\tmp\eta-v0.0.9-win-x64\stdlib

> Eta already on user PATH -- skipping.
> ETA_MODULE_PATH already set -- skipping.
> Installing VS Code extension...
Installing extensions...
Extension 'eta-lang.vsix' was successfully installed.
  [OK] VS Code extension installed.

> Verifying...
  [OK] etai.exe
  [OK] eta_repl.exe
  [OK] eta_lsp.exe
  [OK] eta_dap.exe

[OK] Done! Open a new terminal and try:

    etai --help
    eta_repl

C:\tmp\eta-v0.0.9-win-x64>
```

> [!NOTE]
> Open a **new** terminal after installation so the updated `PATH` and `ETA_MODULE_PATH` take effect.

---

## 3. Run the Examples

The `examples/` directory inside the release bundle contains several `.eta` programs.

**Interpreter** — run a file directly:
```console
C:\>cd tmp

C:\tmp>cd eta-v0.0.9-win-x64

C:\tmp\eta-v0.0.9-win-x64>cd examples

C:\tmp\eta-v0.0.9-win-x64\examples>etai hello.eta
Hello, world!
2432902008176640000

C:\tmp\eta-v0.0.9-win-x64\examples>

```

```console
C:\tmp\eta-v0.0.9-win-x64\examples> etai aad.eta
f(x,y) = x*y + sin(x)
  grad at (2,3): (6.9093 #(2.58385 2))
g(x) = x^2 + 3x + 1
  grad at (4): (29 #(11))
h(x) = exp(2x)
  grad at (1): (7.38906 #(14.7781))
Rosenbrock f(x,y) = (1-x)^2 + 100(y-x^2)^2
  grad at (1,1): (0 #(0 0))
dot(v, [1,2,3])
  grad at (1,1,1): (6 #(1 2 3))
```

**REPL** — interactive session:
```console
C:\tmp\eta-v0.0.9-win-x64> eta_repl
Loaded C:\tmp\eta-v0.0.2-win-x64\stdlib\prelude.eta
eta REPL - type an expression and press Enter.
Use Ctrl+C or (exit) to quit.
eta> (+ 1 2 3 4 5)
=> 15
eta> (exit)
```

---

## 4. VS Code Setup

The installer automatically installs the VS Code extension when VS Code is present. The extension provides syntax highlighting and live diagnostics via the Language Server (`eta_lsp`).

### Configure the Language Server

Open VS Code settings (`Ctrl+,` or `Cmd+,`) and search for **Eta**. Set the following two values to point at the executables inside the release bundle:

| Setting | Value (Windows example)                     |
|---------|---------------------------------------------|
| `eta.executablePath` | `C:\tmp\eta-v0.0.2-win-x64\bin\etai.exe`    |
| `eta.lspPath` | `C:\tmp\eta-v0.0.2-win-x64\bin\eta_lsp.exe` |
| `eta.lspPath` | `C:\tmp\eta-v0.0.2-win-x64\bin\eta_lsp.exe` |

Or add them directly to your `settings.json`:

```json
{
  "eta.executablePath": "C:\\tmp\\eta-v0.0.9-win-x64\\bin\\etai.exe",
  "eta.lspPath":        "C:\\tmp\\eta-v0.0.9-win-x64\\bin\\eta_lsp.exe"
}
```

![LSP configuration in VS Code](docs/img/eta_lsp_config.png)

### Open and Run an Example

1. In VS Code open the `examples/` folder from the release bundle (**File → Open Folder**).
2. Open any `.eta` file — syntax highlighting and diagnostics activate automatically.
3. Run the file with the interpreter from the integrated terminal: `etai hello.eta`

![Running an example in VS Code](docs/img/eta_example_run.png)

---

## 5. Debugging

### Breakpoints & Stepping

1. Open a `.eta` file in VS Code.
2. Click the gutter to set a breakpoint (red dot).
3. Press **F5** (or **Run → Start Debugging**) — the DAP adapter launches the script.
4. When the VM hits a breakpoint it pauses. Use the standard controls:
   - **F10** Step Over · **F11** Step In · **Shift+F11** Step Out · **F5** Continue

Script output (`display`, `newline`, etc.) appears in the **Eta Output** panel (not the Debug Console).

### Heap Inspector

The Heap Inspector lets you visualise heap usage, per-object-kind statistics, and navigate GC roots while the VM is paused.

1. Set a breakpoint and start debugging (as above).
2. Open the Command Palette (`Ctrl+Shift+P`) and run **Eta: Show Heap Inspector**.
3. The inspector panel opens beside your editor showing:
   - **Memory gauge** — current heap usage vs. soft limit.
   - **Object Kinds** — count and bytes per type (Cons, Closure, Vector, etc.).
   - **GC Roots** — expandable tree of root categories (Stack, Globals, Frames, etc.).
4. Click any **Object #N** link to drill into it — view kind, size, value preview, and child references.
5. The panel **auto-refreshes** each time the VM stops (breakpoint, step). You can also click **Refresh** manually.

