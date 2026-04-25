# Jupyter Kernel Implementation Plan (`xeus-eta`)

[← Back to README](../README.md) · [Next Steps](next-steps.md) ·
[DAP Plan](dap_vs_plan.md) · [Architecture](architecture.md) ·
[Bytecode & VM](bytecode-vm.md)

---

## Goal

Ship `xeus-eta`, a native C++ Jupyter kernel that embeds Eta's
`Driver` directly (no FFI), with **rich MIME displays for tensors,
fact tables, causal DAGs, CLP domains, logic substitutions, and
Vega-Lite plots**, plus comm-channel widgets that reuse the existing
DAP webviews (Heap Inspector, Disassembly, Child Processes).

The kernel is the project's **primary acquisition channel**: a
quant, researcher, educator, or evaluator who has never touched Eta
must be able to open a notebook on Binder, run six cells, and walk
away with a working mental model of why Eta exists.

The headline demo objective:

> **Open `04_european_option.ipynb`. Cell 1 imports `std.torch` and
> `std.stats`. Cell 2 prices a European call by Monte Carlo with
> 1 M paths. Cell 3 computes Δ, Γ, Vega via AAD with no source
> change. Cell 4 calibrates SABR by gradient descent through
> libtorch. Cell 5 renders the implied-vol surface as a Vega-Lite
> heatmap. Cell 6 spawns 8 actors and shows the live worker pool
> widget.**
>
> Total wall time on a laptop: < 60 s. Total lines of Eta written:
> ~80.

This plan is **stage-by-stage**, each stage independently buildable
and shippable. Every section names the concrete files to touch and
the existing types it builds on, mirroring the format of
[`dap_vs_plan.md`](dap_vs_plan.md).

---

## Repository Location

### Decision

The kernel C++ executable lives at **`eta/jupyter/`**. Any
JupyterLab front-end (TypeScript MIME renderers, comm widgets) lives
at **`editors/jupyterlab/`**.

### Rationale

| Option | Verdict | Why |
|---|---|---|
| `eta/jupyter/` | ✅ **Chosen** | Same shape as `eta/dap/`, `eta/lsp/`, `eta/repl/` — a C++ executable that links `eta_core` + `eta_interpreter`. CMake plumbing, install rules, and CTest hooks all match the existing pattern. |
| `editors/jupyter/` | ❌ | `editors/` is reserved for **client-side, language-specific assets** (TextMate grammars, snippets, language-config JSON, VS Code TS). A C++ kernel executable is not editor glue. |
| top-level `jupyter/` | ❌ | Breaks the "everything compiled lives under `eta/`" invariant the repo currently maintains. |

**The split is crisp:**

```
eta/jupyter/                     ← C++ kernel executable (eta_jupyter)
  src/eta/jupyter/
    main_jupyter.cpp
    eta_interpreter.{h,cpp}
    display.{h,cpp}
    magics.{h,cpp}
    comm/                         ← comm-channel handlers
      heap_comm.{h,cpp}
      disasm_comm.{h,cpp}
      actors_comm.{h,cpp}
      dag_comm.{h,cpp}
      tensor_comm.{h,cpp}
  resources/
    kernel.json.in                ← templated by --install
    logo-32x32.png
    logo-64x64.png
  CMakeLists.txt

editors/jupyterlab/              ← TypeScript labextension
  package.json
  tsconfig.json
  src/
    index.ts                      ← extension entrypoint
    mime/
      tensor.ts                   ← application/vnd.eta.tensor+json renderer
      facttable.ts                ← application/vnd.eta.facttable+json renderer
      dag.ts                      ← application/vnd.eta.dag+json renderer
      clp.ts
      logic.ts
    widgets/
      heap.ts                     ← eta.heap comm widget
      disasm.ts                   ← eta.disasm comm widget
      actors.ts                   ← eta.actors comm widget
  README.md
```

The classic Jupyter Notebook fallback (no labextension installed)
still works because every custom MIME bundle ships with an
`text/html` (or `image/svg+xml`) sibling rendered server-side.

---

## Architecture

```mermaid
flowchart LR
    NB[Jupyter Notebook / Lab] -- ZMQ --> XEUS[xeus core]
    XEUS -- C++ vtable --> EI[EtaInterpreter]
    EI -- shared --> DRV[(Driver)]
    DRV -- compiles --> VM[VM hot loop]

    subgraph IOPub
        EI -.stream/display_data/error.-> NB
    end

    subgraph Comm
        EI <-.eta.heap / eta.disasm / eta.actors / eta.dag / eta.tensor.-> NB
    end

    DRV -. interrupt flag .-> VM
```

- **Single shared `Driver`** per kernel, mirroring `eta_repl`. Cells
  share globals; redefinitions allowed.
- **Eval runs on a worker thread** owned by the kernel; the ZMQ
  shell socket stays responsive so `interrupt_request` can land.
- **`std.io` print routes through xeus iopub** by replacing the
  default `stdout` sink during a request.
- **Comm channels** are bidirectional: the notebook can ask the
  kernel for a heap snapshot (`eta.heap`) and the kernel can push
  live actor lifecycle events (`eta.actors`).

---

## Dependencies & Build Integration

### Implementation status (April 25, 2026)

- Implemented: `cmake/FetchXeus.cmake` with pinned versions and `ETA_USE_VCPKG` support.
- Implemented: `eta/jupyter/` scaffold (`CMakeLists.txt`, source layout, resources) with a buildable `eta_jupyter` target.
- Implemented: top-level options `ETA_BUILD_JUPYTER` (default `OFF`) and `ETA_USE_VCPKG` in `CMakeLists.txt`.
- Implemented: conditional `add_subdirectory(jupyter)` in `eta/CMakeLists.txt` and conditional `eta_jupyter` aggregation in `eta_all`.

### `cmake/FetchXeus.cmake` *(new)*

