# Featured Examples Plan — First Two App Conversions

[Back to README](../../README.md) ·
[Packaging System](../packaging.md) ·
[How to Build Your First App](../app/first_app.md) ·
[Package Commands](../guide/packages.md) ·
[Cookbook End-to-End Packaging Example](../../cookbook/packaging/end-to-end/README.md) ·
[Blackjack Demo Plan](./blackjack_demo.md) ·
[XVA WWR Notebook v2 Plan](./xva_wwr2.md)

> **Status.** Authoritative plan for the **first two** packaging-era featured-example
> conversions. The chosen pair is:
>
> 1. `portfolio` → `portfolio-app`
> 2. `blackjack` → `blackjack-app`
>
> `xva-wwr` is **not** being dropped; it is deferred to a later third conversion so
> the first two featured apps present a broader and clearer story on the site.

---

## 1. Goal

Convert the current featured-demo model from:

- long-form featured markdown,
- loose cookbook program,
- notebook as a near-mirror of the prose,

into a consistent packaging-era model:

1. **Eta App** — the canonical runnable artifact under `packages/example/`
2. **Notebook** — a high-level guided walkthrough that references the app
3. **Featured doc** — a tight landing page that sells the simplicity of doing the whole workflow in Eta

The first two conversions should establish the template the rest of the site follows.

---

## 2. Why these two first

## 2.1 `portfolio`

`portfolio` remains one of the strongest demonstrations of Eta's core claim:
causal inference, optimisation, constraints, AAD, and ML living in one
language and one runtime.

It should stay because it anchors the serious, real-world side of the story.

## 2.2 `blackjack`

`blackjack` should be the second conversion because it broadens the story in a
way `xva-wwr` does not.

It shows:

- logic / ILP,
- causal inference,
- Monte Carlo,
- learning,
- CLP,
- policy compression,
- and a more playful, more immediately understandable domain.

As a pair, `portfolio + blackjack` shows range better than
`portfolio + xva-wwr`, which would otherwise make the first two flagship apps
feel too concentrated in quant-finance workflow territory.

## 2.3 `xva-wwr` later, not lost

`xva-wwr` stays on the roadmap as the **third** major conversion:

- still valuable,
- still technically strong,
- probably easier to package than `portfolio` structurally,
- but better positioned as an advanced deep-dive app rather than one of the
  first two homepage-shaping examples.

---

## 3. Common deliverable shape

Each converted featured example must produce exactly three user-facing assets.

### 3.1 App package

Each example becomes its own package app under `packages/example/`.

Required shape:

- `eta.toml`
- `README.md`
- `src/`
- `tests/`

Canonical run path should be package-first, e.g.:

```console
eta run -p portfolio-app
eta run -p blackjack-app
```

### 3.2 Notebook

Each example gets one notebook under `cookbook/notebooks/`.

The notebook is not the canonical implementation. It is the guided explainer.
It should call, import, or inspect app code rather than re-implement the full
system as an unrelated second artifact.

### 3.3 Featured doc

Each example keeps a featured page under `docs/featured/`, but the page is
rewritten to be much tighter:

- short sell,
- run commands near the top,
- one pipeline graphic,
- expected output summary,
- links to app package and notebook,
- deeper detail moved down or out.

---

## 4. Editorial rule for all future featured examples

The old style proves capability. The new style must prove **usability**.

The dominant message should become:

> "This whole workflow is one Eta app."

not:

> "This document inventories every advanced subsystem involved."

Every featured example should answer these five questions in the first screenful:

1. What does the app do?
2. Why is it interesting?
3. How do I run it?
4. What should I expect to see?
5. Why is Eta unusually good at this?

---

## 5. Conversion 1 — `portfolio` → `portfolio-app`

## 5.1 Source material

Current assets:

- `docs/featured/portfolio.md`
- `cookbook/numerics/portfolio.eta`
- `cookbook/notebooks/Portfolio.ipynb`

## 5.2 Target package

Create:

- `packages/example/portfolio-app/`

Recommended initial file layout:

- `packages/example/portfolio-app/eta.toml`
- `packages/example/portfolio-app/README.md`
- `packages/example/portfolio-app/src/portfolio_app.eta`
- `packages/example/portfolio-app/src/portfolio_pipeline.eta`
- `packages/example/portfolio-app/src/portfolio_report.eta`
- `packages/example/portfolio-app/tests/`

## 5.3 App contract

The app should expose:

- one pure pipeline function returning a structured artifact,
- one report/printing layer,
- one `main` entrypoint.

Recommended public surface:

- `(run-portfolio-demo)`
- `(run-pipeline ...)`
- `(main ...)`

The returned artifact should be stable and notebook-friendly. Headline keys should
stay small, explicit, and outcome-oriented:

- `run-config`
- `allocation`
- `allocation-pct`
- `return`
- `risk`
- `score`
- `scenarios`
- `decision-robustness`
- `stress-validation`
- `causal-audit`

Keep deeper diagnostics nested rather than flattening everything at top level.

## 5.4 Notebook conversion

Create or replace with:

- `cookbook/notebooks/portfolio-app.ipynb`

Notebook posture:

> "This notebook is the guided tour of `portfolio-app`."

Suggested sections:

1. Title and one-paragraph sell
2. Run the app and inspect the result artifact
3. Inputs: universe, DAG, constraints, scenarios
4. Causal identification
5. Return estimation
6. Feasible optimisation
7. AAD sensitivity view
8. Scenario stress view
9. Result interpretation
10. Links back to app source and references

The notebook should stop mirroring the entire long-form featured doc
section-for-section.

## 5.5 Featured-doc rewrite

Refactor `docs/featured/portfolio.md` into a tighter landing page.

New top-of-page structure:

