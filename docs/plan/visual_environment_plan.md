# Eta Visual Environment Plan

[Back to plans index](./)

Give Eta a declarative way to build interactive analytical applications, with GPU-rendered
charts that look deliberate, not like default widgets. VS Code stays the editor, and during
development the running application appears in a VS Code webview panel next to the code.

The app must **not depend on VS Code**. The same compiled program (`.etac`) must also run on its
own: `etai` opens the app in a standalone window or the system browser, with no editor
involved. A Jupyter front end follows later and uses the same protocol.

The quant demos (portfolio optimisation first, then XVA) set the requirements and serve as the
acceptance test. The final demo is an end-to-end run from compiled code.

---

## 1. Decisions

| Decision | Choice | Reason |
|----------|--------|--------|
| Editor | VS Code (existing extension) | Familiar to users, and the debugger, LSP and webviews already exist |
| Who serves the front end | **The Eta process itself**: local HTTP for the bundle plus a WebSocket for the protocol | Every host becomes a thin client, and VS Code is optional |
| Hosts | System browser, VS Code webview panel, standalone window, then Jupyter | The browser engine hosts the app but does not limit how charts are drawn |
| Standalone window | The `webview/webview` C/C++ library (WebView2 / WKWebView / WebKitGTK) | A small dependency, and it reuses the same bundle. No Electron. |
| Where the UI runtime lives | Native stdlib module `packages/stdlib/native/ui`, exposed as `std.ui` | Ships with every `etai`, like `nng`. Works the same from the REPL, `.etac` and notebooks. Keeps presentation out of the driver. |
| Application chrome | Ordinary web UI (HTML/CSS, a dock/layout library) | Nobody is impressed by a well-made dropdown. Don't write a widget toolkit. |
| Front-end framework | SolidJS + TypeScript | It updates only the values that change, which matches applying patches to individual properties. It is small, and TanStack supports it. |
| Data grid | TanStack Table + TanStack Virtual | Draws nothing itself, so tables share the charts' look instead of looking like a third-party product |
| Analytical grid | FINOS Perspective, as an optional specialist component later | Only if pivoting or streaming aggregation justifies the dependency |
| Visual engine | Custom, built on **three.js `WebGPURenderer`** | One shader codebase (TSL) with automatic WebGPU to WebGL2 fallback, plus bloom, PBR and post-processing |
| 2D text | HTML/CSS overlays positioned over the canvas | Browser typography beats anything built on the GPU, and the text can be selected and copied |
| 3D text | MSDF text (e.g. troika-three-text) | Only for labels that live inside 3D scenes |
| Bulk data | Binary typed-array buffers, separate from the JSON tree | Surfaces, Monte Carlo distributions and long series must not go through JSON |
| App lifecycle | `ui:run` blocks; `ui:start` returns a handle | Explicit behaviour. Never detect notebook or standalone automatically (section 3). |
| Layout persistence | Workspace state persisted, view state opt-in, application state never | Store through the UI server, keyed by app ID (section 3) |
| Front-end distribution | Embedded in the standard `etai` release | No Node or npm at run time, and the front end can't drift out of step with the runtime version |
| Demo packaging | `etai` + `.etac` + launcher script | A single executable and an installer are future options |
| Native renderer (non-web) | Out of scope | It can be added later as another renderer behind the same protocol |

**Not doing:**
- ImPlot or ImGui as the visual foundation. They suit engineering diagnostics, not this goal.
- A general widget toolkit.
- Hand-maintained WebGPU and WebGL2 backends. If three.js is ever dropped, choose WebGL2 alone
  first.
- Calling the environment "native". It runs in a webview. Use "integrated" or "Eta's visual
  computing environment".

---

## 2. Architecture