Mirrors the shape of [`cmake/FetchNng.cmake`](../cmake/FetchNng.cmake)
and [`cmake/FetchEigen.cmake`](../cmake/FetchEigen.cmake).

```cmake
include(FetchContent)

set(ETA_XEUS_TAG          "5.1.1"  CACHE STRING "xeus version")
set(ETA_XEUS_ZMQ_TAG      "3.0.0"  CACHE STRING "xeus-zmq version")
set(ETA_LIBZMQ_TAG        "v4.3.5" CACHE STRING "libzmq version")
set(ETA_CPPZMQ_TAG        "v4.10.0" CACHE STRING "cppzmq version")
set(ETA_NLOHMANN_JSON_TAG "v3.11.3" CACHE STRING "nlohmann_json version")

FetchContent_Declare(libzmq
  GIT_REPOSITORY https://github.com/zeromq/libzmq.git
  GIT_TAG        ${ETA_LIBZMQ_TAG})
FetchContent_Declare(cppzmq
  GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
  GIT_TAG        ${ETA_CPPZMQ_TAG})
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        ${ETA_NLOHMANN_JSON_TAG})
FetchContent_Declare(xeus
  GIT_REPOSITORY https://github.com/jupyter-xeus/xeus.git
  GIT_TAG        ${ETA_XEUS_TAG})
FetchContent_Declare(xeus-zmq
  GIT_REPOSITORY https://github.com/jupyter-xeus/xeus-zmq.git
  GIT_TAG        ${ETA_XEUS_ZMQ_TAG})

# Order matters: libzmq → cppzmq → json → xeus → xeus-zmq
FetchContent_MakeAvailable(libzmq cppzmq nlohmann_json xeus xeus-zmq)
```

#### Per-OS notes

| OS | Notes |
|---|---|
| Linux | Default. `apt install libssl-dev pkg-config` may be needed for libzmq's curve support. |
| macOS | Same. Apple Silicon: confirm libzmq builds universal; gate `-DZMQ_BUILD_TESTS=OFF`. |
| Windows | Prefer `vcpkg install libzmq cppzmq xeus xeus-zmq` and `find_package` instead of FetchContent — ZMQ's CMake on Windows has historical breakage. `FetchXeus.cmake` should detect `ETA_USE_VCPKG=ON` and switch paths. |

### `eta/jupyter/CMakeLists.txt` *(new)*

```cmake
add_executable(eta_jupyter
    src/eta/jupyter/main_jupyter.cpp
    src/eta/jupyter/eta_interpreter.cpp
    src/eta/jupyter/display.cpp
    src/eta/jupyter/magics.cpp
    src/eta/jupyter/comm/heap_comm.cpp
    src/eta/jupyter/comm/disasm_comm.cpp
    src/eta/jupyter/comm/actors_comm.cpp
    src/eta/jupyter/comm/dag_comm.cpp
    src/eta/jupyter/comm/tensor_comm.cpp)

target_link_libraries(eta_jupyter
    PRIVATE eta_core eta_interpreter
            xeus xeus-zmq nlohmann_json::nlohmann_json)

target_include_directories(eta_jupyter
    PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/resources)

install(TARGETS eta_jupyter RUNTIME DESTINATION bin)
install(FILES resources/logo-32x32.png resources/logo-64x64.png
        DESTINATION share/jupyter/kernels/eta)
```

### Top-level `CMakeLists.txt`

```cmake
option(ETA_BUILD_JUPYTER "Build the xeus-eta Jupyter kernel" ON)

if(ETA_BUILD_JUPYTER)
    include(cmake/FetchXeus.cmake)
    add_subdirectory(eta/jupyter)
endif()
```

---

## Stage 0 — Driver REPL Surface Audit

**Why first.** Every stage downstream calls into `Driver`. We need
to identify gaps before building on top.

### Implementation status (April 25, 2026)

- Implemented: `Driver::is_complete_expression(...)` with support for nested
  `#|...|#` comments, unterminated strings, parenthesis depth, and
  dot-prefixed continuation lines.
- Implemented: `Driver::request_interrupt()` and VM-side interrupt checks in
  the same hot-loop branch as the debug hook.
- Implemented: `eta::session::DisplayValue` and
  `Driver::eval_to_display(...)` with structured tags (including tensor
  detection).
- Implemented: `Driver::set_stream_sinks(...)` and
  `Driver::on_actor_lifecycle(...)` surface APIs.
- Implemented: `eta/test/src/driver_jupyter_test.cpp` with
  completeness, interrupt, and display-tag regressions.

### Existing surface (already used by `eta_repl`)

| API | Used for |
|---|---|
| `Driver::eval_string(std::string src, std::string& out)` | Cell evaluation |
| `Driver::completions_at(std::string prefix, size_t cursor)` | (LSP — to be lifted) |
| `Driver::hover_at(std::string symbol)` | (LSP — to be lifted) |
| `Driver::vm()` | Heap snapshot, disassembly, actor table |

### Gaps to fill

| New API | Purpose | File |
|---|---|---|
| `bool Driver::is_complete_expression(const std::string& src, std::string* indent_hint)` | `is_complete_request_impl` — must handle unbalanced parens, unterminated strings, dot-prefixed continuation, `#\|...\|#` block comments | `eta/session/src/eta/session/driver.h` |
| `void Driver::request_interrupt()` | Sets `vm_.interrupt_flag_`; checked in the same null-test slot as `debug_` (re-uses the DAP hook with zero hot-loop cost) | `eta/session/src/eta/session/driver.h`, `eta/core/src/eta/runtime/vm/vm.{h,cpp}` |
| `DisplayValue Driver::eval_to_display(const std::string& src)` | Returns a discriminated union (see Stage 5) instead of a formatted string | `eta/session/src/eta/session/driver.h` + `eta/session/src/eta/session/eval_display.h` |
| `Driver::set_stream_sinks(StreamSink stdout_sink, StreamSink stderr_sink)` | Routes `(println …)` to xeus iopub instead of process stdout | `eta/session/src/eta/session/driver.h`, `stdlib/std/io.eta` (publish hook) |
| `Driver::on_actor_lifecycle(std::function<void(ActorEvent)>)` | Live actor widget feed | `eta/session/src/eta/session/driver.h` |

