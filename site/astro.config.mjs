// @ts-check
import { defineConfig } from "astro/config";

// Set BASE_PATH env var when building for GitHub Pages project URLs
// e.g. BASE_PATH=/eta npm run build
// @ts-ignore
const base = process.env.BASE_PATH ?? "";

/**
 * Remark plugin: rewrite intra-repo relative links from any markdown
 * source rendered into the site.
 *
 *  - Targets inside `docs/` (and ending in `.md` or having no extension)
 *    become clean `/<base>/docs/<path>/` site URLs. `language_guide` is
 *    are normalised to `language-guide` and `README` collapses to a folder
 *    index. `docs/old/` is excluded — that folder is not built into pages,
 *    so links into it go to GitHub instead.
 *  - Targets escaping `docs/` (e.g. `../README.md`, `../cookbook/foo.eta`,
 *    `../eta/core/.../foo.cpp`) are rewritten to the GitHub repo at
 *    `https://github.com/lewismj/eta/{blob,tree}/main/<path>` so the
 *    reader can still follow them.
 *  - Absolute, anchor-only, mailto and protocol URLs are left untouched.
 */
function remarkRewriteMdLinks() {
  const docsBase = `${base}/docs/`;
  const repoBlob = "https://github.com/lewismj/eta/blob/main/";
  const repoTree = "https://github.com/lewismj/eta/tree/main/";
  const skipFolders = ["docs/old/"];

  return (tree, file) => {
    const filePath = (file?.history?.[0] ?? file?.path ?? "").replace(/\\/g, "/");
    // Resolve where this source file sits inside the repo, so we can
    // turn relative URLs into repo-rooted paths. Two well-known anchors
    // cover everything we render today: `<root>/docs/...` and
    // `<root>/editors/.../README.md`.
    let repoFileDir = "";
    const docsM = filePath.match(/\/docs\/(.*)$/);
    const editorsM = filePath.match(/\/editors\/(.*)$/);
    if (docsM) {
      const inDocs = docsM[1];
      const inDir = inDocs.includes("/") ? inDocs.replace(/\/[^/]+$/, "") : "";
      repoFileDir = "docs" + (inDir ? "/" + inDir : "");
    } else if (editorsM) {
      const inEd = editorsM[1];
      const inDir = inEd.includes("/") ? inEd.replace(/\/[^/]+$/, "") : "";
      repoFileDir = "editors" + (inDir ? "/" + inDir : "");
    } else {
      return;
    }

    const rewriteToSite = (repoPath, hash) => {
      let inDocs = repoPath === "docs" ? "" : repoPath.slice("docs/".length);
      if (inDocs.endsWith(".md")) inDocs = inDocs.slice(0, -3);
      if (inDocs.endsWith("/README")) inDocs = inDocs.slice(0, -"/README".length);
      if (inDocs === "README") inDocs = "";
      const mapped = inDocs
        .split("/")
        .filter(Boolean)
        .map((s) => (s === "language_guide" ? "language-guide" : s))
        .join("/");
      return docsBase + (mapped ? mapped + "/" : "") + hash;
    };

    const visit = (node) => {
      if (node && node.type === "link" && typeof node.url === "string") {
        const original = node.url;
        const skip =
          /^[a-z]+:\/\//i.test(original) ||
          original.startsWith("#") ||
          original.startsWith("/") ||
          original.startsWith("mailto:");
        if (!skip) {
          const hashIdx = original.indexOf("#");
          const hash = hashIdx >= 0 ? original.slice(hashIdx) : "";
          const pathPart = hashIdx >= 0 ? original.slice(0, hashIdx) : original;
          if (pathPart) {
            const segs = (repoFileDir ? repoFileDir.split("/") : []).concat(
              pathPart.split("/")
            );
            const out = [];
            let escaped = false;
            for (const s of segs) {
              if (s === "" || s === ".") continue;
              if (s === "..") {
                if (out.length === 0) { escaped = true; break; }
                out.pop();
              } else out.push(s);
            }
            if (!escaped && out.length > 0) {
              const repoPath = out.join("/");
              const lastSeg = out[out.length - 1];
              const hasDot = lastSeg.includes(".");
              const isMd = repoPath.endsWith(".md");
              const inDocs = repoPath === "docs" || repoPath.startsWith("docs/");
              const inSkippedFolder = skipFolders.some((p) =>
                repoPath.startsWith(p)
              );

              if (inDocs && !inSkippedFolder && (isMd || !hasDot)) {
                node.url = rewriteToSite(repoPath, hash);
              } else {
                node.url = (hasDot ? repoBlob : repoTree) + repoPath + hash;
              }
            }
          }
        }
      }
      if (node && node.children) for (const c of node.children) visit(c);
    };
    visit(tree);
  };
}

/**
 * Remark plugin: convert ```mermaid fenced blocks to <div class="mermaid">
 * so the client-side mermaid.js library can render them.  This must run
 * BEFORE Shiki so the code node is never handed to the syntax highlighter.
 */
function remarkMermaid() {
  const esc = (s) =>
    s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  return (tree) => {
    const visit = (node) => {
      if (node.type === "code" && node.lang === "mermaid") {
        node.type = "html";
        node.value = `<div class="mermaid">\n${esc(node.value)}\n</div>`;
        delete node.lang;
        delete node.meta;
      }
      if (node.children) for (const c of node.children) visit(c);
    };
    visit(tree);
  };
}

/**
 * Remark plugin: drop the "Back to README" navigation paragraph that
 * appears near the top of most source markdown files. The site has a
 * full sidebar, so this row is redundant — but it stays in the .md
 * source so the file remains useful when viewed on GitHub.
 */
