<!--[eta_logo.svg](docs/img/eta.svg) -->

<p align="center">
  <img src="docs/img/eta.svg" alt="eta logo">
</p>


<p align="center">
  <strong>η (Eta)</strong><br>
  A Lisp/Scheme-inspired language with built-in logic programming,<br>
  automatic differentiation, neural networks, and causal inference.
</p>

<p align="center">
  <a href="docs/quickstart.md">Quick Start</a> ·
  <a href="docs/build.md">Build from Source</a> ·
  <a href="docs/architecture.md">Architecture</a> ·
  <a href="docs/nanboxing.md">NaN-Boxing</a> ·
  <a href="docs/bytecode-vm.md">Bytecode &amp; VM</a> ·
  <a href="docs/compiler.md">Compiler</a> ·
  <a href="docs/optimization.md">Optimization</a> ·
  <a href="docs/runtime.md">Runtime &amp; GC</a> ·
  <a href="docs/networking.md">Networking</a> ·
  <a href="docs/message-passing.md">Message Passing</a> ·
  <a href="docs/modules.md">Modules &amp; Stdlib</a> ·
  <a href="docs/next-steps.md">Next Steps</a>
</p>
<br>
<p align="center">
<strong>Language Guide</strong></p>
<p align="center">
  <a href="docs/examples.md">Basics</a> ·
  <a href="docs/aad.md">Reverse Mode AAD (Finance Examples)</a> ·
  <a href="docs/logic.md">Logic Programming – Unification and Backtracking</a> ·
  <a href="docs/clp.md">Constraint Logic Programming</a> ·
  <a href="docs/causal.md">Causal Inference &amp; Do-Calculus</a> ·
  <a href="docs/fact-table.md">Fact Tables</a> ·
  <a href="docs/torch.md">Neural Networks with libtorch</a>
</p>
<br>
<p align="center">
<strong>Featured Examples</strong></p>
<p align="center">
  <a href="docs/portfolio.md">Causal Decision Engine for Portfolio Optimisation</a>
</p>


---

## What is Eta?

**Eta** is a Scheme-like language designed for **symbolic reasoning,
differentiable programming, neural-network training, and causal
inference** — all from a single S-expression syntax.
It compiles to compact bytecode and runs on a stack-based VM implemented
in C++ (NaN-boxed values, closures, TCO, `call/cc`, hygienic macros,
module system — see [Architecture](docs/architecture.md) for the
full compilation pipeline).

### Headline Features

| Domain | What you get | Deep-dive |
|--------|-------------|-----------|
| **Scheme Core** | Closures, tail-call elimination, first-class continuations (`call/cc`), hygienic `syntax-rules` macros, module system with import filters | [Bytecode & VM](docs/bytecode-vm.md) · [Modules](docs/modules.md) |
| **Logic Programming** | VM-native structural unification & backtracking — seven dedicated opcodes give you Prolog-style pattern matching without leaving the language | [Logic](docs/logic.md) |
| **Constraint Logic Programming** | `clp(Z)` integer-interval and `clp(FD)` finite-domain solvers built on the unification layer | [CLP](docs/clp.md) |
| **Reverse-Mode AAD** | VM-native tape-based automatic differentiation — standard arithmetic is recorded transparently when a `TapeRef` operand is present; zero closure overhead | [AAD – Finance Examples](docs/aad.md) |
| **Neural Networks (libtorch)** | Native C++ bindings to PyTorch's backend — tensors, autograd, NN layers, optimizers, and GPU offload from Eta code | [Torch](docs/torch.md) |
| **Causal Inference** | Pearl's do-calculus engine, back-door / front-door adjustment, and end-to-end factor analysis | [Causal](docs/causal.md) |
| **Message Passing & Actors** | Erlang-style actor model via nng: `spawn` child processes, `send!`/`recv!` over PAIR sockets, `worker-pool` parallel fan-out, REQ/REP, PUB/SUB, SURVEYOR/RESPONDENT — network-transparent across machines | [Networking](docs/networking.md) · [Message Passing](docs/message-passing.md) |
| **End-to-End Pipeline** | All domains compose: symbolic differentiation → do-calculus identification → logic/CLP validation → libtorch neural estimation | [Causal Factor Pipeline](docs/causal-factor.md) |

The implementation ships as five executables and a VS Code extension:

<p align="center">
  <strong>Bytecode Compiler</strong> (<code>etac</code>)<br>
  <strong>Interpreter</strong> (<code>etai</code>)<br>
  <strong>Interactive REPL</strong> (<code>eta_repl</code>)<br>
  <strong>Language Server</strong> (<code>eta_lsp</code>)<br>
  <strong>Debug Adapter</strong> (<code>eta_dap</code>)<br>
  <strong>VS Code Extension</strong>
</p>

```scheme
;; Hello, Eta!
(module hello
  (import std.io)
  (begin
    (println "Hello, world!")

    (defun factorial (n)
      (if (= n 0) 1
          (* n (factorial (- n 1)))))

    (println (factorial 20))))
```
 
---

## Quick Start

Download the latest [release](https://github.com/lewismj/eta/releases)
for your platform, unpack it, and run the installer:

| Platform     | Archive                            |
|--------------|------------------------------------|
| Windows x64  | `eta-v0.2.0-win-x64.zip`          |
| Linux x86_64 | `eta-v0.2.0-linux-x86_64.tar.gz`  |

```bash
# Windows                              # Linux / macOS
cd eta-v0.2.0-win-x64                  cd eta-v0.2.0-linux-x86_64
.\install.cmd                          chmod +x install.sh && ./install.sh
```

The installer adds `bin/` to your `PATH`, sets `ETA_MODULE_PATH`, and
installs the VS Code extension automatically if VS Code is detected.

> [!NOTE]
> Open a **new** terminal after running the installer for the environment changes to take effect.

### Interpret from Source — `etai`

`etai` compiles a `.eta` file in-memory and executes it immediately:

```console
$ etai examples/hello.eta
Hello, world!
2432902008176640000
```

### Ahead-of-Time Compilation — `etac` + `etai`

`etac` compiles `.eta` source to compact `.etac` bytecode. `etai` then
loads `.etac` files directly, **skipping all front-end phases** (lex,
parse, expand, link, analyze, emit) for faster startup:

```console
$ etac examples/hello.eta
compiled examples/hello.eta → examples/hello.etac (3 functions, 1 module(s))

$ etai examples/hello.etac
Hello, world!
2432902008176640000
```

Key `etac` flags:

| Flag | Effect |
|------|--------|
| `-O` | Enable optimization passes (constant folding, dead code elimination) |
| `--disasm` | Print human-readable bytecode to stdout (no `.etac` written) |
| `--no-debug` | Strip source maps for a smaller output file |
| `-o <path>` | Custom output path (default: `<input>.etac`) |

```console
$ etac -O examples/hello.eta -o hello-opt.etac
$ etac --disasm examples/hello.eta
```

### Interactive REPL

```console
$ eta_repl
η> (+ 1 2 3 4 5)
=> 15
η> (exit)
```

### VS Code

The installer automatically sets up the VS Code extension. Configure the
paths in settings (`Ctrl+,` → search **Eta**):

```json
{
  "eta.lsp.serverPath":     "/path/to/eta-v0.2.0/bin/eta_lsp",
  "eta.dap.executablePath": "/path/to/eta-v0.2.0/bin/eta_dap"
}
```

Open the `examples/` folder, open any `.eta` file, and press **F5** to
debug. The extension provides:

- **Syntax highlighting** and **live diagnostics** (LSP)
- **Breakpoints & stepping** — F10 Step Over · F11 Step In · Shift+F11 Step Out · F5 Continue
- **Heap Inspector** — live memory gauge, per-kind object stats, GC root tree with drill-down (`Ctrl+Shift+P` → *Eta: Show Heap Inspector*)
- **Disassembly View** — live bytecode with current-PC marker in the Debug sidebar (`Ctrl+Shift+P` → *Eta: Show Disassembly*)
- **GC Roots Tree** — expandable root categories (Stack, Globals, Frames), module-grouped globals, object drill-down
- **Child Processes** — debug sidebar panel listing all spawned actor processes with PID, endpoint, and live/exited status

<img src="docs/img/eta_example_run.png" alt="Eta example run in VS Code" width="500">

> [!TIP]
> See [TLDR.md](TLDR.md) for a step-by-step walkthrough with screenshots,
> or [Quick Start](docs/quickstart.md) for the full reference.

---

### Build from Source

For contributors or those who want to build from source, see
**[Building from Source](docs/build.md)** — prerequisites, one-script
builds, manual CMake steps, and CI details.

Quick version:

```bash
# Linux / macOS
./scripts/build-release.sh ./dist/eta-release
cd dist/eta-release && ./install.sh

# Windows (PowerShell)
.\scripts\build-release.ps1 .\dist\eta-release
cd dist\eta-release; .\install.cmd
```

See [Language Guide](docs/examples.md) for a guided tour of the example programs.

### Bundle Layout

```
eta-v0.2.0-<platform>/
  bin/
    etac(.exe)          # Ahead-of-time bytecode compiler
    etai(.exe)          # File interpreter (also runs .etac files)
    eta_repl(.exe)      # Interactive REPL
    eta_lsp(.exe)       # Language Server (JSON-RPC over stdio)
    eta_dap(.exe)       # Debug Adapter (DAP over stdio, used by VS Code)
  stdlib/
    prelude.eta         # Auto-loaded standard library
    std/
      core.eta  math.eta  io.eta  collections.eta  test.eta
      logic.eta  clp.eta  causal.eta  fact_table.eta  torch.eta
      net.eta             # Networking & actor model (nng)
  examples/
    hello.eta           # Hello world & factorial
    basics.eta          # Arithmetic, let, lists, quoting
    functions.eta       # defun, lambda, closures, recursion
    higher-order.eta    # map, filter, fold, sort, zip
    composition.eta     # compose, flip, currying, pipelines
    recursion.eta       # Fibonacci, Ackermann, Hanoi
    exceptions.eta      # catch/raise, dynamic-wind
    boolean-simplifier.eta  # Symbolic boolean rewriting
    symbolic-diff.eta       # Symbolic differentiation & simplification
    unification.eta         # Native structural unification primitives
    logic.eta               # Relational logic programming
    aad.eta                 # Reverse-mode automatic differentiation
    xva.eta                 # Finance example: CVA, FVA calculations with AAD
    european.eta            # European option Greeks (1st & 2nd order) with AAD
    sabr.eta                # SABR vol surface with tape-based AD
    fact-table.eta          # Columnar fact tables with hash-indexed queries
    torch.eta               # Tensor computing & neural network training (libtorch)
    causal_demo.eta         # Demo: symbolic + causal + logic/CLP + libtorch
    message-passing.eta     # Erlang-style parent/child messaging (spawn/send!/recv!)
    message-passing-worker.eta
    worker-pool.eta         # Parallel fan-out: distribute tasks across N workers
    worker-pool-worker.eta
    echo-server.eta         # REP echo server (request-reply pattern)
    echo-client.eta         # REQ echo client
    pub-sub.eta             # PUB/SUB topic filtering
    pub-sub-publisher.eta
    scatter-gather.eta      # SURVEYOR/RESPONDENT scatter-gather
    scatter-gather-worker.eta
    parallel-map.eta        # Parallel map via worker-pool
    parallel-map-worker.eta
    monte-carlo.eta         # Parallel Monte Carlo π estimation
    monte-carlo-worker.eta
    distributed-compute.eta # Cross-machine TCP messaging (server + client)
    causal-factor/          # End-to-end causal factor analysis
    do-calculus/            # Do-calculus identification engine demos
  editors/
    eta-lang-<version>.vsix # VS Code extension
  install.sh / install.cmd
```

---

## Compilation Pipeline

Every Eta source file flows through six phases before execution.
The [`Driver`](eta/interpreter/src/eta/interpreter/driver.h) class
orchestrates the full pipeline and owns the runtime state:

```mermaid
flowchart LR
    SRC["Source\n(.eta)"] --> LEX["Lexer"]
    LEX --> PAR["Parser"]
    PAR --> EXP["Expander"]
    EXP --> LNK["Module\nLinker"]
    LNK --> SEM["Semantic\nAnalyzer"]
    SEM --> EMT["Emitter"]
    EMT --> VM["VM\nExecution"]
    EMT --> SER["Serialize\n(.etac)"]
    SER -.->|"etai fast-load"| VM

    style SRC fill:#2d2d2d,stroke:#58a6ff,color:#c9d1d9
    style LEX fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style PAR fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style EXP fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style LNK fill:#16213e,stroke:#79c0ff,color:#c9d1d9
    style SEM fill:#16213e,stroke:#79c0ff,color:#c9d1d9
    style EMT fill:#0f3460,stroke:#56d364,color:#c9d1d9
    style VM  fill:#0f3460,stroke:#56d364,color:#c9d1d9
    style SER fill:#0f3460,stroke:#56d364,color:#c9d1d9
```

| Phase                 | Input | Output | Header |
|-----------------------|-------|--------|--------|
| **Lexer**             | Raw UTF-8 text | Token stream | [`lexer.h`](eta/core/src/eta/reader/lexer.h) |
| **Parser**            | Tokens | S-expression AST (`SExpr`) | [`parser.h`](eta/core/src/eta/reader/parser.h) |
| **Expander**          | `SExpr` trees | Desugared core forms + macros | [`expander.h`](eta/core/src/eta/reader/expander.h) |
| **Module Linker**     | Expanded modules | Resolved imports/exports | [`module_linker.h`](eta/core/src/eta/reader/module_linker.h) |
| **Semantic Analyzer** | Linked modules | Core IR (`Node` graph) | [`semantic_analyzer.h`](eta/core/src/eta/semantics/semantic_analyzer.h) |
| **Emitter**           | Core IR | `BytecodeFunction`s | [`emitter.h`](eta/core/src/eta/semantics/emitter.h) |
| **VM**                | Bytecode | Runtime values (`LispVal`) | [`vm.h`](eta/core/src/eta/runtime/vm/vm.h) |

> [!NOTE]
> Every phase reports errors through a unified
> [`DiagnosticEngine`](eta/core/src/eta/diagnostic/diagnostic.h) with
> span information.

---

## Key Design Highlights

| Feature | Detail                                                                                                                                                                            |
|---------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **NaN-Boxing** | All values are 64-bit; doubles pass through unboxed while tagged types (fixnums, chars, symbols, heap pointers) are encoded in the NaN mantissa. [→ Deep-dive](docs/nanboxing.md) |
| **AOT Compilation** | `etac` compiles `.eta` → `.etac` bytecode; `etai` loads `.etac` files directly, skipping all front-end phases. Supports `-O` optimization passes (constant folding, DCE). [→ Deep-dive](docs/compiler.md) |
| **47-bit Fixnums** | Integers up to ±70 trillion are stored inline — no heap allocation.                                                                                                               |
| **Mark-Sweep GC** | Stop-the-world collector with sharded heap, hazard pointers, and a GC callback for auto-triggering on soft-limit. [→ Deep-dive](docs/runtime.md)                                  |
| **Tail-Call Elimination** | `TailCall` and `TailApply` opcodes reuse the current stack frame.                                                                                                                 |
| **First-Class Continuations** | `call/cc` captures the full stack + winding stack; `dynamic-wind` is supported.                                                                                                   |
| **Hygienic Macros** | `syntax-rules` with ellipsis patterns.                                                                                                                                            |
| **Module System** | `(module …)` forms with `import`/`export`, `only`, `except`, `rename` filters. [→ Deep-dive](docs/modules.md)                                                                     |
| **Arena Allocator** | IR nodes are block-allocated in a 16 KB arena for cache locality.                                                                                                                 |
| **Concurrent Heap** | `boost::unordered::concurrent_flat_map` with 16 shards for lock-free reads.                                                                                                       |
| **LSP Integration** | JSON-RPC language server for real-time diagnostics in any editor.                                                                                                                 |
| **DAP Integration** | Debug Adapter Protocol server (`eta_dap`) enables breakpoints, step-through debugging, call-stack inspection, and REPL-style expression evaluation directly in VS Code.           |
| **libtorch Integration** | Optional native bindings to PyTorch's C++ backend for tensors, autograd, neural-network layers, optimizers, and GPU offload. [→ Deep-dive](docs/torch.md) |
| **nng Networking** | Erlang-style actor model: `spawn` processes, `send!`/`recv!`, `worker-pool` for parallel fan-out, REQ/REP, PUB/SUB, SURVEYOR/RESPONDENT — network-transparent over IPC or TCP. [→ Deep-dive](docs/networking.md) · [Actor Model](docs/message-passing.md) |

---


## Documentation

| Page                                       | Contents                                                                                      |
|--------------------------------------------|-----------------------------------------------------------------------------------------------|
| **[Quick Start](docs/quickstart.md)**      | Installing, running `etai`/`etac`, REPL, modules, VS Code extension (full reference)          |
| **[Build from Source](docs/build.md)**     | Prerequisites, one-script builds, manual CMake, CI, testing                                   |
| **[Architecture](docs/architecture.md)**   | Full system diagram, phase-by-phase walkthrough, Core IR node types                           |
| **[NaN-Boxing](docs/nanboxing.md)**        | 64-bit memory layout, bit-field breakdown, encoding/decoding examples                         |
| **[Bytecode & VM](docs/bytecode-vm.md)**   | Opcode reference, end-to-end compilation trace, call stack model, TCO                         |
| **[Compiler (`etac`)](docs/compiler.md)**  | AOT bytecode compiler: CLI reference, `.etac` binary format, optimization passes, disassembly |
| **[Optimization](docs/optimization.md)**   | IR optimization pipeline architecture, built-in passes, writing custom passes                 |
| **[Runtime & GC](docs/runtime.md)**        | Heap architecture, object kinds, mark-sweep GC, intern table, factory                         |
| **[Modules & Stdlib](docs/modules.md)**    | Module syntax, linker phases, import filters, standard library reference                      |
| **[Language Guide](docs/examples.md)**     | Guided tour of the language using simple example programs with expected output                |
| **[Networking Primitives](docs/networking.md)** | nng socket API: `nng-socket`, `send!`, `recv!`, `nng-poll`, endpoints, error handling   |
| **[Message Passing & Actors](docs/message-passing.md)** | Actor model: `spawn`, `worker-pool`, REQ/REP, PUB/SUB, scatter-gather, timeouts  |
| **[Network & Message Passing Design](docs/network-message-passing.md)** | Full design doc: architecture, phases, nng rationale             |
| **[AAD – Finance Examples](docs/aad.md)** | Reverse-mode AD walkthrough, xVA sensitivities, European Greeks, SABR vol surface            |
| **[CLP](docs/clp.md)**                     | Constraint Logic Programming: clp(Z) intervals, clp(FD) finite domains, `clp:solve`           |
| **[Causal Inference](docs/causal.md)**     | Do-calculus engine, back-door adjustment, finance factor analysis                             |
| **[Fact Tables](docs/fact-table.md)**      | Columnar fact tables with hash-indexed queries, iteration, and fold                           |
| **[End-to-End Causal Pipeline](docs/causal-factor.md)** | Showcase: symbolic diff → do-calculus → logic/CLP → libtorch NN → ATE                             |
| **[Neural Networks](docs/torch.md)**       | libtorch integration: tensors, autograd, NN layers, training loops, GPU support               |
| **[Next Steps](docs/next-steps.md)**       | Roadmap: network stack, VS Code debugger improvements, performance                            |

---


## Standard Library

The prelude auto-loads the following modules:

| Module | Highlights |
|--------|------------|
| **`std.core`** | `identity`, `compose`, `flip`, `constantly`, `iota`, `assoc-ref`, list utilities |
| **`std.math`** | `pi`, `e`, `square`, `gcd`, `lcm`, `expt`, `sum`, `product` |
| **`std.io`** | `println`, `eprintln`, `read-line`, port redirection helpers |
| **`std.collections`** | `map*`, `filter`, `foldl`, `foldr`, `sort`, `zip`, `range`, vector ops |
| **`std.logic`** | `==`, `copy-term`, `naf`, `findall`, `run1` — Prolog-style combinators |
| **`std.clp`** | `clp:domain`, `clp:in-fd`, `clp:solve`, `clp:all-different` — constraint solving |
| **`std.causal`** | `dag:*`, `do:identify`, `do:estimate-effect` — causal inference engine |
| **`std.fact_table`** | `make-fact-table`, `fact-table-insert!`, `fact-table-query`, `fact-table-fold` — columnar fact tables |
| **`std.torch`** | `tensor`, `forward`, `train-step!`, `sgd`, `adam` — libtorch neural networks |
| **`std.net`** | `spawn`, `send!`, `recv!`, `worker-pool`, `with-socket`, `request-reply`, `pub-sub`, `survey` — Erlang-style actors & nng networking |
| **`std.test`** | `assert-equal`, `assert-true`, `run-tests` — lightweight test framework |

```scheme
(module my-app
  (import std.core)
  (import std.collections)
  (import std.io)
  (begin
    (define xs (iota 10))                    ;; (0 1 2 3 4 5 6 7 8 9)
    (println (filter odd? xs))               ;; (1 3 5 7 9)
    (println (foldl + 0 (filter even? xs)))  ;; 20
  ))
```

---

## Project Structure

```
eta/
├── CMakeLists.txt              # Top-level build
├── eta/
│   ├── core/                   # Shared library: reader + semantics + runtime
│   │   └── src/eta/
│   │       ├── reader/         # Lexer, Parser, Expander, Module Linker
│   │       ├── semantics/      # Semantic Analyzer, Core IR, Emitter, Arena
│   │       ├── runtime/        # NaN-box, VM, Heap, GC, Types, Primitives
│   │       └── diagnostic/     # Unified error reporting
│   ├── compiler/               # etac (AOT bytecode compiler)
│   ├── interpreter/            # etai + eta_repl (Driver orchestration)
│   ├── lsp/                    # eta_lsp (Language Server Protocol, JSON-RPC over stdio)
│   ├── dap/                    # eta_dap (Debug Adapter Protocol, DAP over stdio)
│   ├── nng/                    # nng networking layer (optional, -DETA_BUILD_NNG=ON)
│   ├── torch/                  # libtorch integration (optional, -DETA_BUILD_TORCH=ON)
│   ├── test/                   # Boost.Test unit tests
│   └── fuzz/                   # Fuzz testing (heap, intern table, nanbox)
├── stdlib/                     # Standard library (.eta files)
│   ├── prelude.eta             # Auto-loaded prelude
│   └── std/
│       ├── core.eta            # Combinators, list utilities, platform helpers
│       ├── math.eta            # Arithmetic, trig, gcd/lcm
│       ├── io.eta              # I/O primitives
│       ├── collections.eta     # map*, filter, foldl, sort, zip, range
│       ├── logic.eta           # Prolog-style unification & backtracking
│       ├── clp.eta             # Constraint Logic Programming: clp(Z), clp(FD)
│       ├── causal.eta          # DAG utilities & Pearl do-calculus engine
│       ├── fact_table.eta      # Columnar fact tables with hash indexes
│       ├── torch.eta           # libtorch wrappers (tensors, NN, optimizers)
│       ├── net.eta             # Networking & actor model (nng): spawn, send!, recv!, worker-pool
│       └── test.eta            # Lightweight test framework
├── examples/                   # Example programs
│   ├── hello.eta               # Hello world & factorial
│   ├── basics.eta              # Arithmetic, let, lists, quoting
│   ├── functions.eta           # defun, lambda, closures, recursion
│   ├── higher-order.eta        # map, filter, fold, sort, zip
│   ├── composition.eta         # compose, flip, currying, pipelines
│   ├── recursion.eta           # Fibonacci, Ackermann, Hanoi
│   ├── exceptions.eta          # catch/raise, dynamic-wind, re-raising
│   ├── boolean-simplifier.eta  # Symbolic boolean rewriting
│   ├── symbolic-diff.eta       # Symbolic differentiation & simplification
│   ├── unification.eta         # Native structural unification primitives
│   ├── logic.eta               # Relational logic programming (parento, findall)
│   ├── aad.eta                 # Reverse-mode automatic differentiation
│   ├── xva.eta                 # Finance: CVA, FVA with AAD sensitivities
│   ├── european.eta            # European option Greeks (1st & 2nd order) with AAD
│   ├── sabr.eta                # SABR vol surface with tape-based AD
│   ├── fact-table.eta          # Columnar fact tables with hash-indexed queries
│   ├── torch.eta               # Tensor computing & neural network training
│   ├── causal_demo.eta         # Flagship: symbolic + causal + logic/CLP + libtorch
│   ├── causal-factor/          # End-to-end causal factor analysis (finance)
│   ├── do-calculus/            # Do-calculus identification engine demos
│   └── torch_tests/            # libtorch integration test suite (10 files)
├── editors/vscode/             # VS Code extension (TextMate grammar)
├── scripts/                    # Build & install automation
└── docs/                       # Design documentation (you are here)
```

---

## License

See [LICENSE](LICENSE) for details.
