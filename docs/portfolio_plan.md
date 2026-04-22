# `examples/portfolio.eta` Refactor Plan

## Purpose

This plan defines a practical refactor for `examples/portfolio.eta` so it is:

- easier to read and maintain
- aligned with current stdlib APIs
- split into verifiable phases with test gates

The example should remain a full end-to-end demonstration (data generation,
causal identification, estimation, optimization, scenarios, and control), but
without duplicated helper code and without long display/comment walls.

## Problems To Fix

Current issues in `examples/portfolio.eta`:

1. Mixed responsibilities in one flow:
   code, presentation formatting, and narrative prose are interleaved.
2. Repeated local helpers:
   utility functions overlap with stdlib functionality.
3. Inconsistent idioms:
   legacy loop/style patterns reduce readability.
4. Excessive verbosity:
   large blocks print explanatory text that belongs in docs.
5. Fragile structure:
   hard to verify behavior-preserving changes because stages are not cleanly isolated.

## Refactor Goals

1. Keep behavior and outputs stable (except harmless formatting differences).
2. Prefer stdlib helpers over local reimplementations.
3. Keep the pipeline explicit and top-down.
4. Reduce file size substantially while preserving educational value.
5. Keep changes reviewable by delivering in phases.

## Scope

In scope:

- `examples/portfolio.eta`
- related docs updates in `docs/portfolio.md` and this file
- small helper extraction only if it removes duplication cleanly

Out of scope:

- changing stdlib semantics for this plan
- introducing heavy macro frameworks only for compactness
- changing numerical model assumptions

## Style Rules For This Refactor

1. Use existing Eta stdlib naming and module conventions.
2. Prefer `else` over `(#t ...)` in `cond`.
3. Prefer named `let` for loops.
4. Prefer existing stdlib operations for clamp/stats/collection transforms.
5. Keep comments about functionality, not roadmap stage labels.
6. Keep source comments ASCII-clean (no mojibake).

## Phased Plan

### Phase 0 - Baseline and Safety Harness

Deliverables:

- capture baseline run output for the portfolio example
- document acceptable differences (whitespace/table formatting only)
- identify stage boundaries in current file and mark them minimally

Exit criteria:

- baseline output artifact available for comparison
- no code behavior changed yet

### Phase 1 - Mechanical Cleanup (No Logic Changes)

Deliverables:

- normalize conditional and loop style (`else`, named `let`)
- simplify repeated control flow patterns
- remove dead helpers that are exact duplicates
- keep public behavior unchanged

Exit criteria:

- example still runs
- no semantic drift observed in baseline comparison

### Phase 2 - Stdlib Alignment

Deliverables:

- replace local helper implementations with stdlib calls where equivalent
- remove manual accessor stacks where clearer accessor helpers exist
- reduce repeated ad hoc list/vector manipulation code

Exit criteria:

- same computational results for representative seeds/scenarios
- reduced helper footprint in `portfolio.eta`

### Phase 3 - Pipeline Structure and Readability

Deliverables:

- reshape top-level flow into clear stage blocks
- centralize stage reporting/printing conventions
- move explanatory prose from code into `docs/portfolio.md`
- keep concise functional comments in code

Exit criteria:

- top-level control flow reads as a recipe of stages
- sectioning is consistent and complete

### Phase 4 - Final Tidy and Documentation Sync

Deliverables:

- final ASCII comment/text cleanup in the example
- update docs with new structure and usage notes
- remove stale references in planning docs

Exit criteria:

- docs match current implementation
- no mojibake in comments or markdown

## Validation Gate (Run After Each Phase)

1. Build:
   `cmake --build ... --target eta_all -j 14`
2. C++ tests:
   `eta_core_test.exe`
3. Eta stdlib tests:
   `eta_test.exe --path <stdlib> <stdlib/tests>`
4. Example smoke:
   run `examples/portfolio.eta` and compare against baseline output

If a phase fails the gate, fix before moving forward.

## Risk Register

1. Numerical drift from helper substitution.
   Mitigation: compare key scalar outputs against baseline with tolerance.
2. Behavior changes hidden by formatting noise.
   Mitigation: normalize output before diff where possible.
3. Over-aggressive dedup reducing readability.
   Mitigation: prioritize clarity over minimal LOC.
4. Large single commit making review hard.
   Mitigation: keep phased commits aligned to this plan.

## Success Criteria

1. `examples/portfolio.eta` is materially shorter and easier to scan.
2. Stage flow is explicit and consistent.
3. Duplicate helper logic is minimized.
4. Output behavior is preserved.
5. Build/tests/example smoke pass at each phase gate.
6. Docs are updated and free of mojibake.

## Open Decisions

1. Keep all logic in one file vs move reusable blocks to `examples/_shared/`.
2. Strict line-count target vs readability-first target.
3. How much presentation formatting should remain in the example vs docs.

Default recommendation:

- prioritize readability and maintainability over an arbitrary line budget
- extract only stable, reusable helper blocks
- keep the example executable and pedagogical, with docs carrying long-form explanation
