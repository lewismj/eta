<div align="center" style="display: flex; justify-content: center; align-items: center; gap: 20px;">
  <img src="docs/img/eta.svg" alt="Eta logo" width="120">
  <div style="text-align: left;">
    <h1 style="margin: 0;">η (Eta)</h1>
    <p style="margin: 6px 0 0 0;">
      <strong>A Lisp-inspired language for symbolic, logic and constraint logic (CLP) programming, with machine learning, and causal inference.</strong>
    </p>
  </div>
</div>

---
<p align="center">
    <strong>Language and System Guide</strong><br>
    <a href="https://lewismj.github.io/eta/">Eta</a>
</p>

<p align="center">
  <strong>Featured Examples</strong><br>
  <a href="https://lewismj.github.io/eta/docs/featured/">lewismj.github.io/eta/docs/featured/</a>
</p>

---

## Getting Started

The easiest way to experience Eta is through the pre-built release bundles, which include the full toolchain and standard library.

### 1. Download the Release

Grab the latest bundle for your platform from the [Releases](https://github.com/lewismj/eta/releases) page:

- **Windows x64:** `eta-v0.0.2-win-x64.zip`
- **Linux x86_64:** `eta-v0.0.2-linux-x86_64.tar.gz`

### 2. Install

Unpack the archive and run the platform-specific installer. This configures your `PATH`, sets up `ETA_MODULE_PATH`, and registers the VS Code extension.

**Windows (PowerShell)**
```powershell
cd eta-v0.0.2-win-x64
.\install.cmd
```

**Linux / macOS**
```bash
cd eta-v0.0.2-linux-x86_64
chmod +x install.sh && ./install.sh
```

> **Note:** Restart your terminal after installation to apply the environment changes.

### 3. Verify

```console
eta --help
etai --help
```

### 4. Build Your First App

Eta ships with a project model — `eta new`, `eta build`, `eta run`, `eta test`, `eta add`.

```console
eta new hello_app --bin
cd hello_app
eta build
eta run
```

See [Build Your First App](https://lewismj.github.io/eta/docs/app/first_app/) for a full walkthrough.

---

## CLI overview

`eta` is the primary entrypoint. It drives the packaging workflow and delegates to the underlying tools:

```console
eta new <name> [--bin|--lib]   # scaffold a new package
eta build                       # compile the current package
eta run                         # build and run the entry point
eta test                        # run the test suite
eta add <pkg>                   # add a dependency
eta tree                        # show the resolved dependency graph
eta prof run / report / view    # profiling sub-commands
```

The full release bundle also ships the lower-level tools for direct use or editor integration:

| Tool              | Purpose                                                              |
| :---------------- | :------------------------------------------------------------------- |
| **`etai`**        | Run a single `.eta` source file or pre-compiled `.etac` bytecode.   |
| **`etac`**        | Ahead-of-time compiler — `.eta` → optimised `.etac` bytecode.       |
| **`eta_repl`**    | Interactive REPL.                                                    |
| **`eta_lsp`**     | Language server — diagnostics, completion, and navigation.           |
| **`eta_dap`**     | Debug adapter — breakpoints, stepping, and call-stack inspection.    |
| **`eta_jupyter`** | Jupyter kernel — interactive notebooks with rich output.             |
| **`eta_test`**    | Test runner — TAP / JUnit output and VS Code Test Explorer.          |

The VS Code extension adds a **Heap Inspector**, **Disassembly View**, and **GC Roots Tree** for inspecting the VM at runtime.

See [Quick Start](https://lewismj.github.io/eta/docs/quickstart/) and [Build Your First App](https://lewismj.github.io/eta/docs/app/first_app/) for the full tooling walkthrough.

---

*License: [LICENSE.txt](LICENSE.txt)*
