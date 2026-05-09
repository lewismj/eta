# Site Plan

[Back to README](../../README.md) |
[Quickstart](../quickstart.md) |
[Language Guide](../language_guide.md) |
[Native Sidecar Plan](native_sidecar_plan.md)

Status: proposed (2026-05-09).

---

## 1) Objective

Create a polished static website for Eta, hosted on GitHub Pages, that acts as:

1. a public landing page,
2. a documentation gateway,
3. a cookbook/example showcase,
4. a package and native-sidecar information hub,
5. a stable place to publish project roadmap material.

The visual target is an editorial technical site with generous whitespace, strong
typography, card-based navigation, and a refined look inspired by themes such as
Emil Olsson's HiResponse WordPress theme and the Hideo WordPress theme.

This is not a plan to run WordPress or clone a proprietary theme. The goal is to
build an Eta-branded static site that borrows broad design ideas while using
original implementation, styling, and assets.

---

## 2) Recommendation

Use:

```text
Astro + Markdown/MDX + CSS or Tailwind + GitHub Actions Pages deployment
```

Astro is the preferred site generator because it provides:

1. static output suitable for GitHub Pages,
2. first-class Markdown and MDX support,
3. component-based layouts,
4. low JavaScript by default,
5. content collections for docs, blog posts, cookbook pages, and package pages,
6. a modern design workflow without requiring a full application framework.

Recommended source location:

```text
site/
```

This keeps the website source clearly separated from authored documentation and
scales better for homepage assets, generated package pages, CI, and future
marketing content. Existing Markdown docs can still be imported from `docs/`
or copied into the site's content collections as needed.

---

## 3) Design and licensing constraints

Do not copy proprietary WordPress theme source code, CSS, images, icons,
templates, screenshots, or bundled assets unless the license explicitly allows
that use.

Acceptable approach:

1. study broad visual patterns,
2. create original Eta layouts and CSS,
3. reuse Eta-owned assets from `docs/img/`,
4. use open-source fonts and icons,
5. use Eta examples, screenshots, diagrams, and package metadata generated from
   this repository.

The resulting site should feel polished and editorial, but it should be clearly
Eta's own design.

---

## 4) Visual direction

Working style:

```text
Minimal editorial technical site
```

Core traits:

1. off-white base background,
2. black primary text with muted grey secondary copy,
3. large expressive homepage heading,
4. simple top navigation,
5. card-based feature sections,
6. readable documentation pages,
7. high-quality code blocks,
8. subtle borders and shadows,
9. responsive grid layout,
10. minimal client-side JavaScript.

Suggested palette:

```text
background: #f5f5f4   ; off-white base
surface:    #ffffff   ; cards, docs panels, elevated surfaces
text:       #000000   ; primary editorial text
muted:      #707070   ; secondary metadata and helper text
line:       #d8d6d1   ; soft neutral border derived from grey-on-offwhite
accent:     #fb8aa7   ; sparse highlight/accent
accent-2:   #564101   ; earthy sand/brown secondary accent
accent-3:   #00008b   ; optional deep marine accent for rare emphasis
code-bg:    #000000
code-fg:    #f5f5f4
```

The base should be monochrome-first: off-white, black, white, and grey do most
of the work. Accent colors should be used sparingly for links, labels, callouts,
or feature highlights rather than as a large background system. The palette is
an Eta-owned interpretation of the mood and contrast of
`https://emilolsson.com/login`; do not copy the site's CSS, assets, or font
files into the Eta repository.

Suggested fonts:

```text
Headings/UI: Barlow Condensed as the safest default DIN-like face;
             D-DIN if its source/license is verified;
             IBM Plex Sans Condensed as a conservative technical fallback
Body:        system-ui, Inter, IBM Plex Sans, or another readable sans
Code:        JetBrains Mono, ui-monospace, SFMono-Regular, Consolas,
             or Liberation Mono
```

A DIN-style heading and UI face gives the site a sharper technical/editorial
identity than using a programmer font everywhere. JetBrains Mono remains a good
fit for Eta code blocks because the project has strong JetBrains/CLion
development workflows and code-heavy documentation.

