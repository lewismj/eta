// @ts-check
import { defineConfig } from "astro/config";

// Set BASE_PATH env var when building for GitHub Pages project URLs
// e.g.  BASE_PATH=/eta  npm run build
const base = process.env.BASE_PATH ?? "";

export default defineConfig({
  base,
  output: "static",
  site: "https://eta-lang.github.io",
});

