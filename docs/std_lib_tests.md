# Stdlib Test Coverage — Multi-Stage Plan

[← Back to README](../README.md) · [Modules & Stdlib](modules.md) ·
[DAP + VS Code Plan](dap_vs_plan.md) · [Next Steps](next-steps.md)

---

## Goal

Bring the Eta standard library (`stdlib/std/*.eta` + `stdlib/prelude.eta`)
to **excellent, contributor-trustworthy test coverage**, exclusively
using Eta's own [`std.test`](../stdlib/std/test.eta) framework and the
[`eta_test`](../eta/test_runner/src/main_test_runner.cpp) discovery
runner already wired into CTest and the VS Code Test Explorer (TAP 13).

The work is structured so each stage is independently shippable and
each new test file plugs straight into the existing
`eta_stdlib_tests` CTest target without any C++ changes.

---

## Audit — what actually exists

> Counts come from a fresh read of `stdlib/std/` and `stdlib/tests/`,
> not from prior docs. "Symbols" counts public `(export ...)` entries.
> "Tests" is approximate `(make-test ...)` count derived from file
> structure / size.

| Module (`stdlib/std/…`) | Exports | Test file (`stdlib/tests/…`) | Test count (~) | Coverage | Notable gaps |
|---|---:|---|---:|---|---|
| [`core.eta`](../stdlib/std/core.eta) | 14 | [`core.test.eta`](../stdlib/tests/core.test.eta) | ~40 | **Good** | mostly tests *builtins*; few tests for `assoc-ref`, `caddar`, `windows?`, `void`, `list?` edge cases |
| [`math.eta`](../stdlib/std/math.eta) | 17 | [`math.test.eta`](../stdlib/tests/math.test.eta) | ~25 | **Partial** | `gcd/lcm` with negatives & zero; `expt` with float/negative exponents; `quotient` corner cases; `floor/ceiling/truncate/round` on negatives & half-integers; `sum/product` on empty list |
| [`aad.eta`](../stdlib/std/aad.eta) | 15 | [`aad.test.eta`](../stdlib/tests/aad.test.eta) | ~12 | **Shallow** | no test for `grad`, `check-grad`, `check-grad-report`, `with-checkpoint`; smooth helpers tested only at one point |
| [`io.eta`](../stdlib/std/io.eta) | 8 | — | 0 | **None** | `print`, `println`, `eprintln`, `display-to-string`, `read-line`, `with-output-to-port`, `with-input-from-port`, `with-error-to-port` are entirely untested |
| [`collections.eta`](../stdlib/std/collections.eta) | 22 | [`collections.test.eta`](../stdlib/tests/collections.test.eta) | ~35 | **Partial** | `take`/`drop` with `n>len`, `n=0`, `n<0`; `zip` unequal lengths; `pairwise` on len 0/1; `flatten` deep nesting; `range` step 0/negative; vector helpers on empty / size-1 |
| [`logic.eta`](../stdlib/std/logic.eta) | 22 | [`logic.test.eta`](../stdlib/tests/logic.test.eta), `logic_core_primitives.test.eta`, `logic_minikanren_combinators.test.eta` | ~80 | **Good** | `logic-throw` / `logic-catch` cross-product with `findall`; `conda` / `condu` commit semantics; `run-n` with N=0 |
| [`clp.eta`](../stdlib/std/clp.eta) | 22 | `clp.test.eta` + 9 `clp_*.test.eta` | ~120 | **Good** | `clp:element`, `clp:scalar-product`, `clp:abs` underused; `clp:maximize` mirror of minimize tests |
| [`clpb.eta`](../stdlib/std/clpb.eta) | 13 | `clpb_propagation.test.eta` | ~25 | **Partial** | `clp:taut?`, `clp:card` lower/upper boundary, `clp:imp`, `clp:eq` standalone; labeling-b on UNSAT |
| [`clpr.eta`](../stdlib/std/clpr.eta) | 21 | 6 `clpr_*.test.eta` | ~70 | **Good** | open / half-open interval edge propagation; `clp:r-feasible?` after retract |
| [`freeze.eta`](../stdlib/std/freeze.eta) | 2 (`freeze`, `dif`) | `attrvar_freeze_dif_trail.test.eta` | ~10 | **Partial** | covers trail roundtrips; missing: `freeze` on already-bound var, `dif` chained with re-binding, `dif` inside `findall` |
| [`causal.eta`](../stdlib/std/causal.eta) | ~24 | [`causal.test.eta`](../stdlib/tests/causal.test.eta) | ~25 | **Partial** | `dag:adjustment-sets-observed`, `do:identify-details*`, `do:estimate-effect`, `do:conditional-mean`, `do:marginal-prob` thinly tested or absent |
| [`fact_table.eta`](../stdlib/std/fact_table.eta) | 17 | [`fact_table.test.eta`](../stdlib/tests/fact_table.test.eta) | ~30 | **Partial** | `fact-table-group-sum`, `fact-table-partition`, `fact-table-load-csv`/`save-csv` round-trip; multi-column index; query against missing key |
| [`db.eta`](../stdlib/std/db.eta) | 8 | [`db.test.eta`](../stdlib/tests/db.test.eta) | ~10 | **Shallow** | `index-rel!` perf semantics; `tabled` recursive relation (transitive closure) cycle handling; `retract-all` after `index-rel!` |
| [`csv.eta`](../stdlib/std/csv.eta) | 18 | [`csv.test.eta`](../stdlib/tests/csv.test.eta) | ~15 | **Partial** | quoted commas / embedded newlines / CR/LF; `csv:read-typed-row` type coercion; `csv:fold` short-circuit; writer flush on close; load/save roundtrip with unicode |
| [`stats.eta`](../stdlib/std/stats.eta) | 30 | [`stats.test.eta`](../stdlib/tests/stats.test.eta) | ~25 | **Partial** | `stats:cov-matrix` / `cor-matrix`, `stats:ols-multi*` accessors, `stats:t-quantile` extreme p, `stats:summary` shape, `stats:percentile` boundaries 0/1 |
| [`regex.eta`](../stdlib/std/regex.eta) | 16 | [`regex.test.eta`](../stdlib/tests/regex.test.eta) | ~25 | **Partial** | named groups, `regex:replace-fn`, `regex:split` empty-match behaviour, unicode classes, `regex:quote` round-trip in a real pattern |
| [`time.eta`](../stdlib/std/time.eta) | 10 | [`time.test.eta`](../stdlib/tests/time.test.eta) | ~7 | **Partial** | DST boundary parts, formatting non-zero epochs, `time:sleep-ms` 0 / negative, monotonic strict-positive after sleep |
| [`net.eta`](../stdlib/std/net.eta) | 5 | — | 0 | **None** (NNG-gated) | `with-socket` cleanup on escape; `request-reply` happy path on inproc transport; `worker-pool` task distribution; `pub-sub` event delivery; `survey` scatter-gather |
| [`supervisor.eta`](../stdlib/std/supervisor.eta) | 2 (`one-for-one`, `one-for-all`) | — | 0 | **None** (NNG-gated) | restart on child crash; `one-for-all` cascading restart |
| [`torch.eta`](../stdlib/std/torch.eta) | ~50 | — | 0 | **None** (Torch-gated) | tensor creation, arithmetic, autograd `backward!` + `grad`, `linear` / `sequential`, optimizers, save/load roundtrip |
| [`test.eta`](../stdlib/std/test.eta) | 14 | [`test-framework.test.eta`](../stdlib/tests/test-framework.test.eta) | ~15 | **Partial** | `print-tap` / `print-junit` output golden-string assertion; nested groups; mixed pass/fail summary |
| [`prelude.eta`](../stdlib/prelude.eta) | re-export only | — | 0 | **None** | one smoke test that `(import std.prelude)` exposes every advertised symbol |

