# Eta — Quick Start

[<- Back to README](../README.md) · [Build from Source](build.md) ·
[Language Guide](guide/examples-tour.md) · [Compiler (`etac`)](guide/reference/compiler.md) ·
[REPL](guide/reference/repl.md)

---

> [!IMPORTANT]
> **How to build your first app + end-to-end app/library flow:** start with
> [How to Build Your First App](app/first_app.md), then run the
> [Cookbook End-to-End Packaging Example](../cookbook/packaging/end-to-end/README.md).

> [!IMPORTANT]
> **How to get started with Jupyter:** see
> [Eta Jupyter Kernel (`eta_jupyter`)](guide/reference/jupyter.md) and
> [Cookbook Notebooks](../cookbook/notebooks/README.md).

## Installing from a Release

Download the latest archive for your platform from
[GitHub Releases](https://github.com/lewismj/eta/releases):

| Platform     | Archive                            |
|--------------|------------------------------------|
| Windows x64  | `eta-v0.2.0-win-x64.zip`          |
| Linux x86_64 | `eta-v0.2.0-linux-x86_64.tar.gz`  |

### Linux / macOS

```bash
tar xzf eta-v0.2.0-linux-x86_64.tar.gz
cd eta-v0.2.0-linux-x86_64
./install.sh                  # adds bin/ to PATH, sets ETA_MODULE_PATH
                              # installs VS Code extension if 'code' is on PATH
```

To install into a custom prefix instead of using the bundle in-place:

```bash
./install.sh /usr/local       # copies bin/, stdlib/ ? /usr/local/
```

### Windows

```powershell
Expand-Archive eta-v0.2.0-win-x64.zip -DestinationPath .
cd eta-v0.2.0-win-x64
.\install.cmd                 # adds bin\ to user PATH, sets ETA_MODULE_PATH
                              # installs VS Code extension if 'code' is on PATH
```

To install into a custom prefix:

```powershell
.\install.cmd "C:\Program Files\Eta"
```

> [!NOTE]
> Always use `install.cmd` rather than calling `install.ps1`
> directly — the `.cmd` wrapper launches PowerShell with
> `-ExecutionPolicy Bypass` so it works regardless of the system
> execution policy.

> [!NOTE]
> Open a **new** terminal after running the installer for the
> environment changes to take effect.

---

## Running Programs

Eta provides two ways to run `.eta` source files — **interpret directly**
or **compile ahead-of-time** then run the bytecode.

### Interpret from Source — `etai`

`etai` compiles an `.eta` file in-memory (lex ? parse ? expand ? link ?
analyze ? emit) and executes it immediately:

```bash
etai cookbook/basics/hello.eta
```

```
Hello, world!
2432902008176640000
```

`etai` also accepts pre-compiled `.etac` bytecode files. When given a
`.etac` file it **skips every front-end stage** and jumps straight to VM
execution for faster startup:

```bash
etai cookbook/basics/hello.etac
```

### Ahead-of-Time Compilation — `etac`

`etac` runs the full compilation pipeline and **serializes** the
resulting bytecode to a compact binary `.etac` file instead of executing
it:

```bash
etac cookbook/basics/hello.eta                       # ? cookbook/basics/hello.etac
etai cookbook/basics/hello.etac                      # run from bytecode (instant load)
```

#### `etac` CLI Reference

```
Usage: etac [options] <file.eta> [-o <file.etac>]
```

| Flag | Description |
|------|-------------|
| `-o <output>` | Output file path. Defaults to `<input>.etac`. |
| `-O`, `--optimize` | Enable IR optimization passes (constant folding, primitive specialisation, dead code elimination). |
| `-O0` | Disable optimization (default). |
| `--disasm` | Print disassembly to stdout instead of writing a `.etac` file. |
| `--no-debug` | Strip debug info (source maps) from the output, producing a smaller file. |
| `--path <dirs>` | Module search path (`;`-separated on Windows, `:`-separated on Linux). Falls back to `ETA_MODULE_PATH`. |
| `--help` | Show the help message. |

**Examples:**

```bash
# Compile with optimizations
etac -O cookbook/basics/hello.eta -o hello-opt.etac

# Inspect the emitted bytecode without writing a file
etac --disasm cookbook/basics/hello.eta

# Strip debug info for a smaller distributable
etac --no-debug cookbook/basics/hello.eta -o hello-small.etac
```

#### Disassembly

Both `etac` and `etai` support `--disasm`:

| Command | What it disassembles |
|---------|---------------------|
| `etac --disasm file.eta` | Compile from source, print bytecode to stdout (no `.etac` written). |
| `etai --disasm file.eta` | Compile + execute from source, then dump the registry. |
| `etai --disasm file.etac` | Load a pre-compiled `.etac` file and dump its bytecode. |

> [!TIP]
> See [Compiler (`etac`)](guide/reference/compiler.md) for the full binary format
> specification and optimization pass details.

---

## Interactive REPL

```bash
eta_repl
```

The REPL does not auto-import stdlib modules. Import what you need first:

```scheme
?> (import std.core)
?> (import std.math)
?> (import std.collections)
```

Then call functions normally:

```
?> (atom? 42)
#t
?> (even? 6)
#t
?> (filter (lambda (x) (> x 3)) (range 1 7))
(4 5 6)
```

You can define and use your own functions interactively:

```
?> (defun square (x) (* x x))
?> (square 7)
=> 49
```

Redefining a name is supported. New submissions see the latest definition,
while functions compiled earlier keep the bindings they were compiled with.
See [REPL](guide/reference/repl.md) for details.

You can import user-defined modules too. Use `--path` to tell the REPL
where to find your `.eta` files:

```bash
eta_repl --path ./mylibs
```

```
?> (import greeting)
?> (say-hello "REPL")
Hello, REPL!
```

---

## Modules & Imports

### Standard Library Modules

| Module               | Description                              |
|----------------------|------------------------------------------|
| `std.core`           | `atom?`, `compose`, `flip`, `iota`, —    |
| `std.math`           | `pi`, `e`, `even?`, `gcd`, `expt`, —     |
| `std.io`             | `println`, `eprintln`, `read-line`, port helpers |
| `std.collections`    | `filter`, `foldl`, `sort`, `range`, —     |
| `std.logic`          | `==`, `copy-term`, `naf`, `findall`, —    |
| `std.clp`            | `clp:=`, `clp:all-different`, `clp:solve`, — |
| `std.clpb`           | Boolean CLP → `clp:and`, `clp:sat?`, — *(opt-in)* |
| `std.clpr`           | Real-interval CLP → `clp:r=`, `clp:r-minimize`, — *(opt-in)* |
| `std.causal`         | `dag:*`, `do:identify`, `do:estimate-effect` |
| `std.fact_table`     | `make-fact-table`, `fact-table-query`, —  |
| `std.db`             | `defrel`, `assert`, `retract`, `call-rel`, `tabled` |
| `std.stats`          | `stats:mean`, `stats:ols`, distributions, — |
| `std.time`           | `time:now-ms`, `time:elapsed-ms`, `time:format-iso8601-utc`, ... |
| `std.net`            | `with-socket`, `request-reply`, `worker-pool`, — |
| `std.freeze`         | `freeze`, `dif` *(opt-in)*                |
| `std.supervisor`     | `one-for-one`, `one-for-all` *(opt-in)*   |
| `std.torch`          | `tensor`, `forward`, `train-step!`, — *(opt-in)* |
| `std.test`           | `make-test`, `assert-equal`, `run`, — *(opt-in)* |

### Writing a Module

A module groups definitions behind an explicit `import`/`export`
interface. Save this as **`greeting.eta`**:

```scheme
(module greeting
  (import std.io)
  (export say-hello)
  (begin
    (defun say-hello (name)
      (println (string-append "Hello, " name "!")))))
```

Run it directly:

```bash
etai greeting.eta
```

### Importing Your Module from Another File

Create **`app.eta`** in the same directory:

```scheme
(module app
  (import greeting)
  (begin
    (say-hello "world")))
```

`etai` auto-adds the input file's directory to the module search path,
so sibling modules are found automatically:

```bash
etai app.eta          # prints: Hello, world!
```

For modules in different directories, use `--path` or `ETA_MODULE_PATH`
(`;`-separated on Windows, `:`-separated on Linux):

```bash
etai --path ./libs app.eta

# or
export ETA_MODULE_PATH=./libs        # Linux
set ETA_MODULE_PATH=.\libs           # Windows
etai app.eta
```

### Import Clause Variants

Eta supports several ways to control which names are imported:

```scheme
;; Import all exported names
(import greeting)

;; Import only specific names
(import (only std.math pi e))

;; Import everything except certain names
(import (except std.collections sort))

;; Rename on import
(import (rename std.math (pi PI) (e E)))

;; Prefix — all imported names gain a prefix (namespace-style)
(import (prefix std.math math:))
;; now use math:pi, math:even?, math:gcd, etc.
```

The `prefix` clause is particularly useful when two modules export the
same name:

```scheme
(module app
  (import (prefix mod-a a:))
  (import (prefix mod-b b:))
  (begin
    ;; No conflict — each name is qualified
    (a:process data)
    (b:process data)))
```

---

## VS Code Extension

The installer automatically installs the VS Code extension when VS Code
is present. The extension provides:

- **Syntax highlighting** for `.eta` files
- **Live diagnostics** via the Language Server (`eta_lsp`)
- **Debugging** with breakpoints, stepping, and call-stack inspection via the Debug Adapter (`eta_dap`)
- **Heap Inspector** — live memory visualisation while debugging
- **Disassembly View** — live bytecode view with current-PC marker
- **GC Roots Tree** — sidebar panel showing root categories with drill-down

### Extension Settings

Open VS Code settings (`Ctrl+,` or `Cmd+,`) and search for **Eta**, or
add the following to your `settings.json`:

```json
{
  "eta.lsp.serverPath":    "/path/to/eta-release/bin/eta_lsp",
  "eta.dap.executablePath": "/path/to/eta-release/bin/eta_dap",
  "eta.modulePath":        "/path/to/eta-release/stdlib"
}
```

| Setting | Description |
|---------|-------------|
| `eta.lsp.serverPath` | Path to the `eta_lsp` executable. If empty, the extension searches bundled paths, workspace build output, then `PATH`. |
| `eta.lsp.enabled` | Enable/disable the language server (default: `true`). |
| `eta.dap.executablePath` | Path to the `eta_dap` executable (or the directory containing it). If empty, searches next to `eta_lsp`, then bundled paths, then `PATH`. |
| `eta.modulePath` | Module search path (`ETA_MODULE_PATH`). Used by both LSP and DAP. Falls back to the `ETA_MODULE_PATH` environment variable. |
| `eta.debug.autoShowHeap` | Automatically open the Heap Inspector when a debug session starts (default: `true`). |

The extension looks for the LSP binary in the following order:

1. `eta.lsp.serverPath` setting (if configured)
2. `<extension>/bin/eta_lsp` (bundled in the release)
3. Workspace build output directories
4. `PATH`

### Running & Debugging

1. Open the `cookbook/` folder from the release bundle (**File ? Open Folder**).
2. Open any `.eta` file — syntax highlighting and diagnostics activate automatically.
3. **Run from terminal:** `etai hello.eta` in the integrated terminal.
4. **Debug with F5:** press **F5** (or **Run ? Start Debugging**) — the extension launches `eta_dap` automatically.

#### Breakpoints & Stepping

1. Click the gutter to set a breakpoint (red dot).
2. Press **F5** to start debugging.
3. When the VM hits a breakpoint it pauses. Use the standard controls:
   - **F10** Step Over — **F11** Step In — **Shift+F11** Step Out — **F5** Continue
4. Inspect local variables, the call stack, and evaluate expressions in the Debug Console.

Script output (`display`, `newline`, etc.) appears in the **Eta Output**
panel (not the Debug Console).

### Heap Inspector

The Heap Inspector provides a live visualisation of the VM's heap while
debugging.

1. Open the Command Palette (`Ctrl+Shift+P`) and run **Eta: Show Heap Inspector**.
   (The panel also opens automatically when a debug session starts, controlled by `eta.debug.autoShowHeap`.)
2. The inspector panel opens beside your editor showing:
   - **Memory gauge** — current heap usage vs. soft limit.
   - **Cons Pool** — pool utilisation (live/capacity/free/bytes).
   - **Object Kinds** — count and bytes per type (Cons, Closure, Vector, String, etc.), sorted by size.
   - **GC Roots** — expandable tree of root categories (Stack, Globals, Frames, etc.). Globals are grouped by module.
3. Click any **Object #N** link to drill into it — view kind, size, value preview, and child references.
4. The panel **auto-refreshes** each time the VM stops (breakpoint, step). You can also click **Refresh** manually.

### Disassembly View

The Disassembly View shows the bytecode of the currently executing
function (or all loaded functions) while debugging.

- **Sidebar panel:** The **Disassembly** panel appears in the Debug sidebar when an Eta debug session is active. It shows each bytecode instruction as a tree item, with a `◀ PC` marker on the current program counter. It auto-refreshes on every step/breakpoint.
- **Full-document view:** Open the Command Palette and run:
  - **Eta: Show Disassembly** — disassembly of the current function.
  - **Eta: Show Disassembly (All Functions)** — disassembly of every loaded function.

### GC Roots Tree

The **Memory** panel appears in the Debug sidebar during an Eta debug
session. It provides an expandable tree of GC root categories:

- **Stack**, **Globals**, **Frames**, etc.
- **Globals** are automatically grouped by module prefix for readability.
- Each root object is expandable — clicking it sends an `eta/inspectObject` request and shows child fields (car/cdr, vector elements, upvalues, etc.).
- Click any object node to open it in the Heap Inspector.
- Use the refresh button in the panel title bar, or it auto-refreshes on each VM stop.

---

## Bundle Layout

```
eta-v0.2.0-<platform>/
  bin/
    etac(.exe)              # Ahead-of-time bytecode compiler
    etai(.exe)              # File interpreter (also runs .etac files)
    eta_repl(.exe)          # Interactive REPL
    eta_lsp(.exe)           # Language Server (JSON-RPC over stdio)
    eta_dap(.exe)           # Debug Adapter (DAP over stdio, used by VS Code)
  stdlib/
    std/
      core.eta  math.eta  io.eta  collections.eta  test.eta
      logic.eta  clp.eta  clpb.eta  clpr.eta  causal.eta
      db.eta  fact_table.eta  freeze.eta  net.eta  stats.eta  time.eta
      supervisor.eta  torch.eta
  cookbook/
    hello.eta  basics.eta  functions.eta  higher-order.eta  ...
  editors/
    eta-lang-<version>.vsix # VS Code extension
  install.sh / install.cmd  # Post-extract installer
```

The binaries automatically locate `stdlib/` relative to themselves
(`<exe>/../stdlib/`), so no environment variables are needed when
using the installed layout.
