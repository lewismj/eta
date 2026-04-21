# `examples/portfolio.eta` — Refactor Plan

> **Depends on:** completion of [`docs/stdlib_plan.md`](stdlib_plan.md).
> Every stdlib symbol cited below assumes the promotions in §"Macros to
> introduce" / §"Candidates to promote" of that plan have landed
> (`dotimes`, `clamp`, `alist`/`alist-from`, `list-mean`/`-variance`/
> `-covariance`, `with-port`, `define-aliases`, `defrel`/`tabled` macros).

---

## 1. Overview & Goals

`examples/portfolio.eta` is a 2 990-line "essay with code" that
demonstrates Eta's full pipeline (DGP → fact table → causal identify →
NN estimator → CLP(R) optimiser → AAD → scenarios → robustness →
dynamic control). Today it carries a long tail of helpers that are
either already in the stdlib or *should* be after `stdlib_plan`:
LCG RNG, `list-mean`/`list-variance`/`list-covariance`,
`clamp01`/`clamp-range`, `dict`/`dict-from`, `dotimes`, `report`,
hand-rolled `letrec`+`lambda` accumulator loops, dozens of
`(#t …)` cond fallbacks, and a custom prose/`display` wall around
every stage banner.

**Goals**

1. **Stdlib reuse.** Delete every helper that the post-`stdlib_plan`
   stdlib already provides; consume `std.stats`, `std.collections`,
   `std.math`, `std.io`, `std.core`, `std.causal`, `std.clpr`,
   `std.fact_table`, `std.torch` directly.
2. **Idiomatic Scheme.** Named-`let` for tail loops; `else` not
   `(#t …)`; `when`/`unless` for one-armed effects; `case` for
   symbol dispatch; `let*` only at logical phase boundaries; helpers
   `%`-prefixed; one `define`-form per file (drop `defun`).
3. **Succinctness.** Target **≤ 1 200 lines** by lifting the prose
   wall behind a small reporting DSL (`section`, `report`, `table`)
   and pushing repeated structural data (sector spec, scenario list,
   regimes/strategies dictionary) into top-level constants.
4. **Pipeline-as-recipe.** The body should read top-down as ~10
   stage forms — DGP → fact-table → estimator → optimiser →
   execution → diagnostics — not as 2 800 lines of instrumentation
   wrapping 200 lines of math.

Non-goal: changing the printed output. Numerical drift from
switching to stdlib stats must be ≤ float precision (modulo
formatting whitespace and ASCII vs. Unicode glyphs under the `win`
flag at line 59).

---

## 2. Current Shape Inventory