1. **What this app does**
2. **Run it**
3. **What it shows**
4. **What success looks like**
5. **Pipeline at a glance**
6. **Links to app / notebook / references**

Portfolio-specific headline to preserve:

> Portfolio construction, causal adjustment, exact constraints,
> optimisation, and risk attribution in one Eta app.

---

## 6. Conversion 2 — `blackjack` → `blackjack-app`

## 6.1 Source material

Current design source:

- `docs/plan/blackjack_demo.md`

This conversion is more greenfield than `portfolio`, but it is strategically
important because it gives Eta a flagship example that is not finance-first.

## 6.2 Target package

Create:

- `packages/example/blackjack-app/`

Recommended initial file layout:

- `packages/example/blackjack-app/eta.toml`
- `packages/example/blackjack-app/README.md`
- `packages/example/blackjack-app/src/blackjack_app.eta`
- `packages/example/blackjack-app/src/blackjack_pipeline.eta`
- `packages/example/blackjack-app/src/blackjack_report.eta`
- `packages/example/blackjack-app/tests/`

If needed, split internals further into package-local modules for:

- rule induction,
- Monte Carlo simulation,
- count/representation learning,
- CLP strategy table generation,
- rule compression.

## 6.3 App contract

The app should produce one coherent top-level artifact representing the learned
or derived strategy state.

Recommended public surface:

- `(run-blackjack-demo)`
- `(run-pipeline ...)`
- `(main ...)`

Recommended headline outputs:

- learned rule summary
- causal EV summaries
- learned count / weight vector
- strategy chart summary
- compressed human-readable maxims
- validation metrics / coverage tables

## 6.4 Notebook conversion

Create:

- `cookbook/notebooks/blackjack-app.ipynb`

Notebook posture:

> "This notebook is the guided tour of `blackjack-app`."

Suggested sections:

1. What the app does
2. Run the app and inspect results
3. Rules from traces
4. Causal count / intervention story
5. Learned count representation
6. Strategy table generation
7. Compression back into human rules
8. Why Eta is a good fit for this workflow

The notebook should feel tighter and more exploratory than the detailed plan.

## 6.5 Featured-doc creation

Create a new featured page for blackjack once the app exists.

Recommended top structure:

1. **What this app does**
2. **Run it**
3. **What comes out**
4. **Why Eta makes this unusually simple**
5. **Pipeline at a glance**
6. **Links to app / notebook / references**

Blackjack-specific headline to preserve:

> Learn the rules, estimate action value, simulate play, derive optimal strategy,
> and compress it back into human-readable maxims — in one Eta app.

---

## 7. Package README contract

Both app packages should use the same README shape.

Required sections:

1. one-line description
2. prerequisites
3. run commands
4. expected output summary
5. package layout
6. notebook link
7. featured-doc link

This README is important because the `/packages/` page now auto-discovers package
entries from `eta.toml` and `README.md`.

---

## 8. Test contract

Both packages should ship with at least:

1. **smoke test** — app runs and returns/prints a result
2. **shape test** — expected top-level fields are present
3. **sanity test** — one or two invariants hold
4. **deterministic-seed test** — when seed-based output is part of the story

Examples:

### `portfolio-app`

- allocation sums to 1 (or 100%)
- scenario list is present
- causal-audit block exists
- robustness label is in an expected set

### `blackjack-app`

- strategy chart is non-empty
- learned weights / chart summary exist
- compressed-rule output exists
- snapshot or coverage checks pass for a small seeded run

---

## 9. Homepage and site positioning

After these two conversions, the two flagship examples should be positioned as:

- **Portfolio** — serious applied workflow
- **Blackjack** — compact multi-paradigm showcase

That gives the site one example that feels institutional and one that feels
surprising, elegant, and broadly accessible.

`xva-wwr` should remain visible later as an advanced finance case study,
not as one of the first two packaging-era featured examples.

---

## 10. Delivery order

## Phase 1 — establish the pattern with `portfolio`

1. create `portfolio-app`
2. migrate `cookbook/numerics/portfolio.eta`
3. add tests
4. rewrite notebook around app
5. tighten `docs/featured/portfolio.md`

## Phase 2 — add breadth with `blackjack`

1. create `blackjack-app`
2. implement the first runnable pipeline slice
3. add tests
4. create notebook around app
5. create/tighten featured doc

## Phase 3 — polish cross-links

1. ensure both apps appear well on `/packages/`
2. add clean links from featured docs to packages and notebooks
3. update homepage / featured routing if needed
4. keep `xva-wwr` as the next advanced conversion candidate

---

## 11. Acceptance criteria

This plan is complete when the repo has:

1. `packages/example/portfolio-app/`
2. `packages/example/blackjack-app/`
3. one notebook per app under `cookbook/notebooks/`
4. one tight featured page per app under `docs/featured/`
5. a canonical package-first run path for each app
6. test coverage proving each app runs and returns meaningful output
7. a clear site story where the first two featured apps complement rather than duplicate each other

---

## 12. Non-goals for the first two conversions

The first two conversions should **not** try to solve everything at once.

Not in scope for this phase:

- converting `xva-wwr` immediately,
- making one shared mega-demo workspace,
- turning the notebooks into the canonical implementation,
- keeping every detail from the old long-form docs in the new top-level pages,
- perfecting all possible CLI modes before the first app versions land.

The goal is to establish the pattern, not to finish every future featured example now.

---

## 13. Summary

The first two packaging-era featured-app conversions should be:

1. `portfolio` → `portfolio-app`
2. `blackjack` → `blackjack-app`

This gives Eta:

- one serious flagship example,
- one broad and memorable flagship example,
- a reusable app + notebook + tight-doc pattern,
- and room to bring `xva-wwr` back later as an advanced third showcase.

