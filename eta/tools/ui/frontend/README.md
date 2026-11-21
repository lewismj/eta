# std.ui front end

The browser side of Eta's visual environment. The Eta process serves this
bundle itself (see `packages/stdlib/native/ui`), so nothing here runs at run
time except the built files.

- **Stack:** SolidJS + TypeScript, built with Vite. Charts use three.js
  `WebGPURenderer` with TSL node materials (WebGPU, falling back to WebGL2).
- **Output:** `packages/stdlib/native/ui/web/` (git-ignored), which ships in
  the eta-ui sidecar package.
- **Design tokens:** `src/theme.css`. Chart colours are the validated
  reference palette; the GPU engine reads them from CSS.

## Building

The normal CMake build does this for you (`eta_ui_frontend` target, needs
Node.js + npm; turn off with `-DETA_BUILD_UI_FRONTEND=OFF`). By hand:

```sh
cd eta/tools/ui/frontend
npm ci
npm run build        # one-off build
npm run dev          # rebuild on every change
npm run typecheck
```

## Development loop

Point a running program at the bundle so front-end edits need no `etai`
rebuild:

```sh
# PowerShell
$env:ETA_UI_DEV_DIR = "$PWD\..\..\..\..\packages\stdlib\native\ui\web"
```

Run `npm run dev` in one terminal and your Eta program in another; reload the
page after a rebuild.

## Layout

| Path | Purpose |
|------|---------|
| `src/protocol.ts` | WebSocket framing, binary Float64 buffers, reconnect |
| `src/main.tsx` | App shell, store (keyed reconcile by node id), toasts |
| `src/nodes.tsx` | Element registry: row, column, panel, stat, kv, button, ... |
| `src/chart/Plot.tsx` | Plot frame: canvas, SVG grid, HTML ticks, tooltip, picking |
| `src/chart/engine.ts` | GPU engine: instanced marks, keyed transitions on the GPU |
| `src/chart/scale.ts` | Domains, nice ticks, easing |