Auxiliary test files not mapped to a single `std.*` module
(retain — they cover runtime/builtin behaviour underneath the stdlib):

- [`compound_term_primitives.test.eta`](../stdlib/tests/compound_term_primitives.test.eta)
- [`runtime_error_catchability.test.eta`](../stdlib/tests/runtime_error_catchability.test.eta)
- [`attrvar_freeze_dif_trail.test.eta`](../stdlib/tests/attrvar_freeze_dif_trail.test.eta) (also rated under `freeze.eta` above)

### Coverage rating legend

| Rating | Meaning |
|---|---|
| **None** | No test file references the module. |
| **Shallow** | <½ of public symbols touched; no edge / error tests. |
| **Partial** | Most symbols touched, but no boundary / unicode / large-input / failure-mode tests. |
| **Good** | All public symbols touched, with edge cases and at least one error-path test. |

---

## Conventions

The framework lives in [`stdlib/std/test.eta`](../stdlib/std/test.eta).
Each test file is a self-contained module that ends with
`(print-tap (run suite))` so the [`eta_test`](../eta/test_runner/src/main_test_runner.cpp)
runner can scoop the TAP-13 stream. The Test Controller in
[`editors/vscode/src/testController.ts`](../editors/vscode/src/testController.ts)
parses that exact format.