### Tests

Add `eta/test/src/driver_jupyter_test.cpp`:

- `is_complete_expression("(+ 1")` → `false`, indent hint `"  "`.
- `is_complete_expression("(+ 1 2)")` → `true`.
- `is_complete_expression("\"unterminated")` → `false`.
- `request_interrupt()` from another thread terminates a `(loop)`
  within 50 ms.
- `eval_to_display("(tensor:zeros 3 3)")` returns a `DisplayValue`
  whose tag is `Tensor`.

### Definition of done

`eta_core_test` passes a regression that interrupts a runaway cell,
returns a structured `DisplayValue`, and reports the right
`is_complete_expression` answer for 12 representative inputs.

---

## Stage 1 — Skeleton Kernel & Kernelspec

**Goal.** Smallest possible kernel that boots, prints `(+ 1 2) ⇒ 3`,
and registers a `kernel.json` Jupyter can find.

### Files

| File | Content |
|---|---|
| `eta/jupyter/src/eta/jupyter/main_jupyter.cpp` | xeus boot: builds `xeus::xkernel` with `EtaInterpreter`, calls `start()`. |
| `eta/jupyter/src/eta/jupyter/eta_interpreter.{h,cpp}` | `class EtaInterpreter : public xeus::xinterpreter`. |
| `eta/jupyter/resources/kernel.json.in` | Templated kernelspec (see below). |
| `eta/jupyter/resources/logo-{32,64}x{32,64}.png` | Rendered from `docs/img/eta_logo.svg`. |

### `EtaInterpreter` minimum vtable

```cpp
class EtaInterpreter : public xeus::xinterpreter {
public:
    EtaInterpreter();
    ~EtaInterpreter() override;

protected:
    void configure_impl() override;
    nl::json execute_request_impl(int execution_counter,
                                  const std::string& code,
                                  bool silent,
                                  bool store_history,
                                  nl::json user_expressions,
                                  bool allow_stdin) override;
    nl::json complete_request_impl(const std::string&, int) override;
    nl::json inspect_request_impl(const std::string&, int, int) override;
    nl::json is_complete_request_impl(const std::string&) override;
    nl::json kernel_info_request_impl() override;
    void shutdown_request_impl() override;

private:
    std::unique_ptr<eta::session::Driver> driver_;
    std::mutex driver_mu_;
};
```

### `--install` flow

```
eta_jupyter --install            # writes ~/.local/share/jupyter/kernels/eta/
eta_jupyter --install --user
eta_jupyter --install --prefix=/opt/eta
eta_jupyter --install --sys-prefix
```

`kernel.json.in`:

```json
{
  "argv": ["@CMAKE_INSTALL_PREFIX@/bin/eta_jupyter", "-f", "{connection_file}"],
  "display_name": "Eta",
  "language": "eta",
  "metadata": { "language_info": {
      "name": "eta",
      "mimetype": "text/x-eta",
      "file_extension": ".eta",
      "pygments_lexer": "scheme"
  }}
}
```

### Tests

- `eta_jupyter --install --prefix=$tmp` produces `kernels/eta/kernel.json`
  with the right `argv[0]`.
- `jupyter kernelspec list` (CI) shows the kernel.
- `jupyter run hello.ipynb` (one cell `(+ 1 2)`) outputs `3`.

### Definition of done

`jupyter run examples/notebooks/01_hello_eta.ipynb` exits 0 and the
output cell contains `3`.

---

## Stage 2 — Completions, Hover, `is_complete`

**Builds on:** Stage 0 (`is_complete_expression`).

### Wiring

| Jupyter call | Routes to |
|---|---|
| `complete_request_impl` | `Driver::completions_at(code, cursor_pos)` — returns `{matches, cursor_start, cursor_end}`. The current LSP completion/hover logic lives in `eta/lsp/src/eta/lsp/lsp_server.cpp`; lift the prefix-extraction logic into a shared header `eta/interpreter/src/eta/interpreter/repl_complete.h` so both the LSP and the kernel call the same code. |
| `inspect_request_impl` | `Driver::hover_at(symbol)` — returns the same Markdown the LSP hover produces. Wrapped as `{found: true, data: {"text/markdown": …}}`. |
| `is_complete_request_impl` | `Driver::is_complete_expression(code, &indent)` → `{status: "complete"\|"incomplete", indent}`. |

### Tests

- Tab-complete `(import std.to` returns `std.torch`.
- Hover on `defrel` returns the Markdown signature from
  [`docs/logic.md`](logic.md).
- `is_complete` returns `incomplete` for an open `(let (`.

---

## Stage 3 — Stream Capture (`stdout` / `stderr`)

**Goal.** `(println "hi")` shows up incrementally in the cell, not
buffered until completion.

### Approach

- New `Driver::set_stream_sinks(stdout_sink, stderr_sink)` where
  `StreamSink = std::function<void(std::string_view)>`.
- `EtaInterpreter::execute_request_impl` sets sinks at request entry
  to lambdas that call `publish_stream("stdout", chunk)` /
  `publish_stream("stderr", chunk)`.
- `std.io` `print`, `println`, `display`, `write` are the only
  emission points; they all go through one C++ helper
  (`runtime::io::write`) that consults the current sink.

### Thread safety

- The eval worker thread calls the sink under no lock (xeus
  serialises iopub publishes internally).