| § | Stage | Lines | Approx LOC | Dominant non-idiomatic patterns |
|---|---|---|---|---|
| **header** | Module + local DSL (`sep`, `dict`, `report`, `dotimes`) + intro prose | 1–141 | ~140 | Local re-defs of `dict`/`dotimes`/`report` (lines 68–104); per-line `display` wall for the intro narrative (lines 110–140); ASCII/Unicode branch via `(if win …)` repeated everywhere |
| **§0** | DGP, LCG noise, `make-fact-table`, sector loop, sample rows | 142–255 | ~115 | Bespoke `lcg-next`/`lcg-noise` (lines 173–190) — same as `monte-carlo-worker.eta`; `letrec`+`lambda` row loop (lines 202–224); seed-threading by hand (`seed1…seed4`, lines 229–232); 8 `display` calls for what is one table |
| **§1** | Symbolic objective + `simplify`/`diff`/`D` | 257–342 | ~85 | Reinvents the symbolic differentiator from `causal_demo.eta`/`symbolic-diff.eta` (lines 285–333) — five `(#t …)` clauses; `op`/`a1`/`a2` accessors are textbook `case`-on-`car`; one-line `defun`s |
| **§2** | DAG + `do:identify-details` + ASCII art | 344–421 | ~80 | Mostly fine; ~30 lines of pure prose around 4 lines of computation; ASCII/Unicode duplicated `if win` block (lines 370–380) |
| **§3** | CLP(R) feasibility check + caps | 422–468 | ~45 | Nine repeated `(clp:r>= …)` / `(clp:r<= …)` posts (lines 454–460) — `define-syntax`-able; only 2 lines of actual logic, rest is prose |
| **§4** | Tensor build, network train loop, `nn-predict`, per-sector causal expectation | 470–624 | ~155 | `letrec`+`lambda` train loop (lines 527–537) — should be `dotimes`; `cond`-on-symbol → sector code (lines 489–494) → `case` or `alist-ref`; sector spec re-stated 4× (lines 552–557, 600–603); 18 `display` lines for one verification table |
| **§4b** | Naive vs causal OLS | 625–693 | ~70 | Already calls `stats:ols-multi` ✓; manual `(car (cdr coeffs))` accessor stack (lines 643, 651–652) — use `nth` or named-record |
| **§5** | AAD `grad`, structural / learned / hybrid Σ(m), CLP(R) QP solver, base-case Σ | 694–1306 | **~615** | Two `letrec`+`lambda` AAD helpers (lines 708–724); local `clamp01`/`clamp-range`/`list-mean`/`list-variance`/`list-covariance`/`mean4-series`/`sigma-blend` (lines 799–859) — every one slated for `std.math`/`std.stats`; ad-hoc 10-tuple Σ representation with `(car (cdr (cdr …)))` decoders (lines 1043–1070); manual nested simplex scan (lines 1079–1102); `(#t …)` fallback in σ dispatch (line 1040); `cond` on `sigma-model` symbol (lines 1030–1041) → `case` |
| **§6** | CLP(R) QP solve, λ-sweep table, AAD marginals, counterfactual cap | 1308–1458 | ~150 | `(car (cdr (cdr …)))` weight-tuple shredding repeated at every call site (lines 1324–1333, 1378–1388); λ-sweep printer (`show-lambda-row`, lines 1408–1419) is hand-rolled column-formatter |
| **§7** | Scenario table, stability check, DAG sensitivity, uncertainty optimisation, stress validation, causal-decision coupling, distributed actors, dynamic control, `run-pipeline`, summary | 1460–2989 | **~1 530** | Every multi-element list (`scenarios`, `stage5-regimes`, `stage5-strategies`, `pipeline-result`) is a hand-built `dict`/`list`; nested `letrec` walk-regimes/walk-strategies (lines 2244–2259); per-row table printer copy-pasted three times (lines 2316–2345); `(if win "ASCII" "Unicode")` per line in the coupling chain (lines 2380–2394); `letrec`+`lambda` in the actor `dgp`/`avg-sent` worker (lines 2431–2444); whole "S8/S9" worth of content with no `S8.`/`S9.` banner |
| **summary** | Executive summary | 2944–2989 | ~45 | Pure `display` wall — should be one `section` + table |

**Top-level facts (`grep`):** 19 `letrec`+`lambda` loop sites,
20 `(#t …)` cond fallbacks, ~120 raw `display` calls, 7 `defun`
helpers that are stdlib promotion candidates.

---

## 3. Stdlib Substitutions

After `stdlib_plan` lands, every left-hand entry below is **dead code**
in `portfolio.eta` and should be deleted in favour of the right-hand
import.