**Skeleton** — copy/paste from
[`core.test.eta`](../stdlib/tests/core.test.eta) lines 1–17:

```scheme
;;; stdlib/tests/<module>.test.eta — Unit tests for std.<module>

(module <module>.tests
  (import std.test std.<module>)        ; + any others actually used
  (begin

    (define suite
      (make-group "std.<module>"
        (list
          (make-group "<sub-area>"
            (list
              (make-test "<concise behaviour name>"
                (lambda () (assert-equal 4 (+ 2 2))))
              (make-test "<another>"
                (lambda () (assert-true (number? 1)))))))))

    (print-tap (run suite))))
```

Available assertions (from `std.test` lines 80–123):

- `(assert-true x [msg])`
- `(assert-false x [msg])`
- `(assert-equal expected actual [msg])`     — `equal?` deep compare
- `(assert-not-equal a b [msg])`
- `(assert-approx-equal expected actual [tol])` — defaults to `1e-9`

> **Limitation today:** the framework has no try/catch; a failing
> assertion aborts the file. Stage 5 below proposes a small
> `assert-raises` extension once the runtime exposes try/catch
> (see `runtime_error_catchability.test.eta`).

**Discovery** — files matching `*.test.eta` (or `*_smoke.eta`) under any
path passed to `eta_test` are auto-discovered
([`main_test_runner.cpp`](../eta/test_runner/src/main_test_runner.cpp)
lines 17–25). Adding a new file requires *no* CMake change.

---

## Stage 1 — Fill the no-test modules (S–M)

**Goal.** Eliminate every **None** row in the audit table.

### Files to create

| Path | For module | Notes |
|---|---|---|
| `stdlib/tests/io.test.eta` | `std.io` | All assertions can use `open-output-string` / `open-input-string` — no file system needed |
| `stdlib/tests/freeze.test.eta` | `std.freeze` | High-level `freeze` / `dif` semantics distinct from the existing `attrvar_*` low-level test |
| `stdlib/tests/prelude_smoke.test.eta` | `stdlib/prelude.eta` | Iterate the export list and assert each symbol is `procedure?` / bound |
| `stdlib/tests/net.test.eta` | `std.net` | Wrap whole `(define suite ...)` in a `cond` that bails to a single skip-test when `nng-socket?` is undefined; use only `inproc://` transport |
| `stdlib/tests/supervisor.test.eta` | `std.supervisor` | Same NNG guard. Spawn 2 short-lived children; assert restart count |
| `stdlib/tests/torch.test.eta` | `std.torch` | Guard with `(if (defined? 'torch/tensor) ...)`-style check; one test per logical area: tensor, arithmetic, autograd, NN layer, optimizer, save/load roundtrip |

### Per-file checklist

**`io.test.eta`** — at minimum:

1. `(println "x")` writes `"x\n"` to a captured port.
2. `(eprintln ...)` routes to `current-error-port`, not stdout.
3. `(display-to-string '(1 2 3))` returns `"(1 2 3)"`.
4. `(read-line)` from `"abc\nxyz"` returns `"abc"` and a second call returns `"xyz"`.
5. `(read-line)` from `""` returns `#f`.
6. `(read-line)` skips a `\r` before `\n` (CRLF).
7. `with-output-to-port` restores the prior `current-output-port` even when the thunk throws (use `runtime_error_catchability` patterns once available; for now, assert restoration on normal return).
8. Symmetric tests for `with-input-from-port`, `with-error-to-port`.

**`freeze.test.eta`**:

1. `(freeze v thunk)` on already-bound `v` runs the thunk **immediately**.
2. `(freeze v thunk)` on an unbound `v`, then `(unify v 42)` fires the thunk exactly once.
3. Two consecutive `freeze` calls on the same var both fire (LIFO or in attach order — pin the observed behaviour).
4. `(dif x y)` succeeds when `x` and `y` are distinct ground values.
5. `(dif x y)` fails after later unification makes them equal.
6. `dif` interacts correctly with `findall` (no leaked bindings between branches).

