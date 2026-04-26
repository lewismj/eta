# Eta — Notebook Conversion, Binder Setup, and README Pitch Plan

Three coordinated changes:

1. Convert `docs/portfolio.md` → `examples/notebooks/Portfolio.ipynb`.
2. Make the repo Binder-friendly (so a "launch on Binder" badge in the
   README actually works for `LanguageBasics.ipynb`, `AAD.ipynb`,
   `Portfolio.ipynb`).
3. Re-pitch the README around the notebooks: shorten, link, defer
   reference content.

---

## 1. Convert `docs/portfolio.md` → `examples/notebooks/Portfolio.ipynb`

### 1.1 Target shape

Match the existing `AAD.ipynb` / `LanguageBasics.ipynb` conventions:

```json
"metadata": {
  "kernelspec":   { "display_name": "Eta", "language": "eta", "name": "eta" },
  "language_info": { "codemirror_mode": "scheme", "file_extension": ".eta",
                     "mimetype": "text/x-eta", "name": "eta",
                     "pygments_lexer": "scheme", "version": "0.1.0" }
},
"nbformat": 4, "nbformat_minor": 5
```

Cells should be re-runnable from top to bottom on a fresh kernel, with
`(import …)` cells gating the sections that need them. Use
`std.jupyter` rich-display helpers (`jupyter:table`, `jupyter:plot`,
`jupyter:dag`) wherever the markdown shows ASCII tables / Mermaid
diagrams.

### 1.2 Cell layout (one cell per markdown section, with code cells inserted)

| # | Cell type | Content (source) | Notes |
|---|---|---|---|
| 1 | md | Title + TL;DR | From "Causal Portfolio Engine" through "Executive Walkthrough" + 7-step list |
| 2 | md | "Why this matters" + tables | Drop the GitHub `[!IMPORTANT]` admonitions; render as quoted block |
| 3 | md | Pipeline DAG | Replace Mermaid with a static SVG export OR render with `(jupyter:dag …)` once stage objects exist; for v1 keep Mermaid as fenced code, JupyterLab renders it |
| 4 | code | `(import std.io) (import std.fact_table) (import std.causal) (import std.clpr) (import std.torch) (import std.stats) (import std.aad) (import std.net)` | One import cell up front |
| 5 | md | §0 Data & Fact Table | Prose + DGP equations |
| 6 | code | DGP generation + `make-fact-table` + `fact-table-build-index!` | Same code block as in `examples/portfolio.eta` §0; assert row count |
| 7 | code | `(jupyter:table universe)` | Rich-render the fact table |
| 8 | md | §1 Symbolic spec | Prose |
| 9 | code | `portfolio-objective`, `constraint-spec`, `D` calls | Print results |
| 10 | md | §2 Causal model + DAG | Prose |
| 11 | code | `(define market-dag …)` + `(jupyter:dag market-dag)` | Render the DAG |
| 12 | code | `(do:identify market-dag 'asset_return 'macro_growth)` + `findall` | Inspectable adjustment formula |
| 13 | md | §3 CLP(R) constraints | Prose |
| 14 | code | The `let*` block posting `clp:real`/`clp:r<=`/`clp:r=`/`clp:r-feasible?` | Show `#t` then a deliberate infeasible variant printing `#f` |
| 15 | md | §4 Learning + causal estimation | Prose |
| 16 | code | NN definition (`sequential`, `linear`, `relu-layer`), `train!`, `(eval! net)` | Lower epoch count for notebook (e.g. 1000) — flag with comment |
| 17 | code | Back-door integral over sentiment grid; print causal returns table via `jupyter:table` | |
| 18 | code | `stats:ols-multi` naive vs controlled | Print 4-row comparison table |
| 19 | md | §5 AAD sensitivities | Prose |
| 20 | code | `(grad portfolio-return-fn …)` + Euler decomposition | `jupyter:table` for RC_i |
| 21 | md | §6 Selection + λ-sensitivity + binding constraints | Prose |
| 22 | code | The continuous `clp:rq-minimize` solve | Print allocation + score |
| 23 | code | λ-loop `(0.5 1 2 3 5)` → `jupyter:table` |  |
| 24 | md | §6.5 Frictions (κ, γ, Λ) | Keep as prose only — no code (deferred to dynamic) |
| 25 | md | §7 Scenario analysis | Prose |
| 26 | code | Run 4 scenarios, populate fact table, `jupyter:table` | |
| 27 | code | `(jupyter:plot 'line scenario-names returns)` | Visual punch |
| 28 | md | §8 Stress validation | Prose |
| 29 | code | Stress-validation harness call → `jupyter:table` rows | |
| 30 | md | §9 Dynamic causal control | Prose |
| 31 | code | `(run-pipeline-dynamic …)` (small horizon, e.g. 4) and key `dynamic-control` fields | |
| 32 | md | Verification Summary + Appendices A/B + Source Locations | Single closing markdown cell with links rewritten to repo-relative URLs |