```
 Eta program
   │  eta.ui  (declarative tree, reactive state, events, keyed data)
   │  eta.chart (marks, scales, axes, heatmap, surface)
   ▼
 UI runtime actor (in the Eta process)
   │  diffs the tree, owns state, dispatches events to Eta handlers
   ▼
 UI server (in the Eta process, localhost only)
   │  HTTP: serves the front-end bundle      WebSocket: Eta UI protocol
   │                                          (JSON messages + binary frames)
   ├──────────────┬──────────────────┬──────────────────┐
   ▼              ▼                  ▼                  ▼
 System browser  VS Code webview    Standalone window   Jupyter comm "eta.ui"
                 (dev loop)         (webview lib,        (later; same protocol,
                                     demo / shipped)      different transport)
   │
   ▼
 Front end (one JS/TS bundle for every host)
   ├─ Chrome: layout, panels, tables, forms (web UI)
   └─ Visual engine: scene of marks → three.js → WebGPU / WebGL2
                     + HTML text overlay
```

### Process and transport

- **Responsibilities:**

  | Component | Responsibility |
  |-----------|----------------|
  | `std.ui` (Eta) | Public API, UI tree, state, event registration, diffing |
  | Native UI module (C++) | UI actor plumbing, HTTP/WebSocket server, buffer registry, workspace storage |
  | Driver | Process lifecycle only: `--ui=` flag, orderly shutdown, closing the server on exit |
  | Front end | Rendering, layout, interaction, animation |
  | VS Code extension | A thin host adapter: launch or attach, discover the URL, map the port, manage the panel. It contains no protocol code. |

- **The Eta process serves its own UI.** When a program first calls `ui:start` (or `ui:run`),
  the runtime starts a localhost-only server on an ephemeral port. It serves the front-end bundle over HTTP and
  carries the protocol over a WebSocket, with JSON in text frames and buffers in binary frames.
  A random session token in the URL prevents other local processes from connecting.
- **Where the server comes from: NNG, which the core runtime already uses.** NNG v1.11
  (`cmake/FetchNng.cmake`) carries actor messages between nodes
  (`eta/core/src/eta/runtime/actor/node_transport.cpp`, `pair0` sockets) and also backs
  `std.net`. So it ships with every `etai`, and the UI server adds no new dependency.
  - Do **not** use NNG's `pair0` sockets over `ws://` for the UI. They speak NNG's own message
    protocol, which a browser `WebSocket` can't talk to.
  - Use NNG's lower-level stream API instead: `nng_stream_listener` on a `ws://` URL gives
    plain WebSocket connections. Pair it with `nng_http_server` and a static or directory
    handler for the bundle and the workspace endpoints. Both are in NNG's default build, and
    the WebSocket and HTTP handlers can share one port.
  - Spike A confirms this, including text versus binary frame control. Only if it fails, fall
    back to a small embedded server (e.g. civetweb or IXWebSocket).
- **Bundle:** release builds embed the front-end bundle in the native UI module, so a compiled
  program needs no Node, no npm and no VS Code at run time. In development, setting
  `ETA_UI_DEV_DIR` makes the server read the bundle from disk instead (e.g. Vite's build
  output), so front-end changes don't need an `etai` rebuild.
- **How each host attaches:**
  - *Browser:* `etai app.etac --ui=browser` prints the URL and opens it.
  - *Standalone window:* `etai app.etac --ui=window` opens a native window with the
    `webview/webview` library pointed at the same URL. This is the demo mode. Each platform
    deploys its web engine differently: WebView2 is built into Windows 11, but on Linux
    WebKitGTK has weak WebGPU support, so it runs on the WebGL2 fallback. If the web engine is
    missing, fall back to `--ui=browser` with a message.
  - *VS Code:* the extension opens a webview panel that loads the same URL, using the webview
    `portMapping` option so it also works with remote and devcontainer setups. The extension
    no longer relays messages. It only discovers the URL (from the process output or the DAP
    launch) and opens the panel. The existing DAP custom requests (`eta/heapSnapshot` and
    others) stay as they are for debugging and are separate from the UI.
  - *Jupyter:* a new comm target `eta.ui` next to the existing ones
    (`eta/tools/jupyter/src/eta/jupyter/comm/`). Comm messages carry binary buffers natively,
    as `tensor_comm` already does. The front end hides its transport behind a small interface
    (WebSocket or comm), so the rest of the bundle is identical.