| Local in `portfolio.eta` | Replace with | Lines today |
|---|---|---|
| `dict` macro | `alist` from `std.collections` | 68–71 |
| `dict-from` macro | `alist-from` from `std.collections` | 73–76 |
| `report-line` / `report-value-line` / `report` macro | `report` from `std.io` (promotion target — see §4 of this plan) | 78–93 |
| `dotimes` macro | `dotimes` from `std.core` | 95–104 |
| `sep` / `thin-sep` (and per-call `(if win …)`) | `section` / `subsection` from `std.io` (new — see §4) | 60–65, used ~25× |
| `lcg-next` / `lcg-noise` | `monte-carlo-worker.eta` already imports these — **promote both into `std.math` as `mc:lcg-next` / `mc:lcg-noise`** (extension to `stdlib_plan`) | 173–190 |
| `clamp01` (`< 0 → 0`, `> 1 → 1`) | `(clamp x 0.0 1.0)` from `std.math` | 799–803 |
| `clamp-range x lo hi` | `(clamp x lo hi)` from `std.math` | 805–809 |
| `list-mean` | `stats:mean` from `std.stats` | 811–815 |
| `list-variance` | `stats:variance` from `std.stats` | 817–825 |
| `list-covariance` | `stats:covariance` from `std.stats` | 827–840 |
| `mean4-series` (4-list pointwise mean) | `(map* (lambda xs (/ (apply + xs) 4.0)) a b c d)` via `std.collections` `map*` (n-ary) — or a one-liner local `%mean-series` over `std.collections` `zip` | 842–852 |
| `sigma-blend` (linear blend two lists) | `(map* (lambda (x y) (+ (* alpha x) (* (- 1 alpha) y))) a b)` from `std.collections` | 854–859 |
| `op`/`a1`/`a2` and `simplify`/`simplify*`/`diff`/`D` | Already in `examples/symbolic-diff.eta` and `causal_demo.eta`. **Promote to a new `std.symdiff` module** (extension to `stdlib_plan`); both examples and `portfolio.eta` then import it | 285–333 |
| `grad` (AAD driver) | Identical in `european.eta` / `xva.eta`. **Promote to `std.aad` as `aad:grad`** (extension to `stdlib_plan`) | 705–725 |
| `make-vars`, `dot-expr`, `apply-witness!`, `witness-numbers`, `pick-weight`, `weights-f->pct`, `weights->pct`, `solve-portfolio-for-lambda` | Several appear in `portfolio-lp.eta` too. Lift the QP-solve harness into `std.clpr` as `clpr:solve-qp` (extension; until then, keep local but as named-`let` not `letrec`+`lambda`) | 1116–~1300 |
| `symbol-member?` (lines 1665–1669) | `member` builtin (`(and (member x xs) #t)`) | 1665–1669 |
| `(car (cdr (cdr (cdr ...))))` weight/tau decoders (~25 sites) | `nth` from `std.collections`, or a `(define-record-type weights tech energy fin health)` | passim |
| Hand-rolled `walk-regimes`/`walk-strategies` (lines 2244–2259) | `(map* (lambda (r) (map* (lambda (s) (evaluate-stress-row s r)) strategies)) regimes)` then `concat` | 2244–2259 |
| `count-unique-actions` (`add-unique` letrec) | `(length (delete-duplicates xs))` once `delete-duplicates` is added (currently re-implements it) — flag for stdlib | 2564–2577 |
| Per-row table printers (copy-pasted in §7, §6 λ-sweep, §S5 stress) | `report-table` (new — see §4) | 1408–1419, 1524–1529, 2316–2345 |

---

## 4. Proposed Mini-DSL

Three candidates considered:

### Candidate A — "Reporting DSL only"

Just `(section …)` + `(subsection …)` + `(report label => value)` +
`(report-table headers rows)`. Smallest surface; the body still
threads state explicitly.

- ✅ Zero magic; expansion is one-to-one.
- ✅ Reuses the existing `report` macro at line 88.
- ❌ Does nothing for the RNG-state plumbing or the
  10+ pipeline-binding `define`s in §6/§7.

### Candidate B — "Recipe pipeline"

Adds a threading macro:

```scheme
(pipeline
  (=> dgp        (build-universe dgp-seed))
  (=> estimator  (train-estimator universe))
  (=> optimiser  (solve-portfolio estimator constraints lam))
  (=> execution  (run-scenarios optimiser '(...)))
  (=> diagnostics(diagnose execution))
  ...)
```

where `(=> name expr)` binds `name` to the value of `expr` *and*
prints `(section "name")` + `(report 'result => name)`.

- ✅ Forces the example to declare its top-level recipe in one
  place; everything else becomes helper definitions.
- ✅ Each stage is a value, not a side-effecting block — the
  current `define stage5-…` zoo collapses.
- ❌ Surface is novel; readers unfamiliar with the macro may find
  it harder to skim than a flat `(define stage5-foo …)` chain.

### Candidate C — "Declarative experiment shell"

```scheme
(define-experiment portfolio
  (config (dgp-seed 42) (lambda 2.0) (sigma-model 'hybrid) ...)
  (stages (s0 dgp)
          (s1 symbolic-spec)
          (s2 causal-identify)
          ...))
```

A single `define-experiment` macro expands to the whole top-level
sequence. Maximum compression but maximum opacity.

### **Recommendation: B (with A's reporting forms baked in).**

It surfaces the recipe at the top, keeps every stage inspectable as
a plain value, and reuses the `report` macro that is already in the
file (line 88). Candidate C is rejected as too magical for an
example whose pedagogical value is line-by-line readability.