function remarkDropBackToReadme() {
  const re = /^\s*[←<-]*\s*back to readme\b/i;
  return (tree) => {
    if (!tree.children) return;
    tree.children = tree.children.filter((node) => {
      if (node.type !== "paragraph" || !node.children?.length) return true;
      // Find the first link in the paragraph; if its text matches
      // "Back to README" / "← Back to README" / "<- Back to README",
      // drop the entire paragraph.
      const firstLink = node.children.find((c) => c.type === "link");
      const linkText = firstLink?.children
        ?.map((c) => (c.type === "text" ? c.value : ""))
        .join("");
      if (linkText && re.test(linkText)) return false;
      // Also handle the case where the leading text node is "← " before
      // a link whose text is just "Back to README".
      if (node.children[0]?.type === "text") {
        const lead = node.children[0].value || "";
        const next = node.children[1];
        if (
          /^\s*[←<-]+\s*$/.test(lead) &&
          next?.type === "link" &&
          /^back to readme/i.test(
            next.children?.map((c) => (c.type === "text" ? c.value : "")).join("") || ""
          )
        ) {
          return false;
        }
      }
      return true;
    });
  };
}

/**
 * Remark plugin: turn GitHub-flavoured alert blockquotes into styled
 * callouts. Detects a leading `[!NOTE]` / `[!TIP]` / `[!IMPORTANT]` /
 * `[!WARNING]` / `[!CAUTION]` marker on the first line of a blockquote,
 * strips the marker, attaches a CSS class, and prepends a label paragraph.
 */
function remarkGfmAlerts() {
  const KINDS = ["NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION"];
  const re = new RegExp("^\\s*\\[!(" + KINDS.join("|") + ")\\]\\s*\\n?", "i");

  return (tree) => {
    const visit = (node) => {
      if (
        node &&
        node.type === "blockquote" &&
        node.children?.[0]?.type === "paragraph"
      ) {
        const para = node.children[0];
        const first = para.children?.[0];
        if (first?.type === "text") {
          const m = first.value.match(re);
          if (m) {
            const kind = m[1].toLowerCase();
            first.value = first.value.slice(m[0].length).replace(/^\s+/, "");
            // Drop a leading hard-break / empty leading text node.
            while (
              para.children.length &&
              ((para.children[0].type === "text" && para.children[0].value === "") ||
                para.children[0].type === "break")
            ) {
              para.children.shift();
            }
            const label = kind.charAt(0).toUpperCase() + kind.slice(1);
            node.data = node.data || {};
            node.data.hProperties = node.data.hProperties || {};
            node.data.hProperties.className = `alert alert-${kind}`;
            node.children.unshift({
              type: "paragraph",
              data: { hProperties: { className: "alert__label" } },
              children: [{ type: "text", value: label }],
            });
          }
        }
      }
      if (node && node.children) for (const c of node.children) visit(c);
    };
    visit(tree);
  };
}

/**
 * Remark plugin: strip an in-page "Contents" / "Table of Contents" H2
 * (and the list immediately following it) from the rendered output —
 * the right-rail TOC supersedes it. The marker stays in the source
 * markdown so the file remains readable on GitHub.
 */
function remarkDropInPageToc() {
  return (tree) => {
    if (!tree.children) return;
    for (let i = 0; i < tree.children.length; i++) {
      const n = tree.children[i];
      if (
        n.type === "heading" &&
        n.depth === 2 &&
        n.children?.length === 1 &&
        n.children[0].type === "text" &&
        /^(contents|table of contents)$/i.test(n.children[0].value.trim())
      ) {
        let j = i + 1;
        while (j < tree.children.length && tree.children[j].type !== "heading") j++;
        tree.children.splice(i, j - i);
        i--;
      }
    }
  };
}

/**
 * Custom Shiki theme that mirrors the hand-tuned palette used by the
 * homepage AAD code sample (black background, dusty-pink token ramp).
 */
const etaShikiTheme = {
  name: "eta-dark",
  type: "dark",
  colors: {
    "editor.background": "#000000",
    "editor.foreground": "#ece7e1",
  },
  tokenColors: [
    { scope: ["comment", "punctuation.definition.comment"],
      settings: { foreground: "#857d77", fontStyle: "italic" } },
    { scope: [
        "keyword",
        "keyword.control",
        "storage.type",
        "storage.modifier",
        "constant.language",
        "variable.language",
        "keyword.other",
      ],
      settings: { foreground: "#ff8fb1", fontStyle: "bold" } },
    { scope: [
        "entity.name.function",
        "support.function",
        "meta.function-call",
        "variable.function",
        "entity.name.tag",
      ],
      settings: { foreground: "#f0a4bc" } },
    { scope: ["string", "string.quoted", "string.template", "punctuation.definition.string"],
      settings: { foreground: "#d7b0bb" } },
    { scope: ["constant.numeric", "constant.language.boolean", "constant.character"],
      settings: { foreground: "#e4c1ca" } },
    { scope: ["keyword.operator", "punctuation", "meta.brace", "meta.delimiter"],
      settings: { foreground: "#a19a94" } },
    { scope: ["entity.name.type", "support.type", "support.class", "entity.name.class"],
      settings: { foreground: "#b69a76" } },
    { scope: ["variable", "variable.parameter", "variable.other", "source"],
      settings: { foreground: "#ece7e1" } },
  ],
};

export default defineConfig({
  base,
  output: "static",
  site: "https://lewismj.github.io",
  markdown: {
    remarkPlugins: [
      remarkMermaid,
      remarkDropBackToReadme,
      remarkGfmAlerts,
      remarkDropInPageToc,
      remarkRewriteMdLinks,
    ],
    shikiConfig: {
      // @ts-ignore
      theme: etaShikiTheme,
      wrap: false,
    },
  },
});