- `--ui=none` (headless) keeps the server running without opening anything. Tests and remote
  use need this.

### Threading

- The UI runtime is an **actor** (`std.actor`). Eta code sends it tree and state updates. It
  calls event handlers back in the program's context, through messages, never re-entrantly.
- The front end animates on its own clock. Eta only sends target states, and the engine
  interpolates between them. So a slow model run never makes the UI stutter.

---

## 3. Eta API (first sketch)

Illustrative only. Phase 1 settles the real names.

```scheme
(import std.ui std.chart)

(define state
  (ui:state 'assumptions (default-assumptions)
            'selected #f))

(defun view (s)
  (ui:window "Portfolio Analytics"
    (ui:row
      (ui:panel 'width 280
        (ui:table (holdings (ui:get s 'selected))
                  'key 'asset-id))
      (ui:panel 'flex 1
        (ui:column
          (chart:plot
            'title "Efficient frontier"
            'x (chart:axis "Risk (σ)" 'format "pct")
            'y (chart:axis "Return" 'format "pct")
            (chart:area  (frontier-curve s) 'fill (chart:gradient 'vertical ...))
            (chart:line  (frontier-curve s) 'glow 0.3)
            (chart:points (candidate-portfolios s)
                          'key 'portfolio-id            ; required for transitions
                          'on-hover  'show-metrics
                          'on-select (lambda (p) (ui:set! s 'selected p))))
          (chart:heatmap (correlation-matrix s)
                         'scale (chart:diverging -1 0 1)
                         'link 'selected))))))

(ui:run view state)
```

Principles:

- **Keyed data.** Every collection a mark renders has a `'key`. Keys decide what counts as
  "the same point" across updates. Matched keys animate to their new positions, new keys fade
  in, and missing keys fade out. Without keys, transitions can't work. This is the most
  important part of the Eta-side design.
- **View as a function of state.** `ui:run` calls `view` again when state changes, diffs the
  result against the last tree, and sends only the differences.
- **Handlers are Eta closures.** Events come back as messages, and the runtime invokes the
  matching closure. Handlers that only touch the front end (hover tooltips, zoom, pan) run
  entirely on the front end and never go to Eta.
- **Bulk data by reference.** Large numeric arrays (tensors, f64 vectors) go into the buffer
  part of the protocol automatically. The tree only refers to a buffer ID.
- **Theme is data.** Colours, type scale and spacing come from a theme value, not from
  individual marks.

Modules start as two, `std.ui` and `std.chart`. `graphics`, `plot3d`, `widgets` and `theme`
get split out later, once the boundaries are clear. From day one the engine must not know
anything about portfolios or XVA.

### Lifecycle: `ui:run` and `ui:start`

There are two explicit entry points. The runtime never tries to detect whether it is running in
a notebook or standalone.

```scheme
;; Standalone application: blocks until the window or browser tab closes.
(ui:run view state)

;; REPL / notebook / embedded: returns a handle straight away.
(define app (ui:start view state))
(ui:update! app new-state)   ; or ui:set! on the state value
(ui:wait app)                ; optional: block until closed
(ui:close app)
```

- `ui:run` is `ui:start` followed by `ui:wait`.
- The UI server starts lazily, on the first `ui:start`. One server per process can host several
  apps, each on its own path.
- Blocking the caller never blocks the UI actor. The actor owns state and handles events one at
  a time. Callbacks run through the actor, never re-entrantly, and the front end has its own
  render loop.
- Notebook use: `(jupyter:app (ui:start view state))`. The kernel stays free for later cells.

### State categories

| State | Examples | Owner | Persisted |
|-------|----------|-------|-----------|
| Application | Selected portfolio, assumptions, scenario | Eta (`ui:state`) | No, rebuilt by the program on each run |
| Workspace | Dock positions, panel sizes, open panels, table column configuration | Front end | Yes, by default |
| View | Zoom, pan, brushed region | Front end | Only if the program opts in |