Do not bundle commercial DIN fonts such as FF DIN, DIN Next, or similar
proprietary families unless the project has a valid webfont license. Prefer an
open-source DIN-like font with clear redistribution terms for the default site
theme.

Open-source DIN-style candidates to evaluate:

1. **Barlow Condensed** — recommended default; DIN/institutional signage feel,
   broadly available, generally distributed under the SIL Open Font License.
2. **D-DIN** — closest visual match to DIN; only bundle after verifying the
   exact source and license for the chosen files.
3. **IBM Plex Sans Condensed** — less DIN-pure, but polished, technical, and
   conservative for documentation-heavy pages.
4. **Encode Sans Condensed** — useful secondary option for compact UI labels.
5. **Roboto Condensed** — widely available fallback with permissive licensing,
   though less distinctive.

Avoid relying on proprietary or system fonts such as Bahnschrift, FF DIN, DIN
Next, DIN 2014, or similar commercial DIN families unless the site has an
explicit webfont license and the deployment terms are documented.

---

## 5) Proposed site structure

Initial structure:

```text
site/
  astro.config.mjs
  package.json
  tsconfig.json
  public/
    favicon.svg
    images/
  src/
    content/
      docs/
      blog/
      cookbook/
      packages/
    components/
      Header.astro
      Footer.astro
      Hero.astro
      Card.astro
      CardGrid.astro
      CodeBlock.astro
      Sidebar.astro
      Callout.astro
      PackageCard.astro
    layouts/
      BaseLayout.astro
      DocsLayout.astro
      PostLayout.astro
      PackageLayout.astro
    pages/
      index.astro
      docs/[...slug].astro
      blog/[...slug].astro
      cookbook/[...slug].astro
      packages/index.astro
      packages/[...slug].astro
    styles/
      global.css
```

The site can either:

1. copy selected docs into `site/src/content/docs/`, or
2. generate content from existing Markdown files under `docs/` during the build.

The first option is simpler for the first version. The second option avoids
long-term duplication.

---

## 6) Homepage concept

Homepage sections:

1. hero,
2. feature cards,
3. language sample,
4. native sidecar story,
5. cookbook highlights,
6. package/workspace highlights,
7. links to docs, GitHub, and build/install material.

Sketch:

```text
┌─────────────────────────────────────────────┐
│ Eta                                         │
│ Docs  Guide  Cookbook  Packages  GitHub     │
├─────────────────────────────────────────────┤
│                                             │
│  A Lisp-like language for programmable      │
│  systems, quant workflows, packages, and    │
│  native sidecars.                           │
│                                             │
│  [Get Started] [Read the Guide]             │
│                                             │
├─────────────────────────────────────────────┤
│  Feature cards                              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ Lisp core│ │ Packages │ │ Sidecars │     │
│  └──────────┘ └──────────┘ └──────────┘     │
├─────────────────────────────────────────────┤
│  Cookbook examples / latest posts           │
└─────────────────────────────────────────────┘
```

Example hero copy:

```text
Eta is a Lisp-like language for programmable systems,
quant workflows, package-managed modules, and native sidecars.
```

Example feature cards:

1. **Lisp-shaped core** — modules, macros, records, lists, and symbolic
   programming.
2. **Package-aware tooling** — manifests, lockfiles, workspaces, and build
   artifacts.
3. **Native sidecars** — deterministic extension loading through package
   metadata.
4. **Cookbook-first learning** — practical examples for quant, logic, causal,
   concurrency, and ML workflows.

---

## 7) Documentation layout

Docs pages should use a conventional technical documentation layout:

```text
┌──────────────┬─────────────────────────────┬──────────────┐
│ Sidebar      │ Article                     │ On this page │
│              │                             │              │
│ Quickstart   │ # Language Guide            │ Syntax       │
│ Guide        │                             │ Modules      │
│ Stdlib       │ Main content...             │ Packages     │
│ Packaging    │                             │              │
└──────────────┴─────────────────────────────┴──────────────┘
```

Initial docs navigation:

1. Quickstart
2. Build guide
3. Language guide
4. Standard library
5. Packages
6. Native sidecars
7. Cookbook
8. Release notes
9. Next steps

Existing docs to surface:

```text
docs/quickstart.md
docs/build.md
docs/language_guide.md
docs/stdlib.md
docs/packaging.md
docs/guide/packages.md
docs/plan/native_sidecar_plan.md
docs/next-steps.md
docs/release-notes.md
```

---

## 8) Cookbook and examples

The site should make Eta's examples visible as a project strength. Initial
sections should highlight:

1. basics,
2. quant,
3. logic,
4. causal/do-calculus,
5. concurrency,
6. notebooks,
7. ML/native integrations.

Each cookbook page should include:

1. short summary,
2. source link,
3. rendered Eta snippet,
4. command to run the example,
5. related docs links.

---

## 9) Package and native sidecar pages

The site should eventually expose package-oriented pages because Eta now has
manifest, lockfile, workspace, materialization, and native sidecar support.

Package pages can show:

1. package name,
2. version,
3. source,
4. dependencies,
5. native sidecar metadata if present,
6. target triples,
7. artifact checksums,
8. README excerpt.

Example future page:

```text
/packages/eta-log-sidecar/
```

Native sidecar package pages should emphasize the lockfile-driven security
model:

1. selected target triple,
2. artifact relative path,
3. artifact checksum,
4. materialization-time verification,
5. load-time verification,
6. ABI compatibility.

---

## 10) Styling baseline

Minimal initial CSS direction:

```text
:root {
  --bg: #f5f5f4;
  --fg: #000000;
  --muted: #707070;
  --line: #d8d6d1;
  --accent: #fb8aa7;
  --accent-2: #564101;
  --accent-3: #00008b;
  --card: #ffffff;
  --code-bg: #000000;
  --code-fg: #f5f5f4;
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  background: var(--bg);
  color: var(--fg);
  font-family: Inter, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI",
    sans-serif;
  line-height: 1.6;
}

a {
  color: inherit;
}

.site-header {
  max-width: 1120px;
  margin: 0 auto;
  padding: 28px 24px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  border-bottom: 1px solid var(--line);
}

.brand {
  font-weight: 800;
  font-size: 1.25rem;
  text-decoration: none;
}

.site-nav {
  display: flex;
  gap: 20px;
}

.site-nav a {
  color: var(--muted);
  text-decoration: none;
}

.site-nav a:hover {
  color: var(--fg);
}

.hero {
  max-width: 1120px;
  margin: 0 auto;
  padding: 96px 24px 72px;
}

.hero h1 {
  max-width: 900px;
  margin: 0;
  font-size: clamp(3rem, 8vw, 6.5rem);
  line-height: 0.95;
  letter-spacing: -0.06em;
}

.hero p {
  max-width: 680px;
  color: var(--muted);
  font-size: 1.25rem;
}

.card-grid {
  max-width: 1120px;
  margin: 0 auto;
  padding: 32px 24px 96px;
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 20px;
}

.card {
  background: var(--card);
  border: 1px solid var(--line);
  border-radius: 18px;
  padding: 28px;
}

pre,
code {
  font-family: "JetBrains Mono", ui-monospace, SFMono-Regular, Consolas,
    "Liberation Mono", monospace;
}

pre {
  overflow-x: auto;
  padding: 20px;
  border-radius: 16px;
  background: var(--code-bg);
  color: var(--code-fg);
}
```

---

## 11) GitHub Pages deployment

Use GitHub Actions Pages deployment rather than relying on GitHub's built-in
Jekyll processing.

Example workflow:

```yaml
name: Deploy site to GitHub Pages

on:
  push:
    branches: [main]
    paths:
      - "site/**"
      - ".github/workflows/pages.yml"

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: pages
  cancel-in-progress: false

jobs:
  build:
    runs-on: ubuntu-latest
    defaults:
      run:
        working-directory: site

    steps:
      - uses: actions/checkout@v4

      - uses: actions/setup-node@v4
        with:
          node-version: 22
          cache: npm
          cache-dependency-path: site/package-lock.json

      - run: npm ci
      - run: npm run build

      - uses: actions/upload-pages-artifact@v3
        with:
          path: site/dist

  deploy:
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    runs-on: ubuntu-latest
    needs: build

    steps:
      - id: deployment
        uses: actions/deploy-pages@v4
```