**`net.test.eta` / `supervisor.test.eta`** (NNG-gated): mirror the
existing actor smoke tests in `compound_term_primitives.test.eta`'s
guard pattern; use ephemeral `inproc://...` URIs so tests are
hermetic.

**`torch.test.eta`** (Torch-gated): one `make-group` per area listed
above; assert numeric `assert-approx-equal` with `1e-5` tolerance for
GPU/float32 tests.

**`prelude_smoke.test.eta`**:

1. `(import std.prelude)` succeeds.
2. For a curated list (≈10 representative names from each section of
   `prelude.eta` lines 42–181), assert each is bound and is either
   `procedure?` or, for constants like `pi`, `number?`.

---

## Stage 2 — Deepen shallow / partial tests (M)

**Goal.** Promote every **Shallow** / **Partial** row to **Good** by
adding edge-case, boundary, unicode, and error-path tests.

### Per-module checklist (extend the existing test file in place)

**[`math.test.eta`](../stdlib/tests/math.test.eta)** — add a new
`make-group` "edges" with:
- `(gcd 0 0)` → 0; `(gcd -12 18)` → 6; `(lcm 0 5)` → 0.
- `(expt 2 -3)` → 0.125; `(expt 0 0)` → 1; `(expt 0 5)` → 0.
- `(floor -1.5)` → -2; `(ceiling -1.5)` → -1; `(round 0.5)` round-half-to-even or away-from-zero (pin the observed convention).
- `(quotient 7 -2)` and `(quotient -7 2)`; `(quotient 0 5)`.
- `(sum '())` → 0; `(product '())` → 1.
- `(clamp 5 10 2)` (lo > hi — pin behaviour).

**[`aad.test.eta`](../stdlib/tests/aad.test.eta)**:
- `grad` of `(lambda (x y) (+ (* x x) (* y y)))` at `(3 4)` → value 25, grad `#(6 8)`.
- `grad` of a single-variable `relu` at +1, -1, 0 (kink test).
- `check-grad` on a known polynomial returns `#t` within tolerance.
- `check-grad-report` returns a structured result with `max-abs-err` ≤ tol.
- `with-checkpoint` on a 2-call composition matches `grad` without checkpoint.

**[`collections.test.eta`](../stdlib/tests/collections.test.eta)** — add
`make-group "edges"`:
- `(take 0 '(1 2 3))` → `()`; `(take 5 '(1 2))` → `(1 2)` (no error); `(take -1 '(1 2))` (pin behaviour, document).
- Symmetric for `drop`.
- `(zip '() '(1))` → `()`; `(zip '(1 2 3) '(a))` → `((1 a))`.
- `(pairwise '())` → `()`; `(pairwise '(1))` → `()`; `(pairwise '(1 2))` → `((1 2))`.
- `(flatten '(1 (2 (3 (4))) 5))` → `(1 2 3 4 5)`.
- `(range 5)`, `(range 1 5)`, `(range 0 10 2)`, `(range 5 0 -1)`.
- `(reduce + '())` raises (assert via current abort behaviour or skip until `assert-raises` exists).
- `(any? odd? '())` → `#f`; `(every? odd? '())` → `#t`.
- `(vector-map ...)` on `#()`; `(vector->list #())` → `()`.

**[`stats.test.eta`](../stdlib/tests/stats.test.eta)** — add:
- `stats:percentile` at `0.0` and `1.0`.
- `stats:cov-matrix` of a 3-column constant matrix → all zeros on off-diagonals.
- `stats:cor-matrix` diagonal is exactly 1.0.
- `stats:ols-multi` on a fact-table column slice; assert `coefficients`/`std-errors`/`p-values` accessors return vectors of the right arity.
- `stats:summary` returns an alist containing keys `mean`, `median`, `stddev`, `min`, `max`, `n` (or whatever the implementation actually emits — read the source first).

**[`regex.test.eta`](../stdlib/tests/regex.test.eta)** — add:
- Named group: pattern `(?<word>\w+)` and `regex-match-named`.
- `regex:replace-fn` with a lambda that uppercases each match.
- `regex:split` of `"a,,b"` (pin: empty middle field).
- `regex:quote` of `".*+?"` then re-compile and `regex:match?` against the literal.
- Unicode: `(regex:match? "\\p{L}+" "café")` (only if the engine supports `\p{L}`; otherwise document).

