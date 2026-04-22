# Eta Standard Library — Idiomatic Scheme Refactor Plan

## Overview & Goals

**Ultimate goal: a compact, idiomatic-Scheme stdlib.** Concretely:
fewer lines, fewer files-with-divergent-style, R5RS/R7RS surface
names as the *canonical* spelling, and deletion preferred over
rewriting whenever a stdlib symbol or builtin already does the job.

The Eta stdlib (`stdlib/prelude.eta` + `stdlib/std/*.eta`) is functionally
solid but stylistically heterogeneous: some files use `defun`, others use
`(define (f ...))`; `cond` fallbacks alternate between `(#t …)` and
`(else …)`; tail loops sometimes use `letrec`+`lambda`, sometimes
named-`let`; helpers are variously prefixed `%`, `__`, or unprefixed.

### Scope policy (resolves the earlier "preserve every public name" tension)

Earlier drafts of this plan said "preserve every public name and
semantic" *and* scheduled breaking renames (`assert` → `assert-fact!`,
`display-to-string` → `display->string`, `clp:!=` → `clp:<>`, OLS
tuple → record, `print`/`println` kept as primary). That is a
contradiction. The resolved policy is:

1. **R7RS-style names are canonical.** Where a Scheme-standard name
   exists for what the stdlib already does (`display`, `newline`,
   `write`, `display->string`, `assert-fact!`), it becomes
   the primary export. Eta-specific names that **duplicate** an
   R7RS name (`print` ≡ `display`, `display-to-string`,
   `assert`, `retract`, `clp:!=`) become **deprecated aliases**
   retained for exactly one minor release, then deleted.
   Eta-specific names that **add genuine value** over R7RS
   (`println` = `display` + `newline`; `eprintln` = the same on
   stderr) are **kept as documented Eta extensions** — not
   deprecated, but flagged "non-R7RS" in the `std.io` module
   header. The litmus test is: *does the name save callers from
   writing the same two-form combo every time?* If yes, keep;
   if it is just a synonym, deprecate.
2. **Deletion beats rewriting.** Any helper that *strictly*
   duplicates a builtin (`%clp-member?` ↔ `member`) is deleted, not
   "modernised". This is the source of much of the verbosity
   reduction; mechanical style cleanup (named-`let`, `else`) is
   secondary.
   *Counter-example for clarity:* `assoc-ref` is **not** strict
   duplication — it composes `assoc` + `cdr` + a `#f`-safe guard,
   saving ~3 forms per call site across ~129 sites; it is **kept
   and promoted** (see Phase 0). The litmus test is "does deleting
   it make call sites longer overall?" If yes, keep.
3. **Semantic compatibility, not nominal compatibility.** Public
   *behaviour* is preserved through the deprecation window via
   aliases; public *spellings* are not preserved beyond it. The one
   exception is `defrel`/`tabled` — see the macro section: the
   runtime-procedural form is *kept* alongside any new syntactic
   sugar, because computed-head clause definition (e.g. `(defrel
   (cons name args) …)` or `(apply defrel computed-clause)`) is a
   real use case in `stdlib/std/db.eta:212–229` that a macro-only
   move would silently break.

Non-goals: changing the module system; touching the
`(module … (export …) (begin …))` layout documented in
[`docs/modules.md`](modules.md).

### Verbosity And Idiom Bar

This refactor is evaluated primarily by **readability and Scheme
idiomaticity**, not by `wc -l` or fixed line-count quotas.

Acceptance bar:

- Repetition is collapsed: mechanical wrapper blocks are replaced with
  aliases or macros where appropriate.
- Canonical names are Scheme-like where possible; non-standard names are
  either deprecated aliases (with a removal window) or explicitly
  documented Eta extensions.
- Call sites get simpler on average (especially in `examples/` and
  `stdlib/tests/`), not more verbose.
- Safety and semantics are preserved while simplifying syntax
  (`dynamic-wind` where needed, additive macro strategy for
  `defrel`/`tabled`, one-release alias compatibility).
- `std.prelude` stays focused: deprecated aliases live in owning modules,
  not in prelude's primary export surface.

Tracking signals (non-gating, qualitative):

- `stats.eta` / `torch.eta` alias-heavy sections read as compact alias
  tables, not dozens of repetitive wrappers.
- CLP poster families with repeated skeletons are factored through
  `define-syntax` where it improves clarity.
- Duplicate helpers (`%clp-member?`, shadowing adapters, etc.) are
  removed or collapsed.
- No new style divergence is introduced while refactoring existing files.

The phased plan below is **re-ordered to deletion-first** (new
Phase 0); style cleanup is Phase 1; structural rewrites Phase 2;
macros Phase 3.