### Sketch (`define-syntax`)

```scheme
;; In std.io (alongside the promoted `report`).

(define-syntax section
  (syntax-rules ()
    ((_ title body ...)
     (begin (newline) (display-banner title) body ...))))

(define-syntax subsection
  (syntax-rules ()
    ((_ title body ...)
     (begin (newline) (display-thin-banner title) body ...))))

;; pipeline binds *and* announces each step.
(define-syntax pipeline
  (syntax-rules (=>)
    ((_)                  (begin))
    ((_ (=> name expr) rest ...)
     (let ((name expr))
       (report 'stage => 'name)
       (pipeline rest ...)))))

(define-syntax with-rng
  (syntax-rules ()
    ((_ seed body ...)
     (parameterize ((current-rng-state seed)) body ...))))
```

`with-rng` lets `lcg-next`/`lcg-noise` thread state implicitly so
that `generate-sector-data` (currently lines 201–225, 24 LOC of
`letrec`+seed-passing) becomes:

```scheme
(define (generate-sector-data sector-sym sector-code base-beta)
  (dotimes (i 30)
    (let* ((beta (+ base-beta (* (- (rand-uniform) 0.5) 0.3)))
           (sentiment (rand-uniform))
           (macro (+ 0.15 (* 0.35 sentiment) (* (rand-uniform) 0.2)))
           (rate  (+ 0.01 (* (rand-uniform) 0.04)))
           (noise (rand-normal 0.0 0.05))
           (ret   (dgp-return beta macro sector-code rate sentiment noise)))
      (fact-table-insert! universe sector-sym beta macro rate sentiment ret))))
```

### Before / after — §0 sample-rows block (lines 237–251 → ~6 lines)

**Before** (15 lines of `display`/`fact-table-ref` mash):

```scheme
(display "  Fact table: ") (display (fact-table-row-count universe))
(display " observations across 4 sectors\n")
... (dotimes (i 3) (display "    ") (display (fact-table-ref ...)) ...)
```

**After:**

```scheme
(section "S0. Data Generation & Fact Table"
  (report "Observations" => (fact-table-row-count universe))
  (report "Seed"         => dgp-seed)
  (report-table '(sector beta macro rate sent ret)
                (fact-table-take universe 3)))
```

---

## 5. Phased Refactor Plan

### Phase 1 — Mechanical idiom cleanup (no behavioural change)

- [ ] Replace **all 20** `(#t …)` cond fallbacks with `(else …)`
      (line list above; grep `(#t ` for the live count).
- [ ] Convert **all 19** `letrec`+`lambda` loop sites (per the
      `letrec` grep) to named-`let`, in particular: `dotimes`
      expansion (98), DGP loop (202), training loop (527), AAD
      `mk`/`collect` (708, 718), `list-covariance` (830), `mean4-series`
      (843), `collect-sector-macro-residual` (862),
      `scenario-covariance-empirical` (941, 967), `sigma-min-risk-sampled`
      (1081), `tau`-walks (1841, 2244), `collect-sector-return-samples`
      (2068), `simulate-dynamic-policy-full` (2580), `add-unique` (2565),
      actor `dgp`/`avg-sent` (2431).
- [ ] Choose **one** definition form (`define`) and convert all 20+
      `defun`s.
- [ ] Replace `(when (= … 0) …)` style with `unless` / `when` already
      used (good); audit one-armed `(if … (begin …))` for promotion.
- [ ] Collapse the 9 repeated `(clp:r>= w 0.0) (clp:r<= w 1.0)` lines
      in §3 (lines 454–460) into a `(for-each ... vars)` over a list.
- [ ] Drop `(if win "ASCII" "Unicode")` per-line branches: extract a
      `glyph` helper alist (one definition, used everywhere).

### Phase 2 — Stdlib substitutions (after `stdlib_plan` lands)

- [ ] Delete local `dict`/`dict-from`/`dotimes`/`report`/`clamp01`/
      `clamp-range`/`list-mean`/`list-variance`/`list-covariance` and
      add `(import std.core)` (already), `(import std.collections)`,
      `(import std.math)`, `(import std.stats)` usage.
- [ ] Replace `lcg-next`/`lcg-noise` with the promoted `mc:lcg-next` /
      `mc:lcg-noise` (or wrap behind `with-rng` + `rand-uniform`).