- `spawn-thread` actors capture the sink at spawn time so their
  output continues to land in the originating cell (see Stage 8).

### Tests

- `(begin (println "a") (sleep 100) (println "b"))` produces two
  `stream` messages with the right delay between them.
- `(error "boom")` writes to `stderr`, not `stdout`.

---

## Stage 4 — Errors & Tracebacks

### Approach

`EtaInterpreter::execute_request_impl` wraps the eval call in
`try/catch (eta::runtime::Exception& e)`. The exception already
carries `span` + `kind` + `message`; format it like the REPL does:

```
RuntimeError: division by zero
  at examples/european.eta:42:18
    (set! premium (/ payoff 0))
                  ^^^^^^^^^^^^^
  at examples/european.eta:30:6
    (price-call S K r sigma T)
```

Map to Jupyter `error` reply:

```cpp
return {
  {"status",    "error"},
  {"ename",     to_string(e.kind())},     // "RuntimeError"
  {"evalue",    e.message()},
  {"traceback", ansi_lines}                 // vector<string>
};
```

ANSI colour codes for the source caret line use the same palette as
the LSP diagnostic renderer.

### Tests

- `(error "x")` produces an `error` reply with `ename: "UserError"`,
  one traceback frame.
- Nested call chain produces N frames in correct order.

---

## Stage 5 — Rich Display (the showcase machinery)

**This is the visual payoff.** Every other front-end (Python,
Julia, R) renders matplotlib PNGs and pandas tables. Eta will do
**tensors, fact tables, causal DAGs, CLP domains, logic
substitutions, and Vega-Lite plots, with classic-Notebook fallbacks
that don't need a labextension installed**.

### `DisplayValue` discriminated union

`eta/jupyter/src/eta/jupyter/display.h`:

```cpp
struct DisplayValue {
    enum class Kind {
        Plain,         // anything not specialised
        Tensor,        // torch::Tensor handle
        FactTable,     // Eigen-backed std.fact-table
        Dag,           // causal DAG / symbolic graph
        ClpDomain,     // CLP(FD) domain set
        LogicSubst,    // miniKanren substitution
        Plot,          // Vega-Lite spec built by std.stats helpers
        Image,         // (jupyter:png …) / (jupyter:svg …)
        Html,          // (jupyter:html …)
        Markdown,      // (jupyter:markdown …)
        Latex          // (jupyter:latex …)
    } kind;

    nlohmann::json data;     // type-specific payload
    std::string    text_repr; // always populated, for `text/plain`
};
```

### Detection rules (O(1) on tag inspection — never traverse)

| Tag in `LispVal` | DisplayValue kind |
|---|---|
| `Tag::TorchTensor` | `Tensor` |
| `Tag::FactTable` | `FactTable` |
| `Tag::CausalDag` | `Dag` |
| `Tag::ClpFdDomain` | `ClpDomain` |
| `Tag::LogicSubst` (returned by `run`) | `LogicSubst` |
| `Tag::JupyterDisplay` (boxed by `(jupyter:*)` helpers) | matches embedded `mime` |
| anything else | `Plain` |

### MIME bundle catalogue

Each kind emits the richest MIME for the active frontend, plus
optional fallbacks (classic Notebook, plain text, or both).

| `DisplayValue.kind` | Primary MIME | Fallback MIMEs | Server-side renderer |
|---|---|---|---|
| `Plain` | `text/plain` | — | Default REPL formatter |
| `Tensor` | `application/vnd.eta.tensor+json` | `text/html` (≤2D table; ≥3D shape + first/last 3 along axis 0), `image/png` (heatmap for 2D float) | `display::render_tensor` (Eigen → PNG via `stb_image_write`) |
| `FactTable` | `application/vnd.eta.facttable+json` | `text/html` (Tabulator-like sortable table, capped 1000 rows + "show more" hint), `text/plain` (tab-separated) | `display::render_facttable` |
| `Dag` | `application/vnd.eta.dag+json` | `image/svg+xml` (Graphviz `dot` if on PATH; degrades to `text/plain` adjacency list otherwise) | `display::render_dag` (shells out to `dot -Tsvg`) |
| `ClpDomain` | `application/vnd.eta.clp-domain+json` | `text/plain` (e.g. `{1..3, 7, 12..15}`) | `display::render_clp_domain` |
| `LogicSubst` | `application/vnd.eta.logic-subst+json` | `text/markdown` (table of bindings + residual constraints) | `display::render_logic_subst` |
| `Plot` | `application/vnd.vegalite.v5+json` | `image/svg+xml` (offline Vega → SVG via embedded vendored renderer is too heavy; instead emit a simple matplotlib-style SVG fallback for line/bar/scatter only) | `display::render_plot` |
| `Image` (PNG) | `image/png` | — | passthrough |
| `Image` (SVG) | `image/svg+xml` | — | passthrough |
| `Html` | `text/html` | — | passthrough |
| `Markdown` | `text/markdown` | `text/plain` | passthrough |
| `Latex` | `text/latex` | `text/plain` | passthrough |

### Eta-side display helpers (new prelude bindings)

Added to `stdlib/std/jupyter.eta` and re-exported from `prelude.eta`
**only when running under the kernel** (detected via env var
`ETA_KERNEL=1` set by `EtaInterpreter::configure_impl`):

```scheme
(jupyter:display obj)              ; returns last-expression-equivalent
(jupyter:html "<div>…</div>")
(jupyter:markdown "# heading")
(jupyter:png  bytes)               ; bytes is a bytevector
(jupyter:svg  "<svg …/>")
(jupyter:vega vega-lite-spec)      ; spec is a Scheme assoc-list → JSON
(jupyter:latex "\\frac{a}{b}")
(jupyter:table fact-table)         ; force fact-table render
(jupyter:plot 'line  xs ys)        ; std.stats-aware sugar over jupyter:vega
(jupyter:plot 'scatter xs ys)
(jupyter:plot 'heatmap matrix)
```

