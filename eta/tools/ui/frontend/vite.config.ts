import { defineConfig } from 'vite';
import solid from 'vite-plugin-solid';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';

const here = fileURLToPath(new URL('.', import.meta.url));

// The bundle is written into the eta-ui sidecar package so it ships with the
// runtime (packages/stdlib/native/ui/web). Override with ETA_UI_OUT_DIR.
const outDir = process.env.ETA_UI_OUT_DIR
  ?? resolve(here, '../../../../packages/stdlib/native/ui/web');

export default defineConfig({
  plugins: [solid()],
  base: './',
  build: {
    outDir,
    emptyOutDir: true,
    target: 'es2022',
    sourcemap: true,
    chunkSizeWarningLimit: 2000,
  },
  // Unit tests cover pure logic (framing, scales, transitions): no DOM needed.
  test: {
    environment: 'node',
  },
} as import('vite').UserConfig);
