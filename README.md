<!--[eta_logo.svg](docs/img/eta.svg) -->

<p align="center">
  <img src="docs/img/eta.svg" alt="Eta logo" width="180">
</p>

<h1 align="center">η (Eta)</h1>

<p align="center">
  <strong>A Lisp-inspired language for symbolic, logic and constraint logic (CLP) programming, with machine learning, and causal inference.</strong>
</p>

<p align="center">
  <a href="docs/quickstart.md">Quick Start</a> ·
  <a href="docs/vscode.md">VS Code</a> ·
  <a href="docs/jupyter.md">Jupyter</a> ·
  <a href="docs/architecture.md">Architecture</a> ·
  <a href="docs/index.md">Documentation</a>
</p>

---

## The Language

**Eta** is a modern Lisp/Scheme-inspired language designed for engineers and researchers who require a unified environment for **symbolic logic, differentiable programming, and high-performance numerics.** 

### Runtime Model

Eta is built around a **stack-based VM** that serves as a unified runtime for all language subsystems:

*   **Modern Scheme Core:** Nan-boxing, tail-call optimization (TCO), hygienic `syntax-rules` macros, and a module system.
*   **Native Logic Programming:** Built-in structural unification and backtracking via dedicated VM opcodes, enabling Prolog-style reasoning directly within the language.
*   **Differentiable Programming:** Native reverse-mode Automatic Differentiation (AAD) integrated into the runtime for transparent gradient computation with zero closure overhead.
*   **Machine Learning & Numerics:** First-class C++ bindings to **libtorch** for neural networks and **Eigen** for linear algebra and statistics.
*   **Actor-Model Concurrency:** Erlang-style actors and message passing via **nng**, providing network-transparent parallelism and supervision trees.
*   **Causal Inference:** A built-in library for Pearl's do-calculus. See [causal](docs/causal.md) for supported identification and intervention operators.

## Featured Examples

* Causal Decision Engine for Portfolio Optimisation [docs](docs/portfolio.md) · [notebook](examples/notebooks/Portfolio.ipynb)

* Wrong Way Risk via do-interventions [docs](docs/xva-wwr.md)

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

*   **[Language Guide](docs/examples.md):** A tour through Eta's syntax and core libraries.
*   **[Standard Library](docs/modules.md):** Reference for logic, math, torch, actors, and more.
*   **[Architecture](docs/architecture.md):** Deep dive into the NaN-boxed VM and compilation pipeline.
*   **[Causal Inference Primer](docs/causal.md):** Introduction to Pearl's do-calculus in Eta.

---
*License: [LICENSE.txt](LICENSE.txt)*
