<div align="center" style="display: flex; justify-content: center; align-items: center; gap: 20px;">
  <img src="docs/img/eta.svg" alt="Eta logo" width="120">
  <div style="text-align: left;">
    <h1 style="margin: 0;">η (Eta)</h1>
    <p style="margin: 6px 0 0 0;">
      <strong>A Lisp-inspired language for symbolic, logic and constraint logic (CLP) programming, with machine learning, and causal inference.</strong>
    </p>
  </div>
</div>

<p align="center">
  <a href="docs/quickstart.md">Quick Start</a> ·
  <a href="docs/guide/language_guide.md">Language Guide</a> ·
  <a href="docs/guide/reference/vscode.md">VS Code</a> ·
  <a href="docs/guide/reference/jupyter.md">Jupyter</a> ·
  <a href="docs/architecture.md">Architecture</a>
</p>

<p align="center">
  <strong>Featured Examples</strong><br>
  Causal Decision Engine for Portfolio Optimisation 
  <a href="docs/featured_examples/portfolio.md">docs</a> ·
  <a href="examples/notebooks/Portfolio.ipynb">notebook</a><br>
  Wrong-Way Risk via do-interventions 
  <a href="docs/featured_examples/xva-wwr.md">docs</a>
</p>

<!--
<p align="center">
  <img src="docs/img/eta.svg" alt="Eta logo" width="120">
</p>
<h1 align="center">η (Eta)</h1>


<p align="center">
  <strong>A Lisp-inspired language for symbolic, logic and constraint logic (CLP) programming, with machine learning, and causal inference.</strong>
</p>

<p align="center">
  <a href="docs/quickstart.md">Quick Start</a> ·
  <a href="docs/guide/language_guide.md">Language Guide</a> ·
  <a href="docs/guide/reference/vscode.md">VS Code</a> ·
  <a href="docs/guide/reference/jupyter.md">Jupyter</a> ·
  <a href="docs/architecture.md">Architecture</a>
</p>

<p align="center">
  <strong>Featured Examples</strong><br>
  Causal Decision Engine for Portfolio Optimisation 
  <a href="docs/featured_examples/portfolio.md">docs</a> ·
  <a href="examples/notebooks/Portfolio.ipynb">notebook</a><br>
  Wrong-Way Risk via do-interventions 
  <a href="docs/featured_examples/xva-wwr.md">docs</a>
</p>
-->

---


## The Language

**Eta** is a modern Lisp/Scheme-inspired language designed for engineers and researchers who require a unified environment for **symbolic logic, differentiable programming, and high-performance numerics.** 

### Runtime Model

Eta is built around a **stack-based VM** that serves as a unified runtime for all language subsystems:

*   **Modern Scheme Core:** Nan-boxing, tail-call optimization (TCO), hygienic `syntax-rules` macros, and a module system. See [Language Guide](docs/guide/language_guide.md) for details.
*   **Native Logic Programming:** Built-in structural unification and backtracking via dedicated VM opcodes, enabling [Prolog-style](docs/guide/reference/logic.md) reasoning directly within the language.
*   **Differentiable Programming:** Native reverse-mode Automatic Differentiation [(AAD)](docs/guide/reference/aad.md) integrated into the runtime for transparent gradient computation with zero closure overhead.
*   **Machine Learning & Numerics:** First-class C++ bindings to [**libtorch**](docs/guide/reference/torch.md) for neural networks and **Eigen** for linear algebra and [statistics](docs/guide/reference/stats.md).
*   **Actor-Model Concurrency:** Erlang-style [actors](docs/guide/reference/network-message-passing.md) and message passing via **nng**, providing network-transparent parallelism and supervision trees.
*   **Causal Inference:** A built-in library for Pearl's do-calculus. See [causal](docs/guide/reference/causal.md) for supported identification and intervention operators.

---

## Getting Started

The easiest way to experience Eta is through the pre-built release bundles, which include the full toolchain and standard library.

### 1. Download the Release
Grab the latest bundle for your platform from the [Releases](https://github.com/lewismj/eta/releases) page:

*   **Windows x64:** `eta-v0.4.0-win-x64.zip`
*   **Linux x86_64:** `eta-v0.4.0-linux-x86_64.tar.gz`

### 2. Install
Unpack the archive and run the platform-specific installer. This script configures your `PATH`, sets up the `ETA_MODULE_PATH`, and registers the VS Code extension.

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

### 3. Verify Installation
Run the interpreter to ensure everything is set up correctly:
```bash
etai --version
```

---

## The Toolchain

Eta ships with a set of tools for development, debugging, and research:

| Tool              | Purpose                                                                                                            |
| :---------------- | :----------------------------------------------------------------------------------------------------------------- |
| **`etac`**        | **Ahead-of-Time Compiler:** Compiles `.eta` source code into optimized `.etac` bytecode.                           |
| **`etai`**        | **Interpreter:** Executes `.eta` source files or pre-compiled `.etac` bytecode.                                    |
| **`eta_repl`**    | **Interactive REPL:** A live environment for experimentation and evaluation.                                       |
| **`eta_lsp`**     | **Language Server:** Provides IDE features such as diagnostics, auto-completion, and navigation.                   |
| **`eta_dap`**     | **Debug Adapter:** Enables full-featured debugging (breakpoints, stepping, inspection) in VS Code.                 |
| **`eta_jupyter`** | **Jupyter Kernel:** Native support for interactive notebooks and rich data visualization.                          |
| **`eta_test`**    | **Test Runner:** Event-driven test execution system with VS Code Test Explorer integration and TAP output support. |


The VS Code Extension provides IDE support, including a **Heap Inspector**, **Disassembly View**, and a **GC Roots Tree** to visualize 
the internal state of the VM during execution.

---

## Example: 

Eta allows you to mix paradigms easily. In this snippet, we define a mathematical function and compute its gradient using the built-in AAD:

```scheme
(import std.math)
(import std.aad)
(import std.io)

(define (f x y)
  (+ (* x y) (sin x)))

;; Compute gradient at (2.0, 3.0)
(let ([res (grad f '(2.0 3.0))])
  (println "Value: " (car res))
  (println "Grad:  " (cadr res)))
```

---

## Explore More

*   **[Language Guide](docs/guide/language_guide.md)** — structured tour of the language, with deep dives into syntax, macros, logic, AAD, concurrency, and tooling.
*   **[Examples Tour](docs/guide/examples-tour.md)** — guided reading order for everything in [`examples/`](examples/).
*   **[Standard Library Reference](docs/guide/reference/modules.md)** — modules for logic, math, torch, actors, and more.
*   **[Architecture](docs/architecture.md)** — NaN-boxed VM and compilation pipeline.
*   **[Causal Inference Primer](docs/guide/reference/causal.md)** — Pearl's do-calculus in Eta.
*   **[Release Notes](docs/release-notes.md)** — version history.

---
*License: [LICENSE.txt](LICENSE.txt)*
