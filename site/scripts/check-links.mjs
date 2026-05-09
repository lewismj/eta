// Link checker for the built Astro site.
// Walks dist/ for *.html, collects all internal hrefs, and reports any
// that don't resolve to a built file or in-page anchor.
//
// Usage:  node scripts/check-links.mjs
import { readdirSync, readFileSync, statSync, existsSync } from "node:fs";
import { join, resolve, dirname, posix } from "node:path";

const ROOT = resolve(new URL("..", import.meta.url).pathname.replace(/^\/(\w:)/, "$1"));
const DIST = join(ROOT, "dist");
const BASE = process.env.BASE_PATH ?? ""; // matches astro.config.mjs

if (!existsSync(DIST)) {
  console.error(`dist/ not found at ${DIST}. Run \`npm run build\` first.`);
  process.exit(2);
}

/** Recursively list every file under DIST. */
function walkAll(dir, out = []) {
  for (const entry of readdirSync(dir)) {
    const p = join(dir, entry);
    const s = statSync(p);
    if (s.isDirectory()) walkAll(p, out);
    else out.push(p);
  }
  return out;
}

const allFiles = walkAll(DIST);
const htmlFiles = allFiles.filter((f) => f.endsWith(".html"));

/** Map URL pathname (with trailing slash for index.html) → set of in-page ids. */
const pageIds = new Map();
const validUrls = new Set();

const idRe = /\bid=["']([^"']+)["']/g;
const hrefRe = /\bhref=["']([^"']+)["']/g;

function urlForFile(file) {
  let rel = file.slice(DIST.length).replace(/\\/g, "/");
  if (rel.endsWith("/index.html")) rel = rel.slice(0, -"index.html".length);
  // Astro builds with trailing slash dirs; normalise.
  if (!rel.startsWith("/")) rel = "/" + rel;
  return rel;
}

for (const file of htmlFiles) {
  const url = urlForFile(file);
  validUrls.add(url);
  // Also accept the no-trailing-slash form for /foo/ URLs.
  if (url.endsWith("/") && url !== "/") validUrls.add(url.slice(0, -1));

  const html = readFileSync(file, "utf8");
  const ids = new Set();
  let m;
  while ((m = idRe.exec(html))) ids.add(m[1]);
  pageIds.set(url, ids);
}

// Add every other static asset (favicon, /_astro/*.css|js, images, ...).
for (const file of allFiles) {
  if (file.endsWith(".html")) continue;
  let rel = file.slice(DIST.length).replace(/\\/g, "/");
  if (!rel.startsWith("/")) rel = "/" + rel;
  validUrls.add(rel);
}

/** Resolve an href against the page URL, mirroring browser semantics. */
function resolveHref(pageUrl, href) {
  if (/^[a-z]+:/i.test(href) || href.startsWith("//") || href.startsWith("mailto:")) {
    return null; // external / non-http
  }
  if (href.startsWith("#")) {
    return { url: pageUrl, hash: href.slice(1) };
  }
  const hashIdx = href.indexOf("#");
  const hash = hashIdx >= 0 ? href.slice(hashIdx + 1) : "";
  let path = hashIdx >= 0 ? href.slice(0, hashIdx) : href;
  if (!path) return { url: pageUrl, hash };

  // Strip the configured BASE prefix (so "/eta/docs/foo/" matches our DIST tree).
  if (BASE && path.startsWith(BASE + "/")) path = path.slice(BASE.length);
  else if (BASE && path === BASE) path = "/";

  let abs;
  if (path.startsWith("/")) abs = path;
  else {
    const dir = pageUrl.endsWith("/") ? pageUrl : posix.dirname(pageUrl) + "/";
    abs = posix.normalize(dir + path);
  }
  return { url: abs, hash };
}

const broken = [];
const anchorBroken = [];

for (const file of htmlFiles) {
  const pageUrl = urlForFile(file);
  const html = readFileSync(file, "utf8");
  let m;
  while ((m = hrefRe.exec(html))) {
    const raw = m[1];
    const resolved = resolveHref(pageUrl, raw);
    if (!resolved) continue;
    const { url, hash } = resolved;
    if (!validUrls.has(url)) {
      broken.push({ from: pageUrl, href: raw, resolved: url });
      continue;
    }
    if (hash && !pageIds.get(url)?.has(decodeURIComponent(hash))) {
      anchorBroken.push({ from: pageUrl, href: raw, resolved: url + "#" + hash });
    }
  }
}

console.log(`Scanned ${htmlFiles.length} pages, ${validUrls.size} URL targets.`);
console.log("");
if (broken.length === 0 && anchorBroken.length === 0) {
  console.log("✓ No broken internal links.");
  process.exit(0);
}

if (broken.length) {
  console.log(`✗ ${broken.length} broken page link(s):`);
  for (const b of broken) {
    console.log(`  [${b.from}]  ${b.href}   →   ${b.resolved}`);
  }
  console.log("");
}
if (anchorBroken.length) {
  console.log(`✗ ${anchorBroken.length} broken anchor link(s):`);
  for (const b of anchorBroken) {
    console.log(`  [${b.from}]  ${b.href}   →   ${b.resolved}`);
  }
}
process.exit(1);