When the user evaluates `(jupyter:html "...")` *outside* the kernel
(e.g. from `etai`), the helpers degrade to printing the text repr —
no error.

### `publish_execute_result` shape

```cpp
void EtaInterpreter::publish_result(int counter, const DisplayValue& dv) {
    nl::json bundle, metadata = nl::json::object();
    bundle["text/plain"] = dv.text_repr;
    switch (dv.kind) {
        case DisplayValue::Kind::Tensor:
            bundle["application/vnd.eta.tensor+json"] = dv.data;
            bundle["text/html"]  = display::render_tensor_html(dv.data);
            if (display::is_2d_float(dv.data))
                bundle["image/png"] = display::render_tensor_png(dv.data);
            break;
        case DisplayValue::Kind::FactTable:
            bundle["application/vnd.eta.facttable+json"] = dv.data;
            bundle["text/html"]  = display::render_facttable_html(dv.data);
            break;
        // … one case per kind …
    }
    publish_execute_result(counter, std::move(bundle), std::move(metadata));
}
```

### Tests

For each MIME kind, a notebook smoke test under
`eta/test/src/jupyter/` drives `nbclient` programmatically and
asserts the cell outputs contain the expected MIME keys.

### Definition of done

All 11 MIME types render in **both** classic Notebook (fallback
path) and JupyterLab with the labextension installed.

---

## Stage 6 — Comm-Channel Widgets (live debug surface)

**Goal.** Heap Inspector, Disassembly, Child Processes, DAG
explorer, and Tensor slicer as live JupyterLab widgets — all
reusing logic already shipped for the DAP webviews.

### Comm targets

| Target | Server handler | Front-end widget | Eta helper |
|---|---|---|---|
| `eta.heap` | `comm/heap_comm.cpp` → existing `eta/heapSnapshot` payload | `editors/jupyterlab/src/widgets/heap.ts` | `(jupyter:heap)` |
| `eta.disasm` | `comm/disasm_comm.cpp` → `eta/disassemble` | `widgets/disasm.ts` | `(jupyter:disasm 'fn-name)` |
| `eta.actors` | `comm/actors_comm.cpp` → `eta/childProcesses`, plus push on `Driver::on_actor_lifecycle` | `widgets/actors.ts` | `(jupyter:actors)` |
| `eta.dag` | `comm/dag_comm.cpp` (Cytoscape.js render) | `widgets/dag.ts` | `(jupyter:dag g)` |
| `eta.tensor` | `comm/tensor_comm.cpp` (slice browser for ≥3D) | `widgets/tensor.ts` | `(jupyter:tensor-explorer t)` |

### Code reuse strategy

The DAP webviews live in
[`editors/vscode/src/heapView.ts`](../editors/vscode/src/heapView.ts),
`disassemblyView.ts`, etc. The HTML/CSS/JS payloads are useful
verbatim; the **transport** differs (VS Code postMessage vs.
JupyterLab comm).

**Pragmatic choice for v1:** duplicate the rendering JS into
`editors/jupyterlab/src/widgets/` rather than block on extracting a
shared `editors/shared/webviews/` module. Track the dedup as a
follow-up:

> **Tech-debt ticket:** factor the rendering layer of
> `heapView.ts` / `disassemblyView.ts` / `childProcessTreeView.ts`
> into framework-agnostic ES modules under
> `editors/shared/webviews/`, consumed by both the VS Code
> extension and the JupyterLab labextension.

### Classic Notebook fallback

The labextension is optional. Without it, `(jupyter:heap)` returns
a `DisplayValue` of kind `Html` with a static snapshot. Live
updates are lab-only.

### Tests

- `(jupyter:heap)` round-trips a snapshot through the comm; the
  widget renders the same JSON the DAP webview consumes.
- Spawning a thread actor produces an `eta.actors` push that the
  widget displays within 100 ms.

---

## Stage 7 — Magics & Cell Directives

Dispatched in `EtaInterpreter::execute_request_impl` **before** the
code is handed to `Driver`. Lines starting with `%` (line magic) or
cells starting with `%%` (cell magic) are intercepted.

| Magic | Behaviour |
|---|---|
| `%time EXPR` | Evaluate once, prepend wall-time + GC-time to the result |
| `%timeit EXPR` | Repeat ≥10× or 1 s; report mean/std |
| `%bytecode EXPR` | Disassemble the compiled form instead of running |
| `%load PATH` | Slurp file into the cell input area (sends `set_next_input`) |
| `%run PATH` | Evaluate `(load PATH)` |
| `%env [K[=V]]` | Get/set env var |
| `%cwd [PATH]` | Get/set chdir |
| `%import MOD` | Sugar for `(import MOD)` |
| `%reload MOD` | Clear module cache entry for `MOD` before re-import |
| `%who` / `%whos` | List current globals (name + type for `%whos`) |
| `%plot EXPR` | Coerce result to `Plot` kind |
| `%table EXPR` | Coerce result to `FactTable` kind |
| `%%trace` | Cell magic: enable VM trace for this cell, dump ops to a collapsible HTML widget |

Implementation lives in `eta/jupyter/src/eta/jupyter/magics.{h,cpp}`
as a single switch — no plugin system in v1.

### Tests

- `%time (sleep 100)` reports ≥ 100 ms.
- `%bytecode (lambda (x) (+ x 1))` outputs disassembly.
- `%reload std.foo` invalidates `std.foo` module cache entry.

---

## Stage 8 — Async, Interrupts, Actor Routing

### Interrupts

`xeus::xinterpreter` calls `interrupt_request_impl` (default
behaviour: nothing). Override to call
`driver_->request_interrupt()`.

VM hot loop already has a single null-pointer check for `debug_`
(see [`dap_vs_plan.md`](dap_vs_plan.md) §"Performance"). Add one more
test in the same slot:

```cpp
if (UNLIKELY(debug_ || interrupt_flag_.load(std::memory_order_relaxed))) {
    if (interrupt_flag_.load()) throw runtime::InterruptException{};
    debug_->check_and_wait(span, depth);
}
```

Cost: one additional load per opcode, predictably false → free.

### Actor stdout routing

`spawn-thread` actors capture the **current iopub sinks** at spawn
time via thread-local plumbing in `Driver::set_stream_sinks`. Their
`println` output therefore lands in the originating cell even after
that cell has finished evaluating. (Document this behaviour in the
user-facing `docs/jupyter.md`.)

`spawn` (process actors over inproc/IPC) cannot easily route stdout
back; instead, `(jupyter:show pid)` produces a live widget showing
the actor's status + last-N output frames, fed by the supervisor's
existing log buffer.

### Tests

- `(loop)` interrupts cleanly; subsequent cells still work.
- `(spawn-thread (lambda () (println "hi")))` output lands in the
  spawning cell's output area.

---

## Stage 9 — State Model & Cell Semantics

### Decisions

| Question | Decision |
|---|---|
| Shared `Driver` per kernel vs. per-cell | **Shared** — matches `eta_repl`, allows iterative development. Crash recovery = "Restart kernel". |
| Auto-imports in cell 0 | `(import std.io)` always; user-configurable list via `~/.config/eta/kernel.toml` `[autoimport]` section |
| Working directory | Kernel process CWD at startup (typically the Jupyter server root); `%cwd` can override per session |
| Module cache invalidation | `%reload std.foo` magic clears the module cache for that module |
| Cell re-execution | Standard Jupyter — cell can mutate globals; redefinitions allowed |

### `kernel.toml` example

```toml
[autoimport]
modules = ["std.io", "std.logic", "std.stats"]

[display]
table_max_rows  = 1000
tensor_preview  = 8        # show first/last N along axis 0
plot_theme      = "dark"

[interrupt]
hard_kill_after_seconds = 30   # if soft interrupt fails
```

---

## Stage 10 — Packaging & Distribution

### conda-forge recipe

`recipes/xeus-eta/meta.yaml` (sketch):

```yaml
package:
  name: xeus-eta
  version: "0.1.0"
source:
  git_url: https://github.com/<owner>/eta
  git_rev: v0.1.0
build:
  number: 0
requirements:
  build:
    - {{ compiler('cxx') }}
    - cmake >=3.21
    - ninja
  host:
    - xeus 5.*
    - xeus-zmq 3.*
    - libtorch
    - eigen
    - nng
  run:
    - xeus 5.*
    - xeus-zmq 3.*
    - libtorch
test:
  commands:
    - eta_jupyter --version
    - python -c "import json, subprocess; ks = json.loads(subprocess.check_output(['jupyter','kernelspec','list','--json'], text=True))['kernelspecs']; assert 'eta' in ks"
```

### PyPI wheel