> **Note on macros.** Eta supports R7RS-style hygienic macros via
> `define-syntax` + `syntax-rules` (verified by the VM test suite and
> used in `examples/portfolio.eta`). **No file under `stdlib/std/`
> currently uses them** — every "macro-shaped" abstraction in the
> stdlib today is either a `defun` built-in form or a procedure that
> takes a quoted list (e.g. `(defrel '(edge a b))` in `db.eta`). This
> plan therefore treats *adoption of `define-syntax`* as a first-class
> refactor lever; see the new
> [§"Macros (`define-syntax`)"](#macros-define-syntax) below.

---

## Scheme Idiom Guidelines (Style Checklist)

Adopt and enforce across every file in `stdlib/std/`:

1. **Function definition form.** Use `(define (f x …) body)` for
   ordinary procedures and `(define f (lambda …))` only when needed
   (e.g. point-free combinators). `defun` is an Eta-specific built-in
   form (not Scheme); prefer `define` and reserve `defun` for the
   handful of cases where its argument-handling differs measurably
   from `define`. Pick **one** form per file. Today
   `core.eta`/`math.eta`/`io.eta` use `defun`,
   `fact_table.eta`/`db.eta`/`stats.eta`-style files use `define`.
   *(If `defun` is itself implementable as `(define-syntax defun
   (syntax-rules () ((_ (f . args) body ...) (define (f . args)
   body ...))))`, consider promoting that definition into `prelude.eta`
   so the surface form stays available without a built-in.)*
2. **`cond` fallback** is always `(else …)`, never `(#t …)`. Currently
   inconsistent: see `core.eta:74`, `core.eta:92`, `math.eta:37`,
   `collections.eta:67/92/99`, `db.eta:40/50/57` etc.
3. **Predicates end in `?`** (`atom?`, `even?`, `windows?`) — already
   followed; keep `any?` / `every?` despite SRFI-1's `any`/`every`
   for internal consistency, but document the deviation.
4. **Mutators end in `!`** (`fact-table-insert!`, `register-attr-hook!`,
   `set-current-output-port!`) — already followed.
5. **R7RS-style names are canonical; Eta-isms are aliases or
   explicit extensions.** Conversions use `->` (`vector->list`,
   `list->vector`, `display->string`). The **canonical R7RS output
   primitives** are `display`, `write`, and `newline` (confirmed in
   `stdlib/std/io.eta:4` and `docs/runtime.md:357`). Treatment of
   existing names:
   - `print` is a verbatim synonym for `display` → becomes a
     one-line deprecated alias in `std.io`, removed from
     `std.prelude`'s primary export block.
   - `display-to-string` → `display->string` (pure `->` rename);
     old name kept as a one-release deprecated alias.
   - `println` and `eprintln` are **Eta convenience extensions**
     (`display` + `newline`, optionally to stderr). They are
     *not* R7RS but are genuinely useful and heavily used across
     examples. **Keep them as documented non-standard exports**
     (not deprecated). Flag them in the `std.io` module header as
     "Eta extensions, not R7RS" so readers know the deviation.
   - `assert` → `assert-fact!` and `retract` → `retract-fact!`
     (canonical — see the DB Mutation Naming box below); old
     names kept as one-release deprecated aliases in `std.db`.
   - `clp:!=` → `clp:<>` (Scheme tradition); alias in `std.clp`.

   > **NB:** Earlier drafts listed `write-line` as an R7RS
   > canonical output primitive. `write-line` is *not* R7RS — it's
   > SRFI-6 / Racket. It is dropped from the canonical list. If
   > the refactor wants a `(define (write-line s) (display s)
   > (newline))` helper, add it as an **Eta extension** clearly
   > marked as such, alongside `println`.

   > **DB mutation naming (resolves open question 1):**
   > **canonical names are `assert-fact!` / `retract-fact!`**.
   > Rationale: (a) describes the operation (asserting a *fact*)
   > rather than re-encoding the module name; (b) matches the
   > Prolog/Datalog idiom the module implements; (c) leaves the
   > `db-` prefix free for future higher-level helpers
   > (`db-open`, `db-close`, `db-transaction`) that will feel
   > awkward next to a `db-assert!` that is really about facts,
   > not connections. The old `assert` / `retract` spellings
   > become one-line deprecated aliases inside `std.db`, not
   > re-exported from `std.prelude`.
6. **Tail recursion via named-`let`.** Replace
   `(letrec ((loop (lambda (i acc) …))) (loop 0 '()))` with
   `(let loop ((i 0) (acc '())) …)`. Affects `core.eta:80–84`,
   `math.eta:104–116`, `io.eta:54–71`, `collections.eta:53–59,
   178–224`, `fact_table.eta:88–123`, `db.eta:107–115`,
   `supervisor.eta`, and most `clp.eta` inner loops.
7. **Use `when` / `unless`** for one-armed conditionals with side
   effects (already used in `collections.eta:179`, `181`, `190`,
   `222`; missing in `io.eta` redirect helpers and `db.eta`).
8. **`case` for symbol/literal dispatch** (e.g. `clp:domain-z?` /
   `clp:domain-fd?` chained `cond` in `clp.eta:82–110` is a textbook
   `case` on `(car dom)`).
9. **Internal helpers prefixed `%`** (Eta convention already used in
   `clp.eta`, `clpb.eta`, `causal.eta`). Convert `__map`, `__foldl`,
   `__make-result`, `__make-group-result`, `__make-summary` in
   [`std/test.eta`](../stdlib/std/test.eta) to `%map` / `%foldl` /
   `%make-result` etc.
10. **Doc comments** use `;;;` for module-level prose and `;;` for
    in-body explanations (already followed; enforce by lint). First
    line is a one-sentence purpose, blank `;;;` line, then prose.
11. **Module export hygiene.** Group exports by category with `;; ──`
    comment headers (already done in `prelude.eta`, `clp.eta`,
    `collections.eta`); do this for `db.eta`, `supervisor.eta`,
    `clpr.eta`, `freeze.eta`.
12. **Resource safety via `dynamic-wind`.** Already used in `with-socket`
    ([`net.eta:45`](../stdlib/std/net.eta)); apply to
    `with-output-to-port` / `with-input-from-port` / `with-error-to-port`
    in [`io.eta:76–97`](../stdlib/std/io.eta) so a non-local exit
    restores the port.
13. **Avoid mutation when a fold suffices.** `db.eta`'s alist registry
    (`*db-registry*`, `*db-tabled*`, `*db-table-status*`,
    `*db-table-answers*`) uses `set!` everywhere; encapsulate behind
    `%registry-add` / `%registry-lookup` and consider promoting to a
    hash table once one is available.
14. **Indentation.** Two-space per form; `cond` clauses aligned under
    the keyword; `let`-binding column aligned. Currently uniform
    except `clpb.eta:58–63` (extra indent) and `collections.eta:148–169`
    (`sort` body irregular).
15. **Macros over manual repetition.** Whenever a file contains *N*
    near-identical `(defun foo (x …) (and (%prim! x …) (attach …)))`
    blocks, that is a `define-syntax` site. See the dedicated section
    below.

---

## Macros (`define-syntax`) <a id="macros-define-syntax"></a>

Eta supports R7RS hygienic macros (`define-syntax` + `syntax-rules`,
with literal-identifier lists, `...` ellipses, and nested templates —
all exercised in `eta/test/src/vm_tests.cpp:1992–2090`). The reader,
expander, and module system already cooperate: `examples/portfolio.eta`
defines `dict`, `dict-from`, `report`, `dotimes`, `clamp01`,
`clamp-range`, `list-mean`, `list-variance`, `list-covariance` purely
in user code at lines 68–104 and 807–840. Several of those are
clearly *stdlib-shaped* and have been re-invented across examples.

### Why this matters for the refactor

The earlier per-file inventory called out a recurring pattern: blocks
of mechanically-duplicated `defun`s differing only in which `%`-prim
they call. Every such block is a missed `define-syntax` opportunity.
Concretely:

| Site | Today | With `define-syntax` |
|---|---|---|
| `clpb.eta:76–114` | 5 × `defun clp:and/or/xor/imp/eq` each repeating the `(and (%prim! …) (let ((thunk …)) (attach z) (attach x) (attach y) #t))` skeleton | One `(define-syntax %clpb-binop …)` and 5 one-liner expansions; the prim symbol becomes a syntax argument. **Note:** the per-call thunk closure (`(lambda () (%clp-bool-and! z x y))`) is a *runtime* allocation that the macro does **not** remove — that's a propagator-API design issue, not a syntax one. The macro is purely a verbosity win. |
| `clpr.eta` (mirror set) | Same 4–6× duplication for `clp:r=`/`r<=`/`r<` posters | Same macro family, ensuring visual parity between CLP(B), CLP(Z), and CLP(R). |
| `stats.eta:44–73` | 1-line `(defun stats:foo (xs) (%stats-foo xs))` × ~15 | `(define-syntax %re-export …)` taking a list of names — or, since these are pure aliases, a `(define stats:mean %stats-mean)` block (no macro needed). Pick whichever the runtime treats as zero-cost. |
| `torch.eta:43–60` | Same 1-line aliasing pattern | Same. |
| `db.eta` `defrel`/`tabled` (called as `(defrel '(edge a b))`) | Procedure that dispatches on a quoted clause (`db.eta:223`) | **Add an *additive* surface macro** — e.g. `(define-syntax defrel-clause (syntax-rules () ((_ (head args ...) body ...) (defrel '(head args ...) (lambda ...) ...))))` — that expands into the existing procedure call. **Do not delete the procedural form**: computed-head definitions (`(apply defrel computed-clause)`, dynamic relation registration, REPL experimentation) all rely on it. Documented as "use the macro for static clauses, the procedure for computed ones." This is a *less* aggressive change than the previous draft suggested. |
| Test-framework boilerplate in `std/test.eta` (lines 35–66) | Hand-written `(define-record-type)` + result constructors | Already idiomatic; do *not* macro-ify. Contrast with the `__map`/`__foldl` helpers, which are still procedural and stay so. |

### Idiom guidelines for stdlib macros

- **Use `syntax-rules` only.** No `defmacro`-style escape hatch is
  exposed by the runtime (verified: only `define-syntax` /
  `syntax-rules` appear in test fixtures); preserve hygiene.
- **Name macros like the form they expand to.** A binding macro is
  `defrel`/`tabled`; a control macro is `dotimes`/`while`; a
  declaration macro can omit the `!`/`?` suffix because it is not a
  procedure.
- **Document the literal keywords list.** When a macro uses literal
  identifiers (e.g. `=>` in `examples/portfolio.eta`'s `report`),
  list them in the surrounding `;;;` doc-comment so callers know
  which symbols are part of the surface syntax.
- **Define module-private macros inside `(begin …)` like procedures**;
  export them through `(export …)` exactly as procedures (verified:
  `vm_tests.cpp:2060` exports `double` and uses it from another
  module via `quadruple`).
- **Prefer a macro over a higher-order procedure** when the abstraction
  needs to capture syntax (a binding form, a literal-keyword DSL, or a
  call site that must not allocate a closure on a hot path). Prefer a
  procedure when the abstraction takes only values.
- **Never use a macro to work around the lack of an alias.** Plain
  `(define new-name old-name)` is the right tool for renames.

### Candidates to *promote* from `examples/` into `stdlib/std/`

These already live (re-implemented) in `examples/portfolio.eta` and
should be lifted into the stdlib so other examples stop re-inventing
them:

- `dotimes` — into `std.core` (or a new `std.control`).
- `dict` / `dict-from` — into `std.collections` as `alist` /
  `alist-from` (the `dict` name conflates with a future hash-table
  type).
- `clamp01`, `clamp-range` — into `std.math` as `clamp` (single
  3-arg macro; the 0/1 specialisation collapses to `(clamp x 0 1)`).
- `list-mean` / `list-variance` / `list-covariance` — these are
  *procedures*, not macros; merge into `std.stats` as plain `define`s
  and delete the macro forms in `portfolio.eta`.
- A new `define-aliases` macro for the `stats.eta`/`torch.eta`
  re-export blocks:
  ```scheme
  (define-syntax define-aliases
    (syntax-rules ()
      ((_ (alias original) ...) (begin (define alias original) ...))))
  ```

### Macros to *introduce* during the refactor

- `with-port` / `with-input-port` — a `dynamic-wind`-wrapping macro
  that subsumes Phase 2's `with-output-to-port` rewrite, so callers
  write `(with-port out (println …))` instead of the explicit
  parameter-set/restore pair.
- `match` (small subset) — only if a clear win in `causal.eta`'s
  `(caddr edge)` accessor zoo; otherwise defer.
- `assert-equal?` / `assert-eq?` for `std.test`, replacing the
  procedural `chk` helper that every test file in `stdlib/tests/`
  redefines (`logic_minikanren_combinators.test.eta:24`,
  `clp_propagation_queue.test.eta:30`, etc.).

---

## Per-File Inventory

| File                                                   | Purpose                                     | Priority | Key idiomatic issues                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| ------------------------------------------------------ | ------------------------------------------- | -------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [`prelude.eta`](../stdlib/prelude.eta)                 | Re-export aggregator for the std namespace. | **L**    | Doc comment lists modules but omits `freeze`, `clpb`, `clpr`, `supervisor`, `torch`, `test`. **Either re-export them or explicitly justify their opt-in status** in the doc-comment (consistent with the cross-cutting policy that experimental modules — `torch`, `supervisor`, `freeze` — stay opt-in). Default disposition: `clpb`/`clpr` and `test` should be re-exported (stable, widely used); `freeze`/`supervisor`/`torch` stay opt-in with a one-line justification each.                                                                                                                                                                                |
| [`std/core.eta`](../stdlib/std/core.eta)               | Combinators, accessors, list helpers.       | **M**    | `cond` uses `(#t …)` (lines 74, 92); `iota` uses `letrec`+`lambda` (lines 80–84) — convert to named-let; `last` lacks empty-list error. **`assoc-ref` is kept and promoted** as the canonical lookup helper (Phase 0 rationale: ~129 call sites; deletion would inflate verbosity); just ensure its body is the one-line `(let ((p (assoc k a))) (and p (cdr p)))` form.                                                                                                                                                                                                                                                                                       |
| [`std/math.eta`](../stdlib/std/math.eta)               | Constants and numerical helpers.            | **M**    | `sign` uses `(#t 0)` (line 37); `quotient` (lines 44–54) is a tortured manual truncation — replace with builtin/`floor` composition; `expt` rejects negative exponents silently; `sum`/`product` use `letrec`+`lambda` — convert to named-let or `foldl`.                                                                                                                                                                                                                          |
| [`std/io.eta`](../stdlib/std/io.eta)                   | Print/read/redirect helpers.                | **H**    | `print`/`println` are non-Scheme names (R7RS uses `display`/`newline`); document deviation. Redirect helpers (`with-output-to-port`, etc., lines 76–97) are NOT exception-safe — wrap in `dynamic-wind`. `read-line` uses `letrec`+`lambda` — convert to named-let; `display-to-string` should arguably be named `display->string`.                                                                                                                                                |
| [`std/collections.eta`](../stdlib/std/collections.eta) | List/vector higher-order ops.               | **M**    | `cond` `(#t …)` clauses on lines 67, 92, 99, 156; `map*` / `filter` / `zip` / `pairwise` / `take` / `range` are non-tail-recursive (will blow the stack on long lists) — rewrite with named-let + accumulator + `reverse`; `sort`'s `split` (lines 158–163) is an ad-hoc deal — replace with a clearer halve/merge; `reduce` (line 82) raises on empty — accept SRFI-1 `(reduce f ridentity xs)` signature; `flatten` (line 137) is shallow — rename to `concat` or `append*`.     |
| [`std/logic.eta`](../stdlib/std/logic.eta)             | Prolog/miniKanren combinators.              | **L**    | Largely idiomatic. Minor: `naf`/`succeeds?`/`findall`/`run-n` already use `let*` correctly; consider `let loop` for `findall` (line 100) and `run-n` (line 297) instead of explicit recursion to make the trail-unwind contract visually obvious. `condu` (line 268) is a one-liner — fine.                                                                                                                                                                                        |
| [`std/clp.eta`](../stdlib/std/clp.eta)                 | CLP(Z) / CLP(FD).                           | **M**    | Domain accessors (`clp:domain-lo`/`clp:domain-hi`, lines 82–99) cry out for `case` on `(car dom)`; `%clp-member?` (line 118) duplicates `member` — call the builtin; export list mixes `clp:<>` and `clp:!=` (line 59). Drop `clp:!=` from primary exports; add the deprecated alias `(define clp:!= clp:<>)` **inside `std/clp.eta`** under the `;; ── deprecated aliases ──` header (no `(rename …)` shim in `prelude.eta`).                                                                                                                                                                                       |
| [`std/clpb.eta`](../stdlib/std/clpb.eta)               | CLP(B) Boolean propagators.                 | **L→M**  | Already the cleanest file (use as style baseline). The five `clp:and/or/xor/imp/eq` defuns (lines 76–114) are now a clear `define-syntax` site — see Macros §; promote priority because the macro is a worked example for `clp.eta` / `clpr.eta`.                                                                                                                                                                                                                       |
| [`std/clpr.eta`](../stdlib/std/clpr.eta)               | CLP over reals.                             | **M**    | Same `case`-vs-`cond` opportunity for domain accessors; check that the `clp:r=`/`clp:r<=`/`clp:r<` family follows the same `(and (post!) (attach-thunks))` shape as `clpb.eta` for visual parity.                                                                                                                                                                                                                                                                                  |
| [`std/causal.eta`](../stdlib/std/causal.eta)           | Causal DAGs + do-calculus.                  | **M**    | `dag:nodes` (lines 59–67) uses an `all-nodes` let-binding only to return it — drop the let; `%dag-edge-to` (line 56) — extract `(caddr edge)` once defined, or use `match`-style helpers. Many `cond` chains likely have `(#t …)` fallbacks (verify); also some predicates (`dag:has-path?`) rely on shadowing globals.                                                                                                                                                            |
| [`std/fact_table.eta`](../stdlib/std/fact_table.eta)   | Wrappers for builtin columnar tables.       | **H**    | `(define (%ft-pred x) (fact-table? x))` then `(defun fact-table? …)` (lines 46–47) is a confusing self-shadowing — just `(define fact-table? %fact-table?)` or import-rename the builtin; `fact-table-row` (lines 88–96) hard-codes `ncols-approx 64` and silently swallows out-of-bounds errors — needs a real `%fact-table-col-count` builtin (or, pending that, a documented `fact-table-row ft row col-count` arity). Mixes `defun` and `define` — pick one.                   |
| [`std/db.eta`](../stdlib/std/db.eta)                   | Relations on top of fact tables.            | **H**    | Exports `assert` and `retract` which collide with conventional assertion macros — rename to **`assert-fact!` / `retract-fact!`** (canonical; see Scope policy); huge alist of mutable globals (`*db-registry*` etc., lines 22–32) — encapsulate; `make-col-names` (lines 107–115) uses `letrec`+`lambda` for what's an `(iota arity)` + `map` over `string->symbol`; **`defrel` and `tabled` are procedures (`db.eta:223`) called with quoted lists (`(defrel '(edge a b))` — see `tests/db.test.eta:15`); add an *additive* `defrel-clause` / `tabled-clause` sugar macro for static call sites and KEEP the procedural form for computed-head / `apply`-style use (no migration of the quoted form is required or planned).** `cond (#t …)` on lines 40, 50, 57. |
| [`std/freeze.eta`](../stdlib/std/freeze.eta)           | Attributed-var freeze + dif.                | **L**    | `%freeze-hook` (lines 40–49) good; `(register-attr-hook! 'freeze %freeze-hook)` runs at module body — keep but document the load-order dependency on `std.logic`.                                                                                                                                                                                                                                                                                                                  |
| [`std/stats.eta`](../stdlib/std/stats.eta)             | Statistical primitives.                     | **M**    | Almost every export is a 1-line `(defun stats:foo (xs) (%stats-foo xs))` (lines 44–73) — collapse via `(define stats:mean %stats-mean)` to make the table-of-aliases nature obvious. Accessor stack `(car (cdr (cdr (cdr (cdr (cdr r))))))` (lines 78–80) is unreadable — use `list-ref` or `define-record-type` for OLS results.                                                                                                                                                  |
| [`std/net.eta`](../stdlib/std/net.eta)                 | High-level NNG/actor patterns.              | **L**    | `with-socket` (line 45) — uses a `(make-vector 1 #f)` mutable box; comment is excellent. Otherwise idiomatic. Confirm `worker-pool` and `survey` use `dynamic-wind`.                                                                                                                                                                                                                                                                                                               |
| [`std/supervisor.eta`](../stdlib/std/supervisor.eta)   | Erlang-style sup trees.                     | **M**    | `start-all` letrec layout (line 56+) uses outer `letrec` + inner `let loop` — reduce nesting; `down-message?` (line 39) is an idiomatic predicate, good.                                                                                                                                                                                                                                                                                                                           |
| [`std/test.eta`](../stdlib/std/test.eta)               | Test framework.                             | **H**    | `__make-result`, `__make-group-result`, `__make-summary`, `__map`, `__foldl` (lines 48–78) — rename to `%`-prefixed; the `__map`/`__foldl` re-implementations exist to avoid importing `std.collections` (creating a dep cycle?) — document or break the cycle. `define-record-type` usage (lines 35–66) is correctly idiomatic.                                                                                                                                                   |
| [`std/torch.eta`](../stdlib/std/torch.eta)             | libtorch wrappers.                          | **L**    | Most exports are 1-line aliases (lines 43–60) — collapse via `define`-as-alias to make the wrapping nature literal; `t+`/`t-`/`t*`/`t/` arithmetic shadowing avoidance is OK but document why the `tensor-` prefix wasn't chosen.                                                                                                                                                                                                                                                  |

---

## Phased Refactor Plan

### Phase 0 — Deletion & aliasing (highest verbosity payoff, do first)

Pure simplification of duplication and naming. No structural rewriting.

- [ ] **Delete `%clp-member?`** (`clp.eta:118`); call the builtin
      `member` (or `memv` for fixnums) at the use sites.
- [ ] **Keep `assoc-ref`.** Earlier draft proposed deletion in
      favour of `(let ((p (assoc k a))) (and p (cdr p)))`. Code
      audit (~129 occurrences across `stdlib/std/`, `stdlib/tests/`,
      `examples/`, dominated by `examples/portfolio.eta`) shows
      deletion would *increase* call-site verbosity sharply and run
      counter to the simplification goal. Promote `assoc-ref` to the canonical
      lookup helper and document it in the style guide; do **not**
      reimplement it locally in callers.
- [ ] **Collapse `stats.eta:44–73`** to a single
      `(define-aliases (stats:mean %stats-mean) …)` block — see
      Macros §. Same for `torch.eta:43–60`.
- [ ] **Collapse `fact_table.eta:46–47`** self-shadowing
      `%ft-pred`/`fact-table?` dance to `(define fact-table?
      %fact-table?)`.
- [ ] **Drop `clp:!=`** export entirely (it's a duplicate of
      `clp:<>`); add a one-line deprecated alias in the same file
      under a `;; deprecated` comment for one release.
- [ ] **Promote canonical names; demote duplicates to aliases in
      their owning modules** (see idiom #5 for the keep-vs-demote
      rationale). Concretely:
      - In `std.io`: canonical `display` / `newline` / `write` /
        `display->string`; demote `print` and `display-to-string`
        to one-line `(define print display)` /
        `(define display-to-string display->string)` aliases under a
        `;; ── deprecated aliases ──` header. **Keep** `println`
        and `eprintln` as non-deprecated Eta extensions (they
        bundle `display`+`newline`, which is genuine value).
      - In `std.db`: canonical `assert-fact!` / `retract-fact!`;
        demote `assert` / `retract` to one-line aliases under the
        deprecated header **inside `std.db`** (not `std.io`).
      - In `std.clp`: canonical `clp:<>`; demote `clp:!=` to a
        one-line alias under the deprecated header **inside
        `std.clp`**.
      - In **none of the above** does the alias appear in
        `std.prelude`'s primary export block. Prelude remains focused:
        aliases belong in owning modules.
- [ ] **Remove `__map`/`__foldl`** in `std/test.eta` if the
      dependency cycle that motivated them no longer exists; import
      `std.collections` instead. (If the cycle is real, just rename
      `__` → `%` per Phase 1; document the cycle.)
- [ ] **Audit `(import …)` lines** for unused imports and remove
      (e.g., `causal.eta` may import `std.io` without using it).
- [ ] **Phase-0 review gate:** duplicate helpers removed/collapsed,
      canonical-vs-extension naming policy applied, deprecated aliases
      localized to owning modules, and no call-site verbosity regressions
      in touched examples/tests.

### Phase 1 — Quick Wins (mechanical, low-risk)

- [ ] Replace every `(#t …)` cond fallback with `(else …)` across all
  
      files in `stdlib/std/` (verified offenders: `core.eta`,
      `math.eta`, `collections.eta`, `db.eta`, `causal.eta`).
- [ ] Rename `__`-prefixed helpers in `std/test.eta` to `%`.
- [ ] Add missing `;; ──` section headers in `db.eta`,
  
      `supervisor.eta`, `clpr.eta`, `freeze.eta`.
- [ ] Drop the redundant `clp:!=` export (keep `clp:<>` per Scheme
  
      tradition; if back-compat is needed, the deprecated alias
      `(define clp:!= clp:<>)` lives **inside `std/clp.eta`** under
      a `;; ── deprecated aliases ──` header. **Not** in
      `prelude.eta` (per the global alias-placement policy in the
      Scope section).
- [ ] Update [`prelude.eta`](../stdlib/prelude.eta) header doc-comment
  
      list to include all loaded modules; decide & document whether
      `std.freeze`/`std.clpb`/`std.clpr`/`std.supervisor`/`std.torch`/
      `std.test` should be re-exported.
- [ ] Replace `(define (%ft-pred x) (fact-table? x))` shadowing dance
  
      in `fact_table.eta:46–47` with a single rename-import or
      `(define fact-table? %fact-table?)`.

### Phase 2 — Structural Refactors

- [ ] Convert `letrec`+`lambda` loop pattern to **named-let** in:
  
      `core.eta` (`iota`), `math.eta` (`sum`, `product`), `io.eta`
      (`read-line`), `collections.eta` (`map-indexed`, `sort.msort`,
      `sort.split`, all `vector-*`), `fact_table.eta` (`fact-table-row`,
      `fact-table-for-each`, `fact-table-fold`), `db.eta`
      (`make-col-names`, `tabled-rel?`), `supervisor.eta` (`start-all`).
- [ ] Make non-tail-recursive list builders **tail-recursive** with an
  
      accumulator + final `reverse`: `map*`, `map2`, `filter`, `zip`,
      `pairwise`, `take`, `range`, `flatten` in `collections.eta`;
      `last` in `core.eta`; `fresh-vars` in `logic.eta`.
      *(`assoc-ref` is unchanged — it is already a one-line
      `assoc`+`cdr` wrapper per Phase 0; not a tail-rec target.)*
- [ ] Wrap `with-output-to-port` / `with-input-from-port` /
  
      `with-error-to-port` in `io.eta` with `dynamic-wind` for
      exception/escape safety, mirroring `with-socket` in `net.eta`.
- [ ] Rewrite `clp:domain-lo` / `clp:domain-hi` /
  
      `clp:domain-values` / `clp:domain-empty?` in `clp.eta` and the
      mirror set in `clpr.eta` to dispatch with `case` on
      `(car dom)`.
- [ ] Pick one of `defun` or `(define (f …))` per file and apply
  
      uniformly. Recommendation: prefer `(define (f …))` everywhere
      for R5RS portability; reserve `defun` for variadic
      `(defun f args …)` cases until/unless we add a docstring story.
- [ ] Encapsulate the `*db-*` global alists in `db.eta` behind
  
      `%registry-{lookup,put!,remove!}` accessors; replace `set!` calls
      with `%registry-put!`.

### Phase 3 — API Polish & Surface Cleanup

- [ ] Add `define-record-type` for OLS / t-test result tuples in
  
      `stats.eta`, replacing `(car (cdr (cdr (cdr …))))` accessor
      stacks with `ols-slope` / `ols-intercept` / etc. record fields
      (preserves names; changes representation).
- [ ] Replace `%clp-member?` (`clp.eta:118`) with the builtin `member`
  
      (or `memv` for fixnums). Drop the helper.
- [ ] Replace ad-hoc `flatten` (shallow) with a clearly-named
  
      `concat` (alias `flatten` for back-compat); add a separate
      `flatten-deep` if needed.
- [ ] Rename `display-to-string` → `display->string`; export both for
  
      one release with a deprecation note.
- [ ] Rename `assert` / `retract` in `db.eta` → `assert-fact!` /
  
      `retract-fact!` (canonical — see idiom #5 rationale box);
      keep `assert` / `retract` as one-line deprecated aliases in
      `std.db` under the `;; ── deprecated aliases ──` header for
      one release. Not re-exported from `std.prelude`.
- [ ] Collapse the per-primitive `defun` aliases in `stats.eta` and
  
      `torch.eta` to a single `(define stats:mean %stats-mean)` style
      block (or, if a uniform syntax helps the reader, a
      `(define-syntax define-aliases …)` macro from the
      [Macros §](#macros-define-syntax)). Comment-document why
      aliasing (not re-defun) is safe (top-level binding capture).
- [ ] **Macro conversions** (see [Macros §](#macros-define-syntax)):
      (a) factor the 5 `clp:and/or/xor/imp/eq` defuns in `clpb.eta`
      and the mirror set in `clpr.eta` into a `%clp-binop` macro
      (verbosity win only — does *not* eliminate the per-call thunk
      allocation, which is a separate propagator-API concern);
      (b) **add** a `defrel-clause` / `tabled-clause` syntactic-sugar
      macro in `db.eta` that expands into the existing procedural
      `defrel` / `tabled`. **Keep** the procedures: `db.eta:212–229`
      shows they are needed for computed-head and `apply`-style
      registration. Update `tests/db.test.eta` to cover both call
      shapes (quoted procedure call *and* sugared macro);
      (c) introduce a `with-port` macro in `std.io` that captures the
      `dynamic-wind` pattern from Phase 2 above;
      (d) promote `dotimes`, `clamp`, `dict`/`alist` from
      `examples/portfolio.eta` into the appropriate stdlib module.

---

## Cross-Cutting Concerns

### Naming conventions

- Predicates: `name?` (`atom?`, `even?`, `tabled-rel?`).
- Mutators: `name!` (`fact-table-insert!`, `assert-fact!`).
- Conversions: `from->to` (`vector->list`, `display->string`).
- Internal helpers: `%name` (kebab-case), defined inside the same
  `(begin …)` and **not** exported.
- Namespaced public names use `module:name` (already the convention
  for `clp:`, `dag:`, `do:`, `stats:`); consider whether `dag:` and
  `do:` should remain colon-prefixed given they live in `std.causal`
  (the colon adds value because `do` would shadow a control form).

### Module/export style

- One `(export …)` form per module, grouped with `;; ──` comments by
  category, exactly as `clpb.eta` and `clp.eta` already do.
- No `(import …)` of a module that is not actually used in the
  body (audit: `causal.eta` imports `std.io` — verify usage).
- Re-exports through `std.prelude` only for stable APIs; experimental
  modules (`torch`, `supervisor`, `freeze`) stay opt-in.

### Doc-comment format

```scheme
;;; std/foo.eta — One-line purpose.
;;;
;;; Longer prose paragraph(s) describing what this module provides,
;;; what builtins it depends on, and any cross-module load-order notes.
;;; If the module exports macros, list their literal-keyword identifiers
;;; here (e.g. "`with-port` recognises the literal `=>` separator").
;;;
;;; Exports:
;;;   (foo a b)         ; one-line summary
;;;   (bar! v)          ; …
;;;   (with-port p ...) ; macro: binds current port for the duration
```

Per-definition, use `;; description` immediately above. Avoid block
comments inside `(begin …)` that duplicate the export-block prose.

### Error / contract idioms

- Use `(error "module:proc — message" arg)` consistently for
  precondition violations (current style in `collections.eta:84`).
- Goal-style procedures return `#t` / `#f`; never `(error …)` on
  unification failure (Prolog semantics).
- For records, prefer `(define-record-type)` predicates over manual
  `(and (vector? x) (eq? (vector-ref x 0) 'tag))` — see
  `std/test.eta` as the model.
- For binding-form macros, use the `syntax-rules` literal-keyword
  list to reject malformed call sites at expansion time rather than
  re-checking shape inside the expanded body.

### Test alignment

- After every renamed export, update the corresponding file in
  `stdlib/tests/*.test.eta`. Map of expected impact:
  - `db.eta` rename (`assert` → `assert-fact!`, `retract` →
    `retract-fact!`) → `tests/db.test.eta`; the additive
    `defrel-clause` / `tabled-clause` macros add **new** test
    coverage but do **not** require modifying the existing
    `(defrel '(...))` procedural call sites — those remain
    valid and supported.
  - `flatten` → `concat` → `tests/collections.test.eta`.
  - `display-to-string` → `display->string` →
    `tests/test-framework.test.eta` if used.
  - Promotion of `dotimes` / `clamp` etc. into the stdlib →
    delete the per-example macro definitions in `examples/portfolio.eta`
    (lines 95–104, 807–840) once the imports are wired.
- Add a new test group `idioms` per module that asserts re-exported
  alias names still resolve, to catch accidental removal.
- Add a small fixture under `stdlib/tests/macros.test.eta` that
  exercises every newly-introduced `define-syntax` form so a future
  expander regression is caught early.

---

## Risks & Compatibility

| Change | Breaking? | Mitigation |
|---|---|---|
| `(#t …)` → `(else …)` in `cond` | No | `else` already supported. |
| `letrec`+`lambda` → named-`let` | No | Pure refactor; same scoping. |
| Tail-recursive list builders | Order-preserving (with `reverse`) | Add tests asserting output order. |
| Drop `clp:!=` in favour of `clp:<>` | **Yes** | One-line `(define clp:!= clp:<>)` deprecated alias **inside `std/clp.eta`**; not re-exported from `std.prelude` (per the alias-placement policy). |
| Rename `assert` → `assert-fact!` | **Yes** | Keep `assert` alias for one release inside `std.db`; not re-exported from `std.prelude`. |
| `flatten` semantic clarification | Possibly | Keep current shallow `flatten`; add `flatten-deep`. |
| `display-to-string` → `display->string` | **Yes** | Export both names for one release. |
| OLS result tuple → record | **Yes** for direct `car`/`cdr` users | Existing accessors keep working. |
| `clpb.eta` `defun` posters → `define-syntax` macro | No (call sites unchanged) | Macro produces identical top-level bindings; verify with the existing CLP(B) test suite. The per-call thunk closure is unaffected (see Macros §). |
| `defrel` / `tabled` *additive* macro alongside the procedure | No (procedure stays; macro is new sugar) | Document the split: macro for static clauses, procedure for computed-head / `apply`-based registration (`db.eta:212–229`). Update `tests/db.test.eta` to cover *both* call shapes. |
| New `with-port` macro on top of `with-output-to-port` proc | No (additive) | Old procedure stays; macro expands to it. |
| Promote `dotimes`/`clamp`/`alist` from examples into stdlib | No for the example (it stops re-defining); **Yes** if any other example shadowed the same name | Grep `examples/**/*.eta` before landing each promotion. |
| **Promote R7RS names to canonical (`print`→`display`, `assert`→`assert-fact!`, `retract`→`retract-fact!`, `display-to-string`→`display->string`, `clp:!=`→`clp:<>`)** | **Yes** — call sites must be updated; aliases remain for one release | Update every `examples/*.eta` and `stdlib/tests/*.test.eta` in the same PR; deprecated aliases stay in their owning module (`std.io`, `std.db`, `std.clp`), **not** in `std.prelude`'s primary block, so the prelude shrinks. `println`/`eprintln` stay as Eta-extension exports (not deprecated). |
| Adopting `define-syntax` more broadly | **Possible expander/LSP edge cases** | The VM tests cover hygienic capture (`vm_tests.cpp:1992–2090`) but the LSP may not yet treat user macros as binding forms (cf. `lsp_tests.cpp:422`); file LSP follow-ups rather than block the refactor. |

Deprecation strategy: alias old → new for **one minor release**, list
each alias in a `## Deprecated` section of `release-notes.md`, remove
in the release after.

---

## Success Criteria

1. Refactored modules are **materially less verbose and more idiomatically
   Scheme-like** in review: repetitive wrapper blocks are collapsed,
   duplicate helpers are removed or minimized, and canonical naming
   policy is applied. This is judged by before/after code review, **not**
   by a line-count quota.
2. `stdlib/prelude.eta`'s primary export block does not grow;
   deprecated aliases live in their owning module under a
   `;; ── deprecated aliases ──` header, **not** in `prelude.eta`.
3. `stdlib/tests/*.test.eta` all pass with no test removed.
4. Every `examples/*.eta` file still runs under `etai` (after
   updating call sites for the canonical names —
   `display`/`newline`/`assert-fact!`/`retract-fact!`/`clp:<>`/
   `display->string`). `println` and `eprintln` remain valid
   (kept as Eta extensions).
5. `grep -rn '(#t ' stdlib/std/` returns zero matches.
6. `grep -rnE 'letrec\s*\(\(\w+\s*\(lambda' stdlib/std/` returns zero
   matches in files targeted by Phase 2.
7. `grep -n '__' stdlib/std/test.eta` returns zero matches.
8. `prelude.eta` doc-comment matches the actual `(import …)` list.
9. `stdlib/std/clpb.eta` remains the style baseline — the new
   `%clp-binop` macro becomes the *new* baseline for `clp.eta` and
   `clpr.eta`.
10. **Both** `(defrel '(edge a b))` (procedure call) **and**
    `(defrel-clause (edge a b))` (sugar macro) work and have test
    coverage in `tests/db.test.eta` — the procedural form is *not*
    removed.
11. `stdlib/tests/macros.test.eta` exists and exercises every
    stdlib-exported `define-syntax` form.
12. `docs/modules.md` reference table is updated for any rename, and
    a new `docs/stdlib_style.md` (extracted from §"Scheme Idiom
    Guidelines" + §"Macros" of this plan) is added so the convention
    survives future contributions.
13. **Deprecated aliases** — `print` / `display-to-string` /
    `assert` / `retract` / `clp:!=` are present *only* as
    one-line `(define <old> <new>)` aliases in their owning
    modules (`std.io`, `std.db`, `std.clp`), under a
    `;; ── deprecated aliases ──` header with a
    `;; deprecated — remove in vX.Y` comment. None appear in
    `stdlib/prelude.eta`'s primary export block. `println` and
    `eprintln` are **not** in this list — they are retained
    Eta extensions, documented as such in the `std.io` header.