Persistence needs a stable app ID and a versioned layout schema, e.g.
`(ui:window "Portfolio Analytics" 'id "eta.portfolio" 'layout 'persistent ...)`. If the schema
version changes, the saved layout is discarded, not migrated.

**Storage:** the server picks a random port each run, and browser storage is tied to the port,
so `localStorage` would lose the layout every run. Instead the front end saves the workspace
through the UI server (`GET`/`PUT /workspace/<app-id>`). The server writes JSON under the user
config directory (`%APPDATA%\eta\ui\` or `$XDG_CONFIG_HOME/eta/ui/`). Browser, standalone
window and VS Code all use this one mechanism. Jupyter stores it in notebook metadata instead,
behind the same front-end storage interface. The protocol itself never deals with storage.

---

## 4. Protocol

Versioned, with JSON control messages plus optional binary attachments.

| Direction | Message | Contents |
|-----------|---------|----------|
| Eta → UI | `hello` | protocol version, theme, capabilities |
| Eta → UI | `tree` | full tree (first render or resync) |
| Eta → UI | `patch` | list of tree diffs: insert, remove, replace props, move (by key) |
| Eta → UI | `buffer` | `{id, dtype, shape}` + bytes; replaces or appends (streaming series) |
| Eta → UI | `release` | buffer IDs that are no longer referenced |
| UI → Eta | `event` | `{node-id, kind, payload}` (select, click, form change…) |
| UI → Eta | `ready` / `resize` / `error` | lifecycle |

- Node IDs are stable paths built from component keys, so patches survive reruns.
- `dtype`: `f64`, `f32`, `i32`, `u8`. `shape` is row-major. Columnar tables are one buffer per
  column.
- A protocol test suite (golden JSON plus buffers) runs in C++ and TS, so both ends stay in
  agreement.

---

## 5. Visual engine

### Required quality

| Capability | Target |
|------------|--------|
| Lines | Anti-aliased in screen space, variable width, optional glow, dashes |
| Fills | Multi-stop, directional and radial gradients; layered compositing with opacity |
| Points | Instanced, hover and selection states, 10^5 points at 60 fps |
| Animation | Eased interpolation of positions, colours and scales; enter, update and exit by key |
| Interaction | Smooth eased zoom and pan, linked axes, hover inspection, brush selection |
| Heatmap | Perceptual colour scales (sequential and diverging), cell hover, row and column linking, smooth value transitions |
| 3D surface | GPU mesh from a grid buffer, lighting, colour by value, contour lines, orbit camera with inertia |
| Post | Bloom used sparingly (selection highlight, never everywhere); MSAA |
| Text | HTML overlay for 2D labels, ticks and tooltips; MSDF inside 3D |
| Export | PNG (canvas capture) in v1; SVG for 2D later |

### Structure

```
front end/
  protocol/      message decode, buffer registry, patch application
  chrome/        layout, panels, tables, forms (web UI)
  engine/
    scene/       marks → three.js objects, keyed enter/update/exit
    scales/      linear, log, time, band; tick generation and formatting
    marks/       line, area, points, rect, heatmap, surface
    anim/        tweening, easing, interruption
    interact/    camera, zoom/pan, picking, brushing, linking
    overlay/     HTML text layer synchronised with the camera
    theme/       design tokens → materials and CSS variables
```

### Design system first

Before writing shaders, decide on one reference look and capture it as tokens: background
layers, a categorical palette, sequential and diverging scales, a type scale (one UI typeface
plus a tabular-figure typeface for numbers), stroke weights and motion durations. Most of the
perceived quality comes from these choices, not from the renderer. Avoid the "neon gaming
dashboard" look.

---

## 6. Phases

### Phase 0: Design tokens and reference mock-ups

- One static mock-up of the portfolio screen (frontier, heatmap, composition panel) that sets
  the target look.
- A theme token file, shared by the CSS chrome and the engine.
- **Exit:** the mock-up is agreed as the visual bar.

### Phase 1: End-to-end slice (the most important phase)

Ugly is acceptable here, as long as the full loop works.

Start with two short spikes, because the design depends on them:

- **Spike A:** using the NNG already linked into the runtime, serve a static page with
  `nng_http_server` and connect a browser `WebSocket` to an `nng_stream_listener` on the same
  port. Check text and binary frames in both directions, and large (multi-MB) binary messages.
  If this fails, choose civetweb or IXWebSocket.
- **Spike B:** can a VS Code webview reach that WebSocket through `portMapping`, both locally
  and in a remote or devcontainer session?

Then:

- `std.ui` minimum: `ui:window`, `ui:row`, `ui:column`, `ui:panel`, `ui:state`, `ui:start`,
  `ui:run`, `ui:close`.
- `std.chart` minimum: `chart:plot`, `chart:points` with `'key`.
- The UI runtime actor, tree diffing and the protocol (`tree`, `patch`, `buffer`, `event`).
- The native module `packages/stdlib/native/ui` with the UI server, plus `--ui=browser`.
  The system browser is the first host because it needs no extension work.
- The front-end scaffold (SolidJS, TypeScript, Vite) with the `ETA_UI_DEV_DIR` dev loop.
- Front end: apply patches, render instanced points, show a hover tooltip, send a select event
  back to Eta.
- A VS Code command, "Eta: Open App", that opens a webview panel on the running app's URL.
- **Exit:** edit and rerun the program, and the points **animate** to their new positions.
  Clicking a point updates Eta state, and the side panel changes. This works in both the
  browser and the VS Code panel.

### Phase 2: Hero 2D chart

- Line and area marks with gradients and glow, axes and scales, HTML overlay text, smooth
  zoom/pan, brushing, annotations.
- Streaming append for time series (`buffer` append mode).
- **Exit:** the efficient frontier meets the Phase 0 mock-up.

### Phase 3: Heatmap

- Heatmap mark, colour scales with a legend, cell hover, row and column selection linked to
  other charts (`'link`), smooth value transitions.
- **Exit:** the correlation heatmap is linked to the frontier selection in the portfolio demo.

### Phase 4: 3D surface

- Surface mark from a grid buffer, lighting, colour by value, contours, orbit camera, hover
  readout of the grid value.
- **Exit:** an XVA exposure surface (time × scenario quantile, or time × counterparty) from the
  `cookbook/xva-wwr` model renders and responds to assumption changes.

### Phase 5: Application chrome and demo

- A TanStack Table and Virtual grid (sorting, filtering, column resizing, virtualisation),
  styled with the theme tokens and bound to keyed columnar data. Also forms for assumptions,
  docking or split layout, and a property inspector.
- Workspace persistence through the UI server (`/workspace/<app-id>`), and opt-in view-state
  persistence.
- Portfolio demo complete: composition, frontier, heatmap, factor exposures, stress scenarios.
- **Exit:** a scripted demo runs from a clean checkout.

### Phase 6: Standalone demo from compiled code

- `--ui=window` using the `webview/webview` library (WebView2 on Windows, WKWebView on macOS,
  WebKitGTK on Linux), with the window title and size taken from `ui:window`.
- The front-end bundle is embedded in the release build of the native UI module, so no Node,
  npm or VS Code is needed at run time. The CMake build runs the front-end build (Vite) and
  embeds its output.
- If the platform's web engine is missing, fall back to `--ui=browser`.
- A demo package: `etac` compiles the portfolio app to `.etac`, plus a launcher script
  (`portfolio-demo.cmd` / `.sh`) that runs `etai portfolio.etac --ui=window`.
- **Exit:** on a clean machine with only the Eta release installed, the launcher opens the
  portfolio app in its own window. Everything from Phase 5 works, with no editor running.

### Phase 7: Jupyter renderer

- The `eta.ui` comm target and a JupyterLab/Notebook front-end extension that reuses the same
  bundle.
- `std.jupyter` gains `jupyter:app`, which displays an app inline:
  `(jupyter:app (ui:start view state))`. Workspace state is stored in notebook metadata.
- **Exit:** the portfolio demo runs in `cookbook/notebooks/`.

Phases 2–4 can overlap once Phase 1 is done. A basic `--ui=window` (Phase 6) can be built
early, right after Phase 1, so every later milestone can also be checked standalone. Phase 7
can start any time after Phase 1.

---

## 7. Testing

- **Protocol:** golden message tests on both the C++ and TS sides, and a diff/patch round-trip
  property test (applying `diff(a, b)` to `a` must give `b`).
- **Eta API:** stdlib tests for tree construction, keys and state updates, with no front end
  needed.
- **Visual regression:** a headless browser renders fixed scenes at fixed animation times, and
  the screenshots are compared within a tolerance. Run in CI on the WebGL2 path, which is
  deterministic enough for tolerance diffs.
- **Performance:** 10^5 points, a 100×100 heatmap and a 200×200 surface at 60 fps on reference
  hardware. Also track the time from a state change in Eta to the first animated frame.

---

## 7a. Implementation notes (Phase 1)

What was built, and where it differs from the sections above:

- **Framing.** NNG's WebSocket stream mode sends only binary frames and does not keep frame
  boundaries on receive, so the protocol has its own length-prefixed envelope:
  `[u32 total][u32 json_len][json][f64 buffers]` from the server and `[u32 len][json]` from the
  page. The JSON header lists the buffers, and `{"$buf": i}` refers to one. Buffers start
  8-byte aligned, so the page views them as `Float64Array` without copying.
- **Automatic buffers.** The native side serialises Eta values itself. Any vector of 16 or
  more numbers becomes a binary buffer, so `std.chart` columns travel as binary with no extra
  API. Large trees never become interned strings.
- **Full tree, keyed on the page.** Phase 1 sends the whole tree on each change, and the page
  reconciles it by node id (Solid `reconcile`). Data is already in buffers, so the tree itself
  is small. Patches (§4) are an optimisation for later, if profiling shows a need.
- **Stable port and token per app id**, stored in the UI config directory. A page left open
  reconnects to the next run. If the previous run still holds the port, the new process sends
  it `{"type":"quit"}` and takes over. This is how "rerun and the points animate" works.
- **Discovery.** `<config>/eta/ui/<app-id>.url` holds the current URL. *Eta: Open App* reads it
  and shows the page in an iframe through `env.asExternalUri`, so no `portMapping` is needed.
- **Bundle location.** Phase 1 serves `packages/stdlib/native/ui/web/`, which is built by the
  `eta_ui_frontend` CMake target and installed with the sidecar package, not embedded in the
  binary. It is still part of every release and still versioned with the runtime. Embedding
  remains an option.
- **Lifecycle.** `ui:run` returns when the last page has been closed for 3 s (a reload is not a
  close), or when a newer run takes over. `ui:start` is still to do (Phase 7 needs it).
- **Files:** `packages/stdlib/native/ui/src/eta/ui/{ui_server,ui_primitives}.h`,
  `stdlib/std/{ui,chart}.eta`, `stdlib/tests/ui.test.eta`, `eta/tools/ui/frontend/`,
  `editors/vscode/src/appPanel.ts`, `cookbook/ui/frontier.eta`.

## 7b. Implementation notes (Phases 2–6)

- **Marks.** Lines are instanced capsules with a distance-field edge (round joins, crisp at any
  width). Areas are triangle lists with a vertical gradient or a flat band. Bars are
  rounded-box distance fields with a square baseline end. All of them clip to the plot rectangle
  in the fragment shader rather than with the scissor test, because the scissor's y origin
  differs between the WebGPU and WebGL backends in three r176.
- **Two clocks.** Data transitions (`uT`) and domain changes (`uDomT`) animate separately, so
  zooming and panning never replay a data animation.
- **Transitions.** Points and bars are keyed. Lines and areas morph vertex by vertex when the
  point count is unchanged, and cross-fade otherwise. The planning functions are pure TypeScript
  and unit-tested (`geometry.ts`, `transition.ts`).
- **Heatmap as DOM.** The heatmap is a CSS grid rather than a GPU mark. Matrices here are small,
  and the DOM gives crisp labels, accessible cells, real hit targets, CSS colour transitions,
  and testability in jsdom.
- **Colour.** Sequential and diverging ramps are interpolated in OKLab from the validated palette.
  The dark sequential ramp starts at step 600 so low values stay at least 2:1 against the
  surface. Each diverging arm passes the palette validator; the grey midpoint is the palette's
  prescribed neutral.
- **Inputs and table.** Slider, select and toggle send `{value}`; `std.ui` unwraps it so handlers
  receive the value directly. The table uses TanStack Table v8, sorts on the page, virtualises
  above 200 rows, and maps selection back to the original row.
- **3D surface (Phase 4/5).** Grids are small, so the surface animates on the CPU: heights and
  colours interpolate and normals are recomputed each frame, which keeps raycast hover exact.
  It is upsampled with Catmull-Rom splines, using linearly extrapolated ghost points so the
  edges do not bend. Axis labels are HTML projected from 3D.
- **Standalone window (Phase 6).** `etai app.eta --ui window` (or `ETA_UI=window`) opens a
  webview/webview window (header-only; WebView2 on Windows). It is compiled only into the eta-ui
  sidecar and registered through `window_hooks()`, so the other sidecars do not depend on it.
  Closing the window ends `ui:run`. The CMake option `ETA_UI_WINDOW` is on for Windows and
  macOS. Without it, `--ui window` falls back to the browser.
- **The WWR demo** (`cookbook/ui/wwr/`) uses an analytic Hull–White one-factor Gaussian-copula
  model (`wwr-model.eta`), so every assumption change re-prices the 30-counterparty book
  interactively. Results are cached per assumption set, so selecting a counterparty re-renders
  without re-pricing.
- **Tests.**
  - Front end: Vitest suites, including jsdom component tests.
  - Eta: `std.ui` and `std.chart` tests, plus the WWR model tests.
  - C++: serializer and server tests, and end-to-end app tests that launch the real cookbook
    apps with `etai` and drive them over the WebSocket.

## 8. Decision log

| Question | Decision | Where |
|----------|----------|-------|
| Deploy outside VS Code? | Yes. The Eta process serves its own front end, and VS Code is one host among several. | §2, Phase 6 |
| Where does the UI server live? | Native stdlib module `packages/stdlib/native/ui` (`std.ui`). The driver only manages lifecycle. The server starts lazily. | §1, §2 |
| Does `ui:run` block? | Yes. `ui:start` returns a handle (`ui:update!`, `ui:wait`, `ui:close`). The environment is never detected automatically. | §3 |
| Concurrency | Actor-based. Callbacks run through the actor, and the front end has its own render loop. | §2, §3 |
| Data grid | TanStack Table + Virtual. Perspective may come later as a specialist component. | §1 |
| Front-end framework | SolidJS + TypeScript, built with Vite | §1 |
| Layout persistence | Workspace persisted by default, view state opt-in, application state never. Stored through the UI server, not `localStorage`. | §3 |
| Front-end distribution | Embedded in the standard release. `ETA_UI_DEV_DIR` for development. | §2 |
| Demo packaging | `etai` + `.etac` + launcher. A single executable and an installer are future options. | Phase 6 |

## 9. Open questions

1. Whether NNG's stream API (already in the runtime) gives enough control over WebSocket frames
   and message sizes. Spike A answers this. The expected outcome is that NNG is enough, and a
   separate server is only a fallback.
2. Whether a WebSocket through webview `portMapping` is reliable in remote and devcontainer
   VS Code sessions. This depends on Spike B. If it is not, the extension falls back to a thin
   byte relay, which still contains no protocol code.
3. Which docking/layout library (e.g. dockview, or a small custom split-pane), chosen in
   Phase 5.
4. Whether to package further later: a single executable with the `.etac` and bundle embedded,
   or an installer.