### 1.3 What to drop / soften from the .md

- GitHub-flavoured callouts (`> [!NOTE]`, `> [!IMPORTANT]`) → plain block
  quotes (Jupyter doesn't render them).
- "Click to expand" `<details>` blocks → keep open as code cells.
- Shell snippets (`etac -O …`, `etai …`) → drop (inside the kernel).
- "Reproducibility" warning paragraph → keep, mark cell as `markdown`
  with a leading "ℹ️" so it survives `nbconvert`.

### 1.4 Long-running cells

Three cells will likely be slow on Binder:

- §4 NN training (cap epochs to 1 000, comment "set to 5 000 for paper
  numbers").
- §7 scenario sweep — keep at 4 scenarios.
- §8 stress harness — restrict to two regimes (`dgp-correct`,
  `latent-confounding`) inside the notebook; full run lives in
  `examples/portfolio.eta`.

Add a top-of-notebook cell:

```text
;; Notebook tunables — increase for paper-grade results, decrease for
;; speed on Binder. The .eta script in examples/portfolio.eta uses the
;; full settings.
(define EPOCHS  1000)         ; full run: 5000
(define LAMBDA-GRID '(1 2 5)) ; full run: '(0.5 1 2 3 5)
```

### 1.5 Validation

After conversion, run `jupyter nbconvert --to notebook --execute
Portfolio.ipynb --output Portfolio.executed.ipynb` locally with the
xeus-eta kernel. Diff outputs against the `examples/portfolio.eta`
console transcript for §6 (allocation) and §7 (scenario ordering) —
those are the deterministic anchors.

### 1.6 Cross-linking

- Update `docs/portfolio.md` to add a top banner:
  > **Interactive version:**
  > [`examples/notebooks/Portfolio.ipynb`](../examples/notebooks/Portfolio.ipynb).
- Update `examples/notebooks/Portfolio.ipynb` final cell with a link
  back to `docs/portfolio.md` for the long-form prose.

---

## 2. Binder readiness

Current `binder/` already contains `environment.yml`, `apt.txt`,
`postBuild`, and `recipes/xeus-eta/meta.yaml` exists. The realistic
question is **does `xeus-eta` actually exist on conda-forge yet?** If
not, Binder cannot install it from `conda-forge` — that is the gating
issue.

### 2.1 Decision tree

```
Is xeus-eta published on conda-forge?
├── YES  → keep environment.yml as-is, add Dockerfile-free Binder badge,
│          done.
└── NO   → switch Binder to a Dockerfile-based image that builds
           eta + xeus-eta from source, OR temporarily host a private
           conda channel.
```

The recipe in `recipes/xeus-eta/meta.yaml` looks ready to submit to
conda-forge but the published-package status is the prerequisite. Until
that lands, Binder will fail on `conda install xeus-eta`.

### 2.2 If `xeus-eta` is **not yet on conda-forge** — Binder via Dockerfile

Replace `binder/environment.yml` + `binder/postBuild` with a
`Dockerfile` at repo root or in `binder/`. Sketch:

```dockerfile
FROM condaforge/mambaforge:latest

# Build deps
RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build git graphviz libssl-dev \
 && rm -rf /var/lib/apt/lists/*

# Runtime / kernel deps via conda-forge
RUN mamba install -y -c conda-forge \
    python=3.11 jupyterlab \
    xeus=5 xeus-zmq=3 \
    libtorch nng eigen nlohmann_json \
 && mamba clean -afy

ARG NB_USER=jovyan
ARG NB_UID=1000
RUN useradd -m -s /bin/bash -u ${NB_UID} ${NB_USER}
USER ${NB_USER}
WORKDIR /home/${NB_USER}

# Build eta + eta_jupyter
COPY --chown=${NB_USER}:${NB_USER} . /home/${NB_USER}/eta
RUN cd eta && cmake -S . -B out/release -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
 && cmake --build out/release --target eta_jupyter eta_all -j

ENV PATH=/home/${NB_USER}/eta/out/release/bin:$PATH
ENV ETA_MODULE_PATH=/home/${NB_USER}/eta/stdlib

RUN eta_jupyter --install --user

# Notebooks visible at the JupyterLab root
WORKDIR /home/${NB_USER}/eta/examples/notebooks
```

Trade-offs: cold-build on Binder takes 15–25 minutes the *first* time a
commit is launched. Subsequent launches reuse the cached image. This is
the standard pattern for kernels that are not yet on conda-forge.

### 2.3 If `xeus-eta` **is** on conda-forge

Current files are nearly correct; tighten only:

- `binder/environment.yml` — pin: `xeus-eta=0.1` to keep launches
  stable. Add `pip: [ipywidgets]` if any comm widgets need it.
- `binder/apt.txt` — drop `libssl-dev` (only build-time); keep
  `graphviz` for any DAG rendering done by Python helpers.
- `binder/postBuild` — replace with:

  ```bash
  #!/usr/bin/env bash
  set -euo pipefail
  eta_jupyter --install --sys-prefix
  jupyter labextension list
  ```

  so Binder logs show whether the labextension is registered.

### 2.4 Repo-side changes either way

- Add a `.binder/` symlink or move contents of `binder/` to `.binder/`
  (mybinder.org accepts both, but `.binder/` is the preferred name).
- Add a Binder badge to README — see §3.
- Set the JupyterLab default landing folder to `examples/notebooks/`
  via `postBuild` or `Dockerfile` `WORKDIR`.
- Add a `binder/README.md` with one-paragraph "what to expect on first
  launch" (slow first build if Dockerfile path).
- Consider an `examples/notebooks/README.md` index linking the three
  notebooks with one-sentence descriptions — also acts as the JupyterLab
  landing page when the file browser opens.

### 2.5 Performance / size considerations

- libtorch is large (~600 MB). Binder image will be substantial. If
  this becomes a problem, ship a CPU-only libtorch slim variant via a
  build flag (currently the `torch` target links against the bundled
  libtorch — confirm a CPU-only variant is selectable).
- The Portfolio notebook needs torch; basics + AAD do not. Consider
  splitting into two Binder configs: `binder-light/` (no torch) and
  `binder-full/` (torch) — the README links to the lighter one by
  default, with the full one behind a "run the portfolio demo"
  sub-link. This keeps the casual-first-look launch under 1 minute.

---

## 3. README simplification → pitch with notebook links

### 3.1 Target structure (≤ 200 lines, vs. current 434)

```
1.  Header (logo + tagline, unchanged)
2.  ONE-paragraph pitch
3.  ▶ Try it now — three Binder badges (Basics, AAD, Portfolio)
4.  What is Eta? — 4-bullet list, no table
5.  Showcase — 3 notebooks as cards (link + 1-sentence each)
6.  Quick install (collapsed/short)
7.  Documentation index (link grid, current "Documentation" table moved
    to docs/index.md)
8.  License
```

### 3.2 The three Binder badges (top of README)

```markdown
[![Binder – Language Basics](https://mybinder.org/badge_logo.svg)](
  https://mybinder.org/v2/gh/lewismj/eta/HEAD?labpath=examples%2Fnotebooks%2FLanguageBasics.ipynb)
[![Binder – AAD](https://mybinder.org/badge_logo.svg)](
  https://mybinder.org/v2/gh/lewismj/eta/HEAD?labpath=examples%2Fnotebooks%2FAAD.ipynb)
[![Binder – Portfolio](https://mybinder.org/badge_logo.svg)](
  https://mybinder.org/v2/gh/lewismj/eta/HEAD?labpath=examples%2Fnotebooks%2FPortfolio.ipynb)
```

### 3.3 Pitch paragraph (replaces the 65-line headline-features table)

> **Eta is a Lisp-family language for quants and ML researchers** —
> Scheme syntax, a NaN-boxed bytecode VM, and first-class support for
> reverse-mode AD, libtorch tensors, Pearl's do-calculus,
> CLP(R)/CLP(FD)/CLP(B), Erlang-style supervised actors, and columnar
> fact tables, all in one program. Click any badge above to run a live
> notebook — no install required.

### 3.4 "Showcase" section (the new emotional centre)

Replace the "Headline Features" table and "Featured Examples" block
with three card-style entries (rendered as a small markdown table or
just bold + indent):

```markdown
### 🚀 Showcase Notebooks

- **[Language Basics](examples/notebooks/LanguageBasics.ipynb)** —
  S-expressions, modules, macros, the REPL workflow.
- **[AAD: Greeks in One Backward Pass](examples/notebooks/AAD.ipynb)** —
  reverse-mode autodiff for Black–Scholes Greeks; the same technique
  production xVA desks use.
- **[Causal Portfolio Engine](examples/notebooks/Portfolio.ipynb)** —
  do-calculus + CLP(R) QP + AAD risk + actor-parallel scenarios on a
  realistic 4-asset book.

> Detailed write-up: [docs/portfolio.md](docs/portfolio.md).
```

### 3.5 Move out of README into linked pages

| Current README block | Move to |
|---|---|
| Compilation Pipeline diagram + table | `docs/architecture.md` (already exists; just remove duplicate from README) |
| "Key Design Highlights" table | `docs/architecture.md` (append) |
| Standard Library table | `docs/modules.md` (already covered; replace README block with one link) |
| Bundle Layout tree | `docs/quickstart.md` |
| Project Structure tree | `docs/build.md` |
| Full Documentation table | `docs/index.md` (new file or `docs/README.md`) |

The README ends up with:
- 1 short pitch
- 3 Binder badges
- 1 minimal-feature bullet list
- 1 showcase section
- 1 "Install / build" collapsed section (link-out)
- 1 "Documentation index" link-out
- License

### 3.6 Quick-install collapsed block

Keep the current installer one-liner but inside `<details>`:

```markdown
<details>
<summary>Local install (Windows / Linux)</summary>

…current install script + VS Code paragraph…

</details>
```

This preserves the existing entry point for users who land via the
release page rather than Binder.

### 3.7 Acceptance criteria for the new README

- ≤ 200 lines.
- Three Binder badges in the first viewport (above the fold).
- No tables of more than 4 rows.
- Every removed table is replaced by a single link.
- The phrase that survives a 5-second skim: *"click a badge, get a live
  notebook."*

### 3.8 Sequencing

Order the work to keep the README internally consistent:

1. **Land `Portfolio.ipynb`** (otherwise the third badge 404s).
2. **Land Binder config** (`Dockerfile` or `environment.yml`).
3. **Verify the three Binder URLs build** (cold-launch each badge).
4. **Then rewrite the README** — at that point all links resolve.
5. Add a CI smoke job (optional) that runs `repo2docker` on the
   `binder/` config to catch breakage before merge.

---

## Summary

| Task | Output | Blocking dependency |
|---|---|---|
| 1 | `examples/notebooks/Portfolio.ipynb` (32 cells, kernel = eta) | None |
| 2 | Binder config that builds | `xeus-eta` on conda-forge **or** Dockerfile |
| 3 | README at ~200 lines, 3 Binder badges, notebook-led pitch | Tasks 1 & 2 must land first so links resolve |

The XVA demo plan slots in *after* this work — the simplified README
gives it a clean "Featured demo" slot to occupy without competing with
40 lines of feature tables.

