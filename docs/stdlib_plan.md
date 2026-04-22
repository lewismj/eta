# Eta Standard Library — Idiomatic Scheme Refactor Plan

## Overview & Goals

The Eta stdlib (`stdlib/prelude.eta` + `stdlib/std/*.eta`) is functionally
solid but stylistically heterogeneous: some files use `defun`, others use
`(define (f ...))`; `cond` fallbacks alternate between `(#t …)` and
`(else …)`; tail loops sometimes use `letrec`+`lambda`, sometimes
named-`let`; helpers are variously prefixed `%`, `__`, or unprefixed.
This plan brings the surface syntax closer to idiomatic R5RS/R7RS +
SRFI-1/13/69 conventions while preserving every public name, semantic,
and the existing `(module … (export …) (begin …))` layout documented in
[`docs/modules.md`](modules.md).

Non-goals: changing the module system or breaking the public API of
`std.prelude`.

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
5. **Conversions use `->`** (`vector->list`, `list->vector`,
   `display-to-string` → rename to `display->string` for symmetry; the
   `do:adjustment-formula->string` form is already correct).
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
| `clpb.eta:76–114` | 5 × `defun clp:and/or/xor/imp/eq` each repeating the `(and (%prim! …) (let ((thunk …)) (attach z) (attach x) (attach y) #t))` skeleton | One `(define-syntax %clpb-binop …)` and 5 one-liner expansions; the prim symbol becomes a syntax argument. Eliminates ~30 LOC and removes the closure-allocation per call. |
| `clpr.eta` (mirror set) | Same 4–6× duplication for `clp:r=`/`r<=`/`r<` posters | Same macro family, ensuring visual parity between CLP(B), CLP(Z), and CLP(R). |
| `stats.eta:44–73` | 1-line `(defun stats:foo (xs) (%stats-foo xs))` × ~15 | `(define-syntax %re-export …)` taking a list of names — or, since these are pure aliases, a `(define stats:mean %stats-mean)` block (no macro needed). Pick whichever the runtime treats as zero-cost. |
| `torch.eta:43–60` | Same 1-line aliasing pattern | Same. |
| `db.eta` `defrel`/`tabled` (called as `(defrel '(edge a b))`) | Procedure that `eval`s a quoted clause | True macro: `(define-syntax defrel (syntax-rules () ((_ (head args ...) body ...) …)))` — drops the quote at every call site, gives proper hygiene for fresh logic vars, and lets the LSP/highlighter see `defrel` as a binding form. **Highest-impact macro conversion in the stdlib.** |
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
| [`prelude.eta`](../stdlib/prelude.eta)                 | Re-export aggregator for the std namespace. | **L**    | Doc comment lists modules but omits `freeze`, `clpb`, `clpr`, `supervisor`, `torch`, `test`. Update list and re-export them (or document why they're excluded).                                                                                                                                                                                                                                                                                                                    |
| [`std/core.eta`](../stdlib/std/core.eta)               | Combinators, accessors, list helpers.       | **M**    | `cond` uses `(#t …)` (lines 74, 92); `iota` uses `letrec`+`lambda` (lines 80–84) — convert to named-let; `last` lacks empty-list error; `assoc-ref` is reinventing SRFI-1 `assoc` — wrap the builtin instead.                                                                                                                                                                                                                                                                      |
| [`std/math.eta`](../stdlib/std/math.eta)               | Constants and numerical helpers.            | **M**    | `sign` uses `(#t 0)` (line 37); `quotient` (lines 44–54) is a tortured manual truncation — replace with builtin/`floor` composition; `expt` rejects negative exponents silently; `sum`/`product` use `letrec`+`lambda` — convert to named-let or `foldl`.                                                                                                                                                                                                                          |
| [`std/io.eta`](../stdlib/std/io.eta)                   | Print/read/redirect helpers.                | **H**    | `print`/`println` are non-Scheme names (R7RS uses `display`/`newline`); document deviation. Redirect helpers (`with-output-to-port`, etc., lines 76–97) are NOT exception-safe — wrap in `dynamic-wind`. `read-line` uses `letrec`+`lambda` — convert to named-let; `display-to-string` should arguably be named `display->string`.                                                                                                                                                |
| [`std/collections.eta`](../stdlib/std/collections.eta) | List/vector higher-order ops.               | **M**    | `cond` `(#t …)` clauses on lines 67, 92, 99, 156; `map*` / `filter` / `zip` / `pairwise` / `take` / `range` are non-tail-recursive (will blow the stack on long lists) — rewrite with named-let + accumulator + `reverse`; `sort`'s `split` (lines 158–163) is an ad-hoc deal — replace with a clearer halve/merge; `reduce` (line 82) raises on empty — accept SRFI-1 `(reduce f ridentity xs)` signature; `flatten` (line 137) is shallow — rename to `concat` or `append*`.     |
| [`std/logic.eta`](../stdlib/std/logic.eta)             | Prolog/miniKanren combinators.              | **L**    | Largely idiomatic. Minor: `naf`/`succeeds?`/`findall`/`run-n` already use `let*` correctly; consider `let loop` for `findall` (line 100) and `run-n` (line 297) instead of explicit recursion to make the trail-unwind contract visually obvious. `condu` (line 268) is a one-liner — fine.                                                                                                                                                                                        |
| [`std/clp.eta`](../stdlib/std/clp.eta)                 | CLP(Z) / CLP(FD).                           | **M**    | Domain accessors (`clp:domain-lo`/`clp:domain-hi`, lines 82–99) cry out for `case` on `(car dom)`; `%clp-member?` (line 118) duplicates `member` — call the builtin; export list mixes `clp:<>` and `clp:!=` (line 59) which are duplicates — collapse and add `(rename …)` for back-compat.                                                                                                                                                                                       |
| [`std/clpb.eta`](../stdlib/std/clpb.eta)               | CLP(B) Boolean propagators.                 | **L→M**  | Already the cleanest file (use as style baseline). The five `clp:and/or/xor/imp/eq` defuns (lines 76–114) are now a clear `define-syntax` site — see Macros §; promote priority because the macro is a worked example for `clp.eta` / `clpr.eta`.                                                                                                                                                                                                                       |
| [`std/clpr.eta`](../stdlib/std/clpr.eta)               | CLP over reals.                             | **M**    | Same `case`-vs-`cond` opportunity for domain accessors; check that the `clp:r=`/`clp:r<=`/`clp:r<` family follows the same `(and (post!) (attach-thunks))` shape as `clpb.eta` for visual parity.                                                                                                                                                                                                                                                                                  |
| [`std/causal.eta`](../stdlib/std/causal.eta)           | Causal DAGs + do-calculus.                  | **M**    | `dag:nodes` (lines 59–67) uses an `all-nodes` let-binding only to return it — drop the let; `%dag-edge-to` (line 56) — extract `(caddr edge)` once defined, or use `match`-style helpers. Many `cond` chains likely have `(#t …)` fallbacks (verify); also some predicates (`dag:has-path?`) rely on shadowing globals.                                                                                                                                                            |
| [`std/fact_table.eta`](../stdlib/std/fact_table.eta)   | Wrappers for builtin columnar tables.       | **H**    | `(define (%ft-pred x) (fact-table? x))` then `(defun fact-table? …)` (lines 46–47) is a confusing self-shadowing — just `(define fact-table? %fact-table?)` or import-rename the builtin; `fact-table-row` (lines 88–96) hard-codes `ncols-approx 64` and silently swallows out-of-bounds errors — needs a real `%fact-table-col-count` builtin (or, pending that, a documented `fact-table-row ft row col-count` arity). Mixes `defun` and `define` — pick one.                   |
| [`std/db.eta`](../stdlib/std/db.eta)                   | Relations on top of fact tables.            | **H**    | Exports `assert` and `retract` which collide with conventional assertion macros — rename to `db-assert!` / `db-retract!` (or `assert-fact!`); huge alist of mutable globals (`*db-registry*` etc., lines 22–32) — encapsulate; `make-col-names` (lines 107–115) uses `letrec`+`lambda` for what's an `(iota arity)` + `map` over `string->symbol`; **`defrel` and `tabled` are procedures called with quoted lists (`(defrel '(edge a b))` — see `tests/db.test.eta:15`); convert to `define-syntax` so call sites lose the quote and the LSP recognises them as binding forms.** `cond (#t …)` on lines 40, 50, 57. |
| [`std/freeze.eta`](../stdlib/std/freeze.eta)           | Attributed-var freeze + dif.                | **L**    | `%freeze-hook` (lines 40–49) good; `(register-attr-hook! 'freeze %freeze-hook)` runs at module body — keep but document the load-order dependency on `std.logic`.                                                                                                                                                                                                                                                                                                                  |
| [`std/stats.eta`](../stdlib/std/stats.eta)             | Statistical primitives.                     | **M**    | Almost every export is a 1-line `(defun stats:foo (xs) (%stats-foo xs))` (lines 44–73) — collapse via `(define stats:mean %stats-mean)` to make the table-of-aliases nature obvious. Accessor stack `(car (cdr (cdr (cdr (cdr (cdr r))))))` (lines 78–80) is unreadable — use `list-ref` or `define-record-type` for OLS results.                                                                                                                                                  |
| [`std/net.eta`](../stdlib/std/net.eta)                 | High-level NNG/actor patterns.              | **L**    | `with-socket` (line 45) — uses a `(make-vector 1 #f)` mutable box; comment is excellent. Otherwise idiomatic. Confirm `worker-pool` and `survey` use `dynamic-wind`.                                                                                                                                                                                                                                                                                                               |
| [`std/supervisor.eta`](../stdlib/std/supervisor.eta)   | Erlang-style sup trees.                     | **M**    | `start-all` letrec layout (line 56+) uses outer `letrec` + inner `let loop` — reduce nesting; `down-message?` (line 39) is an idiomatic predicate, good.                                                                                                                                                                                                                                                                                                                           |
| [`std/test.eta`](../stdlib/std/test.eta)               | Test framework.                             | **H**    | `__make-result`, `__make-group-result`, `__make-summary`, `__map`, `__foldl` (lines 48–78) — rename to `%`-prefixed; the `__map`/`__foldl` re-implementations exist to avoid importing `std.collections` (creating a dep cycle?) — document or break the cycle. `define-record-type` usage (lines 35–66) is correctly idiomatic.                                                                                                                                                   |
| [`std/torch.eta`](../stdlib/std/torch.eta)             | libtorch wrappers.                          | **L**    | Most exports are 1-line aliases (lines 43–60) — collapse via `define`-as-alias to make the wrapping nature literal; `t+`/`t-`/`t*`/`t/` arithmetic shadowing avoidance is OK but document why the `tensor-` prefix wasn't chosen.                                                                                                                                                                                                                                                  |

---

## Phased Refactor Plan

### Phase 1 — Quick Wins (mechanical, low-risk)

- [ ] Replace every `(#t …)` cond fallback with `(else …)` across all
  
      files in `stdlib/std/` (verified offenders: `core.eta`,
      `math.eta`, `collections.eta`, `db.eta`, `causal.eta`).
- [ ] Rename `__`-prefixed helpers in `std/test.eta` to `%`.
- [ ] Add missing `;; ──` section headers in `db.eta`,
  
      `supervisor.eta`, `clpr.eta`, `freeze.eta`.
- [ ] Drop the redundant `clp:!=` export (keep `clp:<>` per Scheme
  
      tradition; add `(import (rename std.clp (clp:<> clp:!=)))` shim
      in prelude if back-compat needed).
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
      `last`/`assoc-ref` in `core.eta`; `fresh-vars` in `logic.eta`.
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
- [ ] Rename `assert` / `retract` in `db.eta` → `db-assert!` /
  
      `db-retract!` / `db-retract-all!`; keep old names as aliases for
      one release.
- [ ] Collapse the per-primitive `defun` aliases in `stats.eta` and
  
      `torch.eta` to a single `(define stats:mean %stats-mean)` style
      block (or, if a uniform syntax helps the reader, a
      `(define-syntax define-aliases …)` macro from the
      [Macros §](#macros-define-syntax)). Comment-document why
      aliasing (not re-defun) is safe (top-level binding capture).
- [ ] **Macro conversions** (see [Macros §](#macros-define-syntax)):
      (a) factor the 5 `clp:and/or/xor/imp/eq` defuns in `clpb.eta`
      and the mirror set in `clpr.eta` into a `%clp-binop` macro;
      (b) convert `defrel` / `tabled` in `db.eta` from quoted-list
      procedures to `define-syntax` binding forms (update
      `tests/db.test.eta` to drop the leading quote);
      (c) introduce a `with-port` macro in `std.io` that captures the
      `dynamic-wind` pattern from Phase 2 above;
      (d) promote `dotimes`, `clamp`, `dict`/`alist` from
      `examples/portfolio.eta` into the appropriate stdlib module.

---

## Cross-Cutting Concerns

### Naming conventions

- Predicates: `name?` (`atom?`, `even?`, `tabled-rel?`).
- Mutators: `name!` (`fact-table-insert!`, `db-assert!`).
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
  - `db.eta` renames + `defrel`/`tabled` macro conversion →
    `tests/db.test.eta` (drop the leading `'` on every `defrel` /
    `tabled` call site — see lines 15–17, 59–60, 73–74, 80–81).
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
| Drop `clp:!=` in favour of `clp:<>` | **Yes** | Keep alias via `(rename …)` for one release. |
| Rename `assert` → `db-assert!` | **Yes** | Keep `assert` alias for one release. |
| `flatten` semantic clarification | Possibly | Keep current shallow `flatten`; add `flatten-deep`. |
| `display-to-string` → `display->string` | **Yes** | Export both names for one release. |
| OLS result tuple → record | **Yes** for direct `car`/`cdr` users | Existing accessors keep working. |
| Replace `print`/`println` with R7RS `display`/`newline` | **No** (don't do it) | Keep current names; document deviation. |
| `clpb.eta` `defun` posters → `define-syntax` macro | No (call sites unchanged) | Macro produces identical top-level bindings; verify with the existing CLP(B) test suite. |
| `defrel` / `tabled` procedure → macro | **Yes** (call sites lose the leading `'`) | Land the macro and the `tests/db.test.eta` edit in the same commit; keep the procedural form under `%defrel-proc` for one release for any out-of-tree caller. |
| New `with-port` macro on top of `with-output-to-port` proc | No (additive) | Old procedure stays; macro expands to it. |
| Promote `dotimes`/`clamp`/`alist` from examples into stdlib | No for the example (it stops re-defining); **Yes** if any other example shadowed the same name | Grep `examples/**/*.eta` before landing each promotion. |
| Adopting `define-syntax` more broadly | **Possible expander/LSP edge cases** | The VM tests cover hygienic capture (`vm_tests.cpp:1992–2090`) but the LSP may not yet treat user macros as binding forms (cf. `lsp_tests.cpp:422`); file LSP follow-ups rather than block the refactor. |

Deprecation strategy: alias old → new for **one minor release**, list
each alias in a `## Deprecated` section of `release-notes.md`, remove
in the release after.

---

## Success Criteria

1. `stdlib/tests/*.test.eta` all pass with no test removed.
2. Every `examples/*.eta` file still runs under `etai` unchanged
   (after the in-example macro definitions for `dotimes` etc. are
   replaced by `(import std.core)` / `(import std.math)`).
3. `grep -rn '(#t ' stdlib/std/` returns zero matches.
4. `grep -rnE 'letrec\s*\(\(\w+\s*\(lambda' stdlib/std/` returns zero
   matches in files targeted by Phase 2.
5. `grep -n '__' stdlib/std/test.eta` returns zero matches.
6. `prelude.eta` doc-comment matches the actual `(import …)` list.
7. `stdlib/std/clpb.eta` remains the style baseline — the new
   `%clp-binop` macro becomes the *new* baseline for `clp.eta` and
   `clpr.eta`.
8. `grep -rn "(defrel '(" stdlib/ examples/` returns zero matches
   after the `defrel` macro lands.
9. `stdlib/tests/macros.test.eta` exists and exercises every
   stdlib-exported `define-syntax` form.
10. `docs/modules.md` reference table is updated for any rename, and
    a new `docs/stdlib_style.md` (extracted from the §"Scheme Idiom
    Guidelines" + §"Macros" of this plan) is added so the convention
    survives future contributions.