**[`csv.test.eta`](../stdlib/tests/csv.test.eta)** — add:
- Quoted field containing a comma: `"a,b","c"` → 2 fields.
- Quoted field with embedded `\n`.
- CRLF line endings.
- `csv:read-typed-row` with column types `(int float str)`.
- `csv:fold` accumulator threading.
- `csv:save-file` then `csv:load-file` round-trip on a 100-row table.

**[`time.test.eta`](../stdlib/tests/time.test.eta)** — add:
- Format non-zero epoch (e.g. `1700000000000`) UTC string equals a known constant.
- `time:utc-parts` of a known DST date, assert `is-dst` matches the timezone-correct expectation only on `local-parts`.
- `time:sleep-ms 0` returns immediately and does not error.

**[`db.test.eta`](../stdlib/tests/db.test.eta)** — add:
- `index-rel!` followed by a query that would otherwise be O(n).
- `tabled` over a recursive `path/2` defined on a graph with a cycle —
  assert termination and answer-set cardinality.
- `retract-all` then `call-rel` returns `()`.

**[`fact_table.test.eta`](../stdlib/tests/fact_table.test.eta)** — add:
- `fact-table-group-sum` on a 3-group dataset.
- `fact-table-partition` returns the documented two halves.
- CSV bridge: build a table, `fact-table-save-csv`, reload via
  `fact-table-load-csv`, assert row equality.

**[`causal.test.eta`](../stdlib/tests/causal.test.eta)** — add:
- `dag:adjustment-sets-observed` filters out latents.
- `do:identify-details` returns the adjustment set on the
  `backdoor-dag`.
- `do:estimate-effect` on a hand-built dataset returns the analytic
  expected value within `assert-approx-equal` 1e-6.

**[`test-framework.test.eta`](../stdlib/tests/test-framework.test.eta)** — add:
- `print-tap` of a 2-pass / 1-fail group writes lines exactly matching
  a golden string captured via `with-output-to-port`.
- `print-junit` writes `<testsuite tests="3" failures="1">`.
- Nested `make-group` two levels deep produces a flat TAP plan.

---

## Stage 3 — Property-style / fuzz tests (M)

**Goal.** Catch regressions that example-based tests miss, by sampling
inputs and asserting algebraic laws.

The framework has no `quickcheck`. Build a tiny helper inside each
test file (or once in a new
`stdlib/tests/_helpers/property.eta` that test files can `import`):

```scheme
(defun for-all (gen n thunk)
  (letrec ((loop (lambda (i)
                   (if (= i n) 'ok (begin (thunk (gen i)) (loop (+ i 1)))))))
    (loop 0)))

(defun gen-int (i) (- (modulo (* i 2654435761) 1000) 500))
```

### Properties to add

- **`std.collections`** (`stdlib/tests/collections_properties.test.eta`)
  - `(reverse (reverse xs)) ≡ xs`
  - `(length (append xs ys)) ≡ (+ (length xs) (length ys))`
  - `(foldl + 0 xs) ≡ (sum xs)`
  - `(map* identity xs) ≡ xs`
  - `(filter (negate p) xs) ++ (filter p xs)` is a permutation of `xs`.
  - `(take n xs) ++ (drop n xs) ≡ xs` for `0 ≤ n ≤ length`.

- **`std.math`** (`stdlib/tests/math_properties.test.eta`)
  - `(gcd a b) | a` and `| b`.
  - `(* (gcd a b) (lcm a b)) ≡ (* a b)` for positive a, b.
  - `(square (sign x))` ≡ 0 or 1.

- **`std.logic`** (`stdlib/tests/logic_properties.test.eta`)
  - `(== x y)` after success means `(equal? (deref-lvar x) (deref-lvar y))`.
  - `findall` of a goal ∪ `naf` of the same goal is the universe.
  - `copy-term*` preserves structure (`equal?` on ground; `length` on lists).

- **`std.regex`** (`stdlib/tests/regex_properties.test.eta`)
  - For random ASCII strings `s`, `(regex:match? (regex:quote s) s)` ≡ `#t`.
  - `(regex:split p (string-join (regex:find-all p s) ""))` round-trips for non-overlapping matches.

---

## Stage 4 — Performance / large-input smoke tests (S)

**Goal.** Detect O(n²) or accidental algorithmic regressions, without
becoming a benchmark suite. Each test asserts a *cap* on
`time:elapsed-ms`, not a target.