- [ ] Replace the symbolic differentiator (lines 285–333) with
      `(import std.symdiff)`.
- [ ] Replace `grad` (lines 705–725) with `(import std.aad)` and
      call `aad:grad`.
- [ ] Switch the `(car (cdr ...))` weight/tau accessor patterns to
      either `nth` (`std.collections`) or, ideally, a new
      `(define-record-type weights …)` local to the example.
- [ ] Use `case` for the `sigma-model` dispatch (lines 1030–1041) and
      for the sector→code map (lines 489–494).
- [ ] Replace the hand-rolled stress-row matrix walk (2244–2259) with
      `(concat (map* (lambda (r) (map* (lambda (s) (evaluate-stress-row s r)) strategies)) regimes))`.
- [ ] Replace `symbol-member?` with `member`.

### Phase 3 — DSL adoption + section restructuring

- [ ] Introduce `section` / `subsection` / `report-table` in
      `std.io` (Phase-3 of `stdlib_plan` macro work).
- [ ] Wrap each existing `(sep) (display "S<n>. …\n") (sep)` triple
      with `(section "S<n>. …" …)`. ~12 sites.
- [ ] Convert all hand-printed tables (λ-sweep at 1408–1419,
      scenario at 1524–1529, stress matrix at 2316–2345, summary at
      2951–2987) to `report-table` calls.
- [ ] Add the missing **S8** and **S9** banners for the
      "DAG sensitivity / uncertainty optimisation / stress
      validation / dynamic control" content (lines ~1620–2940) so
      the file's self-declared section numbering matches
      `docs/portfolio.md`'s §0–§9 narrative.
- [ ] Lift the recipe to a top-level `(pipeline (=> dgp …) (=> spec …)
      (=> dag …) (=> constraints …) (=> nn …) (=> sigma …)
      (=> opt …) (=> scenarios …) (=> robustness …) (=> dynamic …))`
      block so the body reads as ~10 lines.

### Phase 4 — Prose / comment tidy + LOC budget

- [ ] Move multi-paragraph educational prose (intro 110–140, why
      synthetic 130–135, pipeline-layer narration 137–140, coupling
      chain 2378–2399, executive summary 2944–2987) into
      [`docs/portfolio.md`](portfolio.md). Leave only one-line "what
      this stage computes" comments in the source.
- [ ] Audit the 18-line "DGP structural vs NN estimate" verifier
      (605–623) — the table itself is two `report-table` rows; the
      24 lines of explanation belong in the doc.
- [ ] Verify final LOC ≤ 1 200 (target). Stretch ≤ 1 000.

---

## 6. Cross-Cutting Concerns

### Narrative-vs-code balance

The file today is ~70 % code, 30 % prose-via-`display`. Move the
prose to `docs/portfolio.md` (which already covers §0–§9) and let
the example read as a compact recipe. Each `(section …)` keeps a
one-line tagline; deeper exposition lives in the doc.

### Reproducibility

Surface `dgp-seed` (line 228) and `sigma-model` (788) as the *only*
top-level configuration in a single `(define-experiment-config …)`
block at the head of the body, so reviewers can see what determines
output shape without scrolling. Use `with-rng` so RNG state never
appears in the data-generation body.

### Output formatting

The ASCII-fallback (`win`, line 59) currently triggers ~30 inline
`(if win "ASCII" "Unicode")` branches. Centralise it once as
`(define (glyph k) (if win ascii-table[k] unicode-table[k]))` so
the body never branches on platform.

### Numerical stability

`stats:mean`/`-variance`/`-covariance` use the same naïve
sum-then-divide as the local versions (verified
`stdlib/std/stats.eta:44–53`); switching should be exact modulo
floating-point evaluation order. Add a regression test: run the
pre- and post-refactor binary on `dgp-seed = 42` and diff the
captured stdout (modulo whitespace) — expect zero numerical
divergence.

---

## 7. Risks & Compatibility