If the site is published at a project path such as:

```text
https://eta-lang.github.io/eta/
```

then configure Astro with the correct `base` path.

---

## 12) Alternatives considered

### 12.1 Jekyll

Pros:

1. supported directly by GitHub Pages,
2. Markdown-first,
3. simple for small sites.

Cons:

1. older component model,
2. less pleasant for custom interactive docs,
3. less flexible for generated package/stdlib pages.

Jekyll is acceptable, but Astro is a better long-term fit.

### 12.2 Hugo

Pros:

1. very fast,
2. good for docs/blog sites,
3. mature static site generator.

Cons:

1. Go template syntax is less approachable than Astro components,
2. integrating MDX-style interactive content is less natural.

Hugo is a good fallback if build speed becomes a priority.

### 12.3 WordPress static export

Pros:

1. can use existing WordPress themes if licensed,
2. editorial tooling is familiar to many users.

Cons:

1. awkward in a source-controlled language repository,
2. generated output is harder to review,
3. theme/plugin output can be large,
4. less natural for generated API/package docs.

Not recommended.

### 12.4 Raw HTML/CSS

Pros:

1. maximum control,
2. no build tool.

Cons:

1. repetitive layouts,
2. poor content scaling,
3. harder to generate docs/package pages.

Not recommended beyond a temporary landing page.

---

## 13) Staged roadmap

### SITE0 - Design spike

Scope:

1. create a one-page static mockup under `site/`,
2. define colors, typography, spacing, and card style,
3. verify GitHub Pages base path behavior.

Gate:

1. local build produces static output,
2. homepage renders correctly at both `/` and project-page base path.

### SITE1 - Astro scaffold

Scope:

1. add Astro project,
2. add base layout,
3. add header/footer,
4. add global styles,
5. add homepage.

Gate:

1. `npm run build` succeeds,
2. static output is generated under `dist/`.

### SITE2 - Docs import

Scope:

1. add docs layout,
2. import or copy current Markdown docs,
3. add sidebar navigation,
4. add code block styling.

Gate:

1. quickstart, language guide, stdlib, packaging, and native sidecar plan render
   in the site.

### SITE3 - Cookbook and examples

Scope:

1. add cookbook landing page,
2. surface selected examples from `cookbook/`,
3. add syntax-highlighted Eta snippets,
4. link to source files.

Gate:

1. at least one example each from basics, quant, logic, causal, and concurrency
   is visible.

### SITE4 - Package and sidecar pages

Scope:

1. add packages landing page,
2. generate or hand-author initial package pages,
3. include native sidecar package pages,
4. document lockfile/checksum/ABI model.

Gate:

1. `eta-log-sidecar` has a rendered package page,
2. native fields and checksum model are documented.

### SITE5 - GitHub Pages deployment

Scope:

1. add GitHub Actions workflow,
2. configure Pages deployment,
3. verify published site URL,
4. document local development commands.

Gate:

1. push to `main` publishes the site,
2. broken internal links fail CI if practical.

### SITE6 - Polish and accessibility

Scope:

1. responsive navigation,
2. keyboard navigation,
3. color contrast checks,
4. metadata and social cards,
5. favicon and logo assets.

Gate:

1. site is usable on mobile and desktop,
2. Lighthouse accessibility score is acceptable,
3. metadata previews are correct.

---

## 14) Acceptance criteria

The site effort is complete when:

1. GitHub Pages serves an Eta-branded static site.
2. The visual style is editorial, polished, and distinct from copied WordPress
   theme assets.
3. Existing core docs are available through the site.
4. Cookbook examples are discoverable.
5. Native sidecar documentation is available and linked from the main navigation.
6. Package/native-sidecar pages can be generated or authored without redesigning
   the site architecture.
7. The site builds deterministically in CI.
8. The deployment pipeline is documented.
9. The repository README links to the published site once deployed.