### Files to create

- `stdlib/tests/collections_perf_smoke.eta` — sort 100k random ints,
  filter/map/foldl over 1M-element lists. Cap: < 5 s.
- `stdlib/tests/regex_perf_smoke.eta` — `regex:find-all` on a 1 MB
  string with a benign pattern. Cap: < 2 s.
- `stdlib/tests/fact_table_perf_smoke.eta` — insert 100k rows, build
  index, query 1k random keys. Cap: < 2 s.
- `stdlib/tests/clp_perf_smoke.eta` — N-queens N=10. Cap: < 5 s.

The runner picks up `*_smoke.eta` automatically
([`main_test_runner.cpp`](../eta/test_runner/src/main_test_runner.cpp)
lines 19–22), so no CMake edits.

Use `(time:monotonic-ms)` deltas around the workload and
`assert-true (< delta cap-ms)`.

---

## Stage 5 — Error-path tests + small framework lift (S–M)

**Goal.** Today an `(error ...)` aborts the whole file. Once
runtime try/catch is exposed (it already exists per
`runtime_error_catchability.test.eta`), wire a thin
`assert-raises` into `std.test`.

### Steps for the framework

1. In [`stdlib/std/test.eta`](../stdlib/std/test.eta), add:
   - `(defun assert-raises (thunk [pred] [msg]))` that uses the same
     try/catch primitive exercised in `runtime_error_catchability.test.eta`,
     returning `'ok` if the thunk raises (and optionally `pred`
     matches the error), `(error ...)` otherwise.
   - Update the runner loop `__run-test` (lines 127–133) to catch
     thrown errors and mark the test failed instead of aborting the file.
2. Add tests in `test-framework.test.eta` for:
   - `assert-raises` on a thunk that raises → pass.
   - `assert-raises` on a thunk that returns normally → fail.
   - One failing test in a group does not prevent later tests from running.

### Per-module error tests to add

- `std.collections`: `(reduce + '())` raises; `(list-ref '() 0)` raises.
- `std.math`: `(quotient 1 0)` raises; `(gcd ...)` with non-integer.
- `std.logic`: `(unify <cyclic>)` behaviour pinned (occurs check on/off).
- `std.regex`: `(regex:compile "[")` raises a structured error.
- `std.csv`: malformed quoting raises; missing column in `read-typed-row` raises.
- `std.fact_table`: insert with wrong arity raises; query unknown column raises.

---

## Stage 6 — CI gating + coverage reporting (S)

**Goal.** Make the test suite a hard gate and surface its size as a
visible metric, mirroring the DoD pattern in
[`dap_vs_plan.md`](dap_vs_plan.md).

1. **CTest** — already wired. Confirm
   [`eta/test_runner/CMakeLists.txt`](../eta/test_runner/CMakeLists.txt)
   lines 50–57 register the suite under `eta_stdlib_tests`. No change
   needed unless we want to split fast vs `*_smoke` perf tests; if so,
   add a second `add_test(NAME eta_stdlib_perf_smoke ... LABELS perf)`
   so contributors can `ctest -L perf` selectively.
2. **GitHub Actions** — extend the existing CI workflow (or create
   `.github/workflows/stdlib-tests.yml`) to:
   - Build `eta_test`.
   - Run `ctest --output-on-failure -R eta_stdlib_tests`.
   - Upload the aggregated TAP output as a workflow artifact for the
     VS Code Test Explorer to consume on PR review.
3. **Coverage signal** — add a `scripts/stdlib-coverage.eta` (or a
   small shell script) that scans `stdlib/std/*.eta` `(export …)`
   sections, greps `stdlib/tests/**/*.eta` for each name, and prints
   an "untested exports" report. Wire it into CI as a non-blocking
   advisory until **Good** is reached for every module.

---

## Recommended ordering & sizing