Use [`scikit-build-core`](https://scikit-build-core.readthedocs.io/)
to ship `pip install xeus-eta`. Wheel post-install runs
`eta_jupyter --install --sys-prefix`.

### Binder

`binder/`:

```
binder/
  apt.txt              # graphviz, libssl-dev
  environment.yml      # xeus-eta from conda-forge, jupyterlab
  postBuild            # eta_jupyter --install --sys-prefix
                       # jupyter labextension install @eta-lang/jupyterlab
```

README hero gets a Binder badge that opens
`examples/notebooks/00_index.ipynb`.

### Docker

`docker/jupyter/Dockerfile` based on `jupyter/scipy-notebook:latest`,
adds the kernel + libtorch + graphviz. Tag scheme:
`etalang/jupyter:<version>` and `:latest`.

### CI

GitHub Actions matrix:

| OS | Build | Notebook smoke (`nbconvert --execute`) |
|---|---|---|
| ubuntu-latest | ✅ | ✅ all 11 notebooks |
| macos-latest | ✅ | ✅ subset (skip libtorch CUDA) |
| windows-latest | ✅ (vcpkg) | ✅ subset |

---

## Stage 11 — Demo Notebooks (the actual showcase)

All under `examples/notebooks/`. Each notebook follows a
**setup → hook → payoff → docs link** beat sheet.

### `00_index.ipynb`

Landing page. Markdown cells with thumbnails linking to each demo.
The Binder badge boots into this notebook.

### `01_hello_eta.ipynb` — REPL feel

| Beat | Cell |
|---|---|
| Setup | `(import std.io) (println "hello eta")` |
| Hook | Tab-complete inside `(import std.tor` → `std.torch` |
| Payoff | Hover on `defrel` shows the docstring inline |
| Docs | → [`docs/quickstart.md`](quickstart.md) |

### `02_logic_minikanren.ipynb` — Logic that other Schemes don't have

| Beat | Cell |
|---|---|
| Setup | `(import std.logic)` and define `appendo` |
| Hook | `(run* (q) (appendo '(1 2) '(3 4) q))` returns the substitution rendered as a Markdown table |
| Payoff | Solve send-more-money via `clp:fd`; result table |
| Docs | → [`docs/logic.md`](logic.md), [`examples/send-more-money.eta`](../examples/send-more-money.eta) |

### `03_clp_portfolio.ipynb` — Convex QP in three lines

| Beat | Cell |
|---|---|
| Setup | Fact table of 50 assets (rendered as sortable HTML table) |
| Hook | `(clp:r:minimise (qf w Σ w) subject-to: …)` |
| Payoff | Vega-Lite bar chart of optimal weights; risk-return scatter |
| Docs | → [`docs/portfolio.md`](portfolio.md), [`examples/portfolio-lp.eta`](../examples/portfolio-lp.eta) |

### `04_european_option.ipynb` — Monte Carlo + AAD greeks (the headline demo)

| Beat | Cell |
|---|---|
| Setup | Define `(price-call S K r σ T n)` Monte Carlo with 1 M paths |
| Hook | Compute Δ, Γ, Vega via `(grad price-call …)` — same source, no rewrite |
| Payoff | Convergence plot of price vs. n via `(jupyter:plot 'line)` |
| Docs | → [`docs/european.md`](european.md), [`docs/aad.md`](aad.md) |

### `05_sabr_calibration.ipynb` — libtorch under the hood

| Beat | Cell |
|---|---|
| Setup | Load market vol surface as fact-table |
| Hook | Calibrate SABR (`α, β, ρ, ν`) by Adam through libtorch |
| Payoff | Implied-vol heatmap (PNG) before/after calibration |
| Docs | → [`docs/sabr.md`](sabr.md), [`examples/sabr.eta`](../examples/sabr.eta) |

### `06_xva_pipeline.ipynb` — Multi-stage pipeline + FactTable rendering

| Beat | Cell |
|---|---|
| Setup | Load trades, market data, collateral schedule |
| Hook | Compute CVA / DVA / FVA via Monte Carlo |
| Payoff | FactTable widget showing per-counterparty XVA, sortable |
| Docs | → [`docs/xva.md`](xva.md), [`examples/xva.eta`](../examples/xva.eta) |

### `07_causal_dag.ipynb` — DAG widget on do-calculus

| Beat | Cell |
|---|---|
| Setup | Define a 6-node SCM |
| Hook | `(jupyter:dag g)` renders interactive Cytoscape widget |
| Payoff | `(do-calculus g 'X 'Y)` returns the identifying formula as LaTeX |
| Docs | → [`docs/causal.md`](causal.md), [`examples/causal_demo.eta`](../examples/causal_demo.eta) |

### `08_actors_supervision.ipynb` — Erlang-style supervision live

| Beat | Cell |
|---|---|
| Setup | Spawn a worker pool of 8 actors under a `one-for-one` supervisor |
| Hook | Live actor widget shows them; kill one — supervisor restarts it |
| Payoff | `parallel-fib` across the pool; latency histogram |
| Docs | → [`docs/supervisor.md`](supervisor.md), [`examples/worker-pool.eta`](../examples/worker-pool.eta) |

### `09_clpb_sat.ipynb` — Boolean SAT via CLP(B)

| Beat | Cell |
|---|---|
| Setup | Encode an N-queens-style boolean problem |
| Hook | `(clp:b:sat? formula)` |
| Payoff | Truth-assignment table |
| Docs | → [`docs/clpb.md`](clpb.md), [`examples/boolean-simplifier.eta`](../examples/boolean-simplifier.eta) |

### `10_torch_train.ipynb` — Train a NN, plot loss live

| Beat | Cell |
|---|---|
| Setup | `(import std.torch)`; build `Sequential` MLP |
| Hook | Loop calling `train-step!`; each step pushes loss to a live Vega-Lite plot via comm channel |
| Payoff | Final accuracy + a confusion-matrix heatmap |
| Docs | → [`docs/torch.md`](torch.md), [`examples/torch.eta`](../examples/torch.eta) |

### Notebook test gate

CI runs every notebook with `jupyter nbconvert --to notebook
--execute --ExecutePreprocessor.timeout=120`. Failure on any
notebook is a blocking CI failure.

---

## Stage 12 — `docs/jupyter.md` (User-Facing Reference)

A standalone reference page (sibling to
[`dap_vs_plan.md`](dap_vs_plan.md)) covering:

- **Install:** `pip install xeus-eta` / `mamba install -c conda-forge xeus-eta` / Binder badge / Docker image
- **Magics reference:** complete list with examples
- **MIME catalogue:** every `application/vnd.eta.*+json` schema
  documented
- **Eta-side helpers:** `(jupyter:*)` reference
- **Comm widgets:** when each appears, how to invoke
- **Configuration:** `~/.config/eta/kernel.toml` keys
- **Troubleshooting:** kernel won't start, no rich rendering
  (labextension missing), interrupt doesn't fire, libtorch GPU
  detection
- **Architecture diagram** lifted from this plan

The README hero acquires a "Try Eta in your browser →" Binder badge
linking to `00_index.ipynb`.

---

## Sequencing & Sizing

| Stage | Depends on | Size | Risk |
|---|---|---|---|
| 0 — Driver audit | — | M (3–5 d) | Med (touches VM hot loop) |
| 1 — Skeleton kernel | 0 | M (3–5 d) | Low |
| 2 — Completions / hover | 1 | S (2 d) | Low |
| 3 — Stream capture | 0, 1 | S (2 d) | Low |
| 4 — Errors / tracebacks | 1 | S (1–2 d) | Low |
| 5 — Rich display | 1, 3 | L (2 wk) | Med (renderer breadth) |
| 6 — Comm widgets | 5 | L (2 wk) | Med (labextension toolchain) |
| 7 — Magics | 1 | S (2 d) | Low |
| 8 — Interrupts / actor routing | 0, 3 | M (3–5 d) | Med (concurrency) |
| 9 — State model | 1 | S (1–2 d) | Low |
| 10 — Packaging | 1 | M (1 wk) | Med (CI matrix) |
| 11 — Demo notebooks | 1, 5 (7 optional for magic-heavy cells) | M (1 wk; rolls out per stage) | Low |
| 12 — `docs/jupyter.md` | all of above | S (2 d) | Low |

### Recommended order — **demo-first MVP**

The plan deliberately frontloads the path to a *runnable Binder demo*:

1. **Stage 0 → 1 → 4 → 3** — kernel boots, runs cells, prints,
   reports errors. (≈ 1.5 wk)
2. **Stage 5 (Tensor + FactTable + Plot only)** — enough rich
   display for the four headline notebooks. (≈ 1 wk)
3. **Stage 11 (notebooks 01, 03, 04, 07)** + **Stage 9** + **Stage 10
   (Binder + Docker only)** — **MVP demo ships.** (≈ 1 wk)
4. **Stage 2** — completions/hover polish.
5. **Stage 7** — magics.
6. **Stage 5 remaining renderers** (Dag/SVG via Graphviz, CLP, Logic).
7. **Stage 8** — interrupts and actor routing.
8. **Stage 6** — labextension widgets.
9. **Stage 11 remaining notebooks** (02, 05, 06, 08, 09, 10).
10. **Stage 10 conda-forge + PyPI** — full distribution surface.
11. **Stage 12** — user docs close out the work.

**Time-to-demo (Stages 0/1/3/4/5-partial/9/10-partial/11-partial):
~3.5 weeks.**

---

## Cross-cutting Concerns

### Thread safety

- ZMQ poll thread (xeus-owned) → enqueues requests.
- Eval worker thread → owns `driver_mu_` while a cell runs.
- IOPub publishes from xeus are internally serialised.
- `Driver::request_interrupt()` is the only API safe to call from
  the poll thread while the worker is running; everything else
  takes `driver_mu_`.

### Memory

- Shared `Driver` means kernel restart is the only hard reset.
  Document this clearly in `docs/jupyter.md`.
- Heap snapshots are O(heap size); rate-limit `(jupyter:heap)`
  pushes (debounce 250 ms) when the actor widget is auto-refreshing.

### Security

Notebooks execute arbitrary code — same trust model as the Python
kernel. We do **not** add per-cell sandboxing in v1. The Stage 0
sandbox planned for the DAP (`docs/dap_vs_plan.md` §"A0") is
**reused for read-only inspection only**, not for cell isolation.

### Versioning

- Jupyter `protocol_version` pinned to `5.4`.
- xeus pin: `>=5,<6` (semver allows minor upgrades).
- Custom MIME types are namespaced under `application/vnd.eta.*` and
  versioned by an explicit `version` field in the JSON payload.

### Performance

- Rich-display detection is **O(1) tag inspection on the boxed
  value** — never traverses large structures.
- Tensor PNG render is gated on `≤ 512×512`; larger tensors emit
  shape summary only.
- FactTable HTML render caps at 1000 rows; user must `(jupyter:vega
  …)` or pivot to plot for larger.

### What we are NOT doing

- IPython-style `?` / `??` introspection. Folded into the existing
  hover provider.
- `ipywidgets` compatibility. Separate effort if there's demand.
- Per-cell sandboxing / fresh-`Driver` mode. v2.
- Polyglot kernels (Python + Eta in the same notebook).
- Kernel-side notebook conversion (use `nbconvert`).

---

## Definition of Done (whole plan)

1. `pip install xeus-eta` and `mamba install -c conda-forge xeus-eta`
   both produce a working kernel listed by `jupyter kernelspec list`.
2. The Binder badge in the README boots `00_index.ipynb` and the
   user can run `04_european_option.ipynb` end-to-end within 90 s
   of badge click.
3. All 11 notebooks under `examples/notebooks/` execute cleanly via
   `nbconvert --execute` in CI on `ubuntu-latest`.
4. The MIME catalogue (Stage 5) is documented in `docs/jupyter.md`
   with a JSON-schema sketch per type.
5. `docs/jupyter.md` exists, links from the README hero and from
   `next-steps.md`.
6. The Stage 11 headline demo (cell 1–6 of `04_european_option`)
   runs in < 60 s on a reference laptop.
7. Interrupt latency for a runaway cell is < 100 ms.
8. The labextension under `editors/jupyterlab/` builds reproducibly
   in CI and publishes to npm under `@eta-lang/jupyterlab` on tag
   push.

---

## Source Locations Referenced

| Component | File |
|---|---|
| Kernel main | `eta/jupyter/src/eta/jupyter/main_jupyter.cpp` |
| `EtaInterpreter` | `eta/jupyter/src/eta/jupyter/eta_interpreter.{h,cpp}` |
| `DisplayValue` + renderers | `eta/jupyter/src/eta/jupyter/display.{h,cpp}` |
| Magics | `eta/jupyter/src/eta/jupyter/magics.{h,cpp}` |
| Comm handlers | `eta/jupyter/src/eta/jupyter/comm/*.{h,cpp}` |
| Kernelspec template | `eta/jupyter/resources/kernel.json.in` |
| Kernel CMake | `eta/jupyter/CMakeLists.txt` |
| Fetch script | `cmake/FetchXeus.cmake` |
| Driver facade | `eta/session/src/eta/session/driver.h` |
| Repl-shared completion logic | `eta/interpreter/src/eta/interpreter/repl_complete.h` *(new, shared with LSP)* |
| LSP completion + hover handlers | `eta/lsp/src/eta/lsp/lsp_server.cpp` |
| VM hot loop | `eta/core/src/eta/runtime/vm/vm.cpp` |
| `std.io` print sink | `stdlib/std/io.eta` |
| Jupyter prelude helpers | `stdlib/std/jupyter.eta` *(new)* |
| DAP webviews (rendering reference) | `editors/vscode/src/heapView.ts`, `disassemblyView.ts`, `gcRootsTreeView.ts`, `childProcessTreeView.ts` |
| JupyterLab labextension | `editors/jupyterlab/` *(new)* |
| Demo notebooks | `examples/notebooks/0[0-9]_*.ipynb`, `10_*.ipynb` |
| User-facing docs | `docs/jupyter.md` *(new, Stage 12)* |
| Binder | `binder/{apt.txt,environment.yml,postBuild}` |
| Docker | `docker/jupyter/Dockerfile` |
| Conda recipe | `recipes/xeus-eta/meta.yaml` |