| Change | Risk | Mitigation |
|---|---|---|
| Drop local `list-mean`/`list-variance`/`list-covariance` | Stdlib uses `%stats-*` builtin, may evaluate sum in different order | Snapshot `etai examples/portfolio.eta > golden.txt` pre-refactor; diff post-refactor |
| Promote `lcg-next` into `std.math` | Other examples (`monte-carlo-worker.eta`) already use the same impl — verify identical output sequence under same seed | Add `stdlib/tests/mc.test.eta` pinning `(mc:lcg-next 42)` to the current value |
| Promote `grad` into `std.aad` | `european.eta`, `xva.eta` re-define the same procedure | Bulk import; one PR touches all three examples |
| `with-rng` parameter implementation | Eta's parameterize semantics under `dynamic-wind` must be verified | Land after `with-port` (see `stdlib_plan` Phase 2) which exercises the same machinery |
| ASCII-fallback (`win`) | Centralised glyph table risks missing a branch | Grep `(if win` post-refactor, expect ≤ 2 occurrences (the section banner + the DAG ASCII art) |
| `case` for `sigma-model` dispatch | `case` uses `eqv?`; symbols compare correctly but verify under the Eta reader | Existing stdlib `clpb.eta` test suite already covers this pattern |
| Lifting "S8/S9" content into proper sections | Re-numbering may break links from `docs/portfolio.md` | Audit `portfolio.md` for `§8`/`§9` references; update in same commit |
| `(define-record-type weights …)` | Changes representation of `opt-w*` — every downstream consumer must use accessors | Confine to portfolio.eta; do not export |

---

## 8. Success Criteria

1. **LOC budget.** `wc -l examples/portfolio.eta` reports ≤ 1 200
   (target), ≤ 1 000 (stretch). Today: 2 990.
2. **No re-implementations.** All of the following greps return zero
   matches inside `examples/portfolio.eta`:
   - `defun lcg-`
   - `defun list-(mean|variance|covariance)`
   - `defun clamp(01|-range)`
   - `define-syntax (dict|dict-from|dotimes|report)`
   - `letrec\s*\(\(\w+\s*\(lambda` (Phase 1 invariant)
   - `(#t ` (Phase 1 invariant)
3. **Equivalent output.** `etai examples/portfolio.eta | diff -w
   golden.txt` reports only formatting differences (whitespace,
   table alignment) and **no numerical drift**. ASCII branch (`win=#t`)
   verified on Windows CI.
4. **Top-level reads as a recipe.** The body of the `(begin …)` at
   line 57 contains ≤ 10 stage forms (one `pipeline` block plus
   `section`-wrapped helpers). Today: ~120 top-level forms.
5. **Stdlib alignment.** Every helper that was promoted is exercised
   by a test in `stdlib/tests/*.test.eta` (bullet ties into
   `stdlib_plan` Success Criterion 9).
6. **Doc parity.** `docs/portfolio.md` covers any prose that was
   removed from the source; `docs/portfolio.md` §0–§9 numbering
   matches the example's `S0`–`S9` banners.
7. **No broken cross-example dependencies.** `etai`-running
   `portfolio-lp.eta`, `causal_demo.eta`, `monte-carlo.eta`,
   `european.eta`, `xva.eta`, `sabr.eta` all still succeed (they
   share the promoted helpers).

---

## 9. Open Questions

1. **Promotion scope beyond `stdlib_plan`.** This plan assumes three
   *additional* promotions (`mc:lcg-*` → `std.math`, `aad:grad` →
   new `std.aad`, `simplify`/`diff` → new `std.symdiff`). Three
   options:
   - **A.** Fold these into `stdlib_plan` Phase 3 before starting
     this refactor.
   - **B.** Keep them local to `portfolio.eta` (and the other
     duplicating examples) for now; revisit later.
   - **C.** Park them under `examples/_shared/` as a private
     module imported by every example that needs them.

   Recommendation: **A** for `mc:lcg-*` (smallest, most-duplicated)
   and **C** for `aad:grad` + `symdiff` (still under design across
   examples).

2. **DSL aggressiveness.** Candidate B (`pipeline` + `section`/
   `report`) is the recommendation. Switch to **A** (reporting only)
   if the `pipeline` macro feels too clever; switch to **C**
   (`define-experiment`) if you want maximum compression and accept
   the opacity cost.

3. **LOC target.** 1 200 lines is achievable without splitting the
   file. 1 000 lines requires moving §S5's 615-line covariance
   machinery into either a doc appendix or a new
   `examples/portfolio-risk.eta`. Decide whether the example should
   stay monolithic.