| # | Stage | Sizing | Rationale |
|---|---|:---:|---|
| 1 | Stage 6 (CI gating, advisory coverage script first) | **S** | Surfaces the gap quantitatively before we start filling it |
| 2 | Stage 1 (no-test modules: `io`, `freeze`, `prelude` smoke) | **S** | Pure non-gated wins, pure-Eta, no NNG/Torch needed |
| 3 | Stage 1 (NNG- and Torch-gated: `net`, `supervisor`, `torch`) | **M** | Need build-flag guards; smaller scope per file |
| 4 | Stage 2 (deepen `math`, `collections`, `aad`, `csv`, `regex`, `stats`, `time`) | **M** | Bulk of new test code; high coverage payoff per hour |
| 5 | Stage 5 (lift framework + add error-path tests) | **M** | Unblocks structured error tests for *all* modules |
| 6 | Stage 2 (deepen `db`, `fact_table`, `causal`, `clpb`) | **M** | Domain-specific; benefits from Stage 5's `assert-raises` |
| 7 | Stage 3 (property tests) | **M** | Catches regressions Stage 2 misses |
| 8 | Stage 4 (perf smoke caps) | **S** | Quick win once Stage 2 stabilises |
| 9 | Stage 6 (flip the coverage script to **blocking** in CI) | **S** | Final gate; prevents regression |

Total: ≈ 15–25 engineer-days, parallelisable across modules.

---

## Definition of Done

A stage is done when:

1. Every row in the §"Audit" table is rated **Good**, and the
   advisory `scripts/stdlib-coverage.eta` reports zero untested public
   exports for every non-gated module.
2. `ctest -R eta_stdlib_tests` passes locally and in CI on
   Linux / Windows / macOS.
3. NNG-gated and Torch-gated test files self-skip cleanly when the
   feature is not built in (single TAP `ok` line documenting the skip,
   not a failure).
4. The TAP-13 output produced by `eta_test` opens cleanly in the
   VS Code Test Explorer with no parser warnings (verified against
   [`testController.ts`](../editors/vscode/src/testController.ts)).
5. `std.test` exposes `assert-raises`, and at least one error-path
   test exists per module that has a documented failure mode.
6. The CI workflow uploads the aggregated TAP file as an artifact and
   the perf-smoke job reports its measured timings in the summary.
7. `docs/std_lib_tests.md` (this file) is linked from `README.md` and
   `docs/modules.md`.

---

## How to run the tests locally

The test runner discovers `*.test.eta` (and `*_smoke.eta`) files
recursively and aggregates TAP-13.

```bash
# 1. Build (configures CTest as a side effect)
cmake -S . -B build
cmake --build build --target eta_test etai eta_core_test

# 2a. Run via CTest (preferred — matches CI)
ctest --test-dir build --output-on-failure -R eta_stdlib_tests

# 2b. Run directly (more flexible — pick a single file or a sub-tree)
./build/eta/test_runner/eta_test \
    --path stdlib \
    stdlib/tests

# 2c. A single file (any extension is accepted when given as a file arg)
./build/eta/test_runner/eta_test \
    --path stdlib \
    stdlib/tests/collections.test.eta

# 2d. JUnit XML for IntelliJ / CI ingestion
./build/eta/test_runner/eta_test --format junit \
    --path stdlib stdlib/tests > stdlib-tests.xml

# 3. The convenience target rebuilds binaries and re-runs both
#    the C++ runtime tests and the stdlib tests
cmake --build build --target eta_rebuild_and_test
```

(Sources: [`eta/test_runner/CMakeLists.txt`](../eta/test_runner/CMakeLists.txt)
lines 50–57; [`eta/CMakeLists.txt`](../eta/CMakeLists.txt) lines 113–123;
[`eta/test_runner/src/main_test_runner.cpp`](../eta/test_runner/src/main_test_runner.cpp)
lines 1–28.)

On Windows, swap `./build/.../eta_test` for `build\eta\test_runner\<Config>\eta_test.exe`.

---

## Source locations referenced

| Component | File |
|---|---|
| Test framework | [`stdlib/std/test.eta`](../stdlib/std/test.eta) |
| Test runner | [`eta/test_runner/src/main_test_runner.cpp`](../eta/test_runner/src/main_test_runner.cpp) |
| Runner CMake | [`eta/test_runner/CMakeLists.txt`](../eta/test_runner/CMakeLists.txt) |
| Top-level convenience target | [`eta/CMakeLists.txt`](../eta/CMakeLists.txt) (lines 113–123) |
| Stdlib prelude | [`stdlib/prelude.eta`](../stdlib/prelude.eta) |
| Stdlib modules | [`stdlib/std/`](../stdlib/std/) |
| Existing tests | [`stdlib/tests/`](../stdlib/tests/) |
| VS Code Test Explorer integration | [`editors/vscode/src/testController.ts`](../editors/vscode/src/testController.ts) |

