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
  <a href="https://lewismj.github.io/eta/docs/featured/portfolio/">Causal Decision Engine for Portfolio Optimisation</a> ·
  <a href="cookbook/notebooks/Portfolio.ipynb">notebook</a><br>
  <a href="https://lewismj.github.io/eta/docs/featured/xva-wwr/">Wrong-Way Risk via do-interventions</a>
</p>

---

## Getting Started

The easiest way to experience Eta is through the pre-built release bundles, which include the full toolchain and standard library.

### 1. Download the Release

Grab the latest bundle for your platform from the [Releases](https://github.com/lewismj/eta/releases) page:

- **Windows x64:** `eta-v0.4.0-win-x64.zip`
- **Linux x86_64:** `eta-v0.4.0-linux-x86_64.tar.gz`

### 2. Install

Unpack the archive and run the platform-specific installer. This configures your `PATH`, sets up `ETA_MODULE_PATH`, and registers the VS Code extension.

**Windows (PowerShell)**
```powershell
cd eta-v0.4.0-win-x64
.\install.cmd
```

**Linux / macOS**
```bash
cd eta-v0.4.0-linux-x86_64
chmod +x install.sh && ./install.sh
```

> **Note:** Restart your terminal after installation to apply the environment changes.

### 3. Verify

```bash
etai --version
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

## Toolchain

| Tool              | Purpose                                                                                   |
| :---------------- | :---------------------------------------------------------------------------------------- |
| **`etac`**        | Ahead-of-time compiler — `.eta` source to optimised `.etac` bytecode.                    |
| **`etai`**        | Interpreter — runs `.eta` source or pre-compiled `.etac` bytecode.                       |
| **`eta_repl`**    | Interactive REPL.                                                                         |
| **`eta_lsp`**     | Language server — diagnostics, completion, and navigation.                                |
| **`eta_dap`**     | Debug adapter — breakpoints, stepping, and inspection in VS Code.                         |
| **`eta_jupyter`** | Jupyter kernel — interactive notebooks with rich output.                                  |
| **`eta_test`**    | Test runner — TAP / JUnit output and VS Code Test Explorer integration.                   |

The VS Code extension adds a **Heap Inspector**, **Disassembly View**, and **GC Roots Tree** for inspecting the VM at runtime.

---

*License: [LICENSE.txt](LICENSE.txt)*
