# Interpreter Loop and Conformance Plan

Two independent pieces of work:

- **Part A, interpreter loop overhead:** remove per-instruction work in the VM dispatch loop.
  Small and mechanical; no semantic change.
- **Part B, R7RS conformance suite:** a scored, repeatable check of language semantics against
  R7RS-small. It is open-ended, because every failure it finds becomes a fix.

## Recommended order

1. **A0** (benchmark baseline) first. Part A needs a baseline, and Part B needs the harness to
   track over time.
2. **A1–A3** next. A few days' work, low risk, and already covered by the existing ~1000 C++
   tests and the stdlib suite. It changes no semantics, so a conformance suite would add little
   protection here.
3. **Part B** after that. It runs on a faster VM, and its fixes are independent of Part A.
4. **A4** (verifier stack-depth proof, then dropping the runtime check) is optional and can come
   at any point after A3.

The reasoning: Part A is bounded and Part B is not. Doing A first gives the measured gain
quickly and leaves a stable VM for the open-ended conformance work.

---

## Part A: Interpreter loop overhead

### Background

Each instruction runs this work before the `switch` (`core/src/eta/runtime/vm/vm.cpp`, the
`while (current_func_ && pc_ < ...)` loop near line 898):

| # | Work | Cost |
|---|------|------|
| 1 | `complete_pending_blocking_primitive()` | out-of-line call; returns early unless a dirty task is pending |
| 2 | `pending_run_loop_status_.has_value()` | cheap |
| 3 | `prof::runtime_profiler().sampling_active()` | function-local static (init-guard check) plus an atomic load |
| 4 | `heap_.pending_finalizer_count()` | **locks `finalizer_mutex_`** (`heap.cpp:166`), once per instruction |
| 5 | `debug_ \|\| interrupt_flag_.load()` | cheap |
| 6 | `stack_operands(instr)` plus the underflow check | a `switch` plus a compare. This is the only stack-underflow guard, because the verifier does not check stack depth. |
| 7 | `charge_reductions()` | non-atomic counters, plus `actor_system_->add_current_thread_reductions()`. That is a thread-local lookup plus an **atomic `fetch_add`** per instruction, and the Driver always has an actor system. |
| 8 | `current_func_->span_at(pc_ - 1)` | a bounds check and a `Span` copy per instruction; only error paths and calls use it |

Most opcodes are a push or a pop, so items 3, 4, 7 and 8 likely cost more than the instruction
itself.

### Phase A0: Benchmark baseline (you run this)

- **Add `qa/bench/eta/`** with small, deterministic programs. Each prints its result so
  correctness is checked too:
  - `fib.eta`: recursive `(fib 30)` (calls, arithmetic, `NumCmp`)
  - `loop_int.eta`: sum to 10M with `(< i n)`, as in the numerics plan's Phase 0
  - `loop_flo.eta`: the same loop over flonums
  - `closure.eta`: a closure loop that reads 3 captured variables per iteration (`LoadUpval`)
  - `vector.eta`: `vector-ref`/`vector-set!` over a 1M-element vector (builtin calls, heap
    access)
  - `list.eta`: build and `reverse`/`length`/`map` a 1M-element list (cons pool, `null?`,
    `pair?`)
  - `string.eta`: `string-append` and `string-ref` in a loop
- **Run each 5 times with `etai`** on a Release build and record the median in
  `qa/bench/eta/RESULTS.md`, with the machine and commit.
- **Profile `fib` and `closure` once** (VS Profiler or `perf`) to confirm items 3, 4 and 7 show
  up. Record the top 10 functions in the same file.

### Phase A1: Cheap fixes (no behaviour change)

- **Finalizer check.** Add `std::atomic<std::size_t> pending_finalizer_hint_` to `Heap`, updated
  inside the existing locked enqueue and dequeue in `heap.cpp`. Add
  `bool has_pending_finalizers() const noexcept` that does a relaxed load. The loop calls that
  instead of `pending_finalizer_count()`. `process_pending_finalizers()` still takes the lock.
- **Profiler check.** Cache `prof::ProfilerRuntime& profiler_` as a VM member (reference
  initialised in the constructor), so the per-instruction check is just the atomic load, with no
  static-init guard.
- **Pending primitive.** Inline the fast check:
  `if (pending_blocking_primitive_.has_value()) [[unlikely]] { ...complete... }`.
- **Source span.** Remove the per-instruction `instr_span`. Keep
  `const std::uint32_t instr_pc = pc_ - 1;` and add
  `reader::lexer::Span span_of(const BytecodeFunction*, std::uint32_t pc)`. At each use (error
  returns and `dispatch_callee(..., span)` calls), compute the span from
  `instr_func`/`instr_pc` captured before the `switch`. A call can change `current_func_` before
  its error is routed, so the call opcodes (`Call`, `TailCall`, `Apply`, `TailApply`,
  `CallWithValues`, `DynamicWind`, `CallCC`) compute their span once, up front.
- **Tests:** the existing suite (error spans are asserted in `runtime_error_catchability`, the DAP
  tests and `vm_tests`) plus the A0 benchmarks.

### Phase A2: Batch the atomic reduction count

- **Split `charge_reductions`** into:
  - an inline fast path, `charge_reductions_fast()`, run once per instruction: `++reductions_`,
    `++unflushed_reductions_`, and the slice-budget decrement (all plain members)
  - `flush_reductions()`, which does the
    `actor_system_->add_current_thread_reductions(unflushed_reductions_)` atomic add and zeroes
    the counter
- **Flush:**
  - in `dispatch_callee` before running a primitive (so `process-info` sees an up-to-date count;
    `actor_process_info_reports_reductions_and_state` relies on this)
  - when a slice ends or yields
  - when `execute` returns
  - every 1024 unflushed reductions
- **Semantics:** `vm.reductions()` and the slice budget stay exact and per-instruction, so
  `vm_tests` around line 3570 is unaffected. Only the cross-thread actor counter lags, by at most
  1024 reductions or until the next primitive call.
- **Double charging:** `dispatch_callee` also calls `charge_reductions()`. Keep that, since it
  counts the call, but route it through the fast path.

### Phase A3: One safepoint check

- **Group the rare conditions** behind one countdown: profiler sampling, pending finalizers and
  the interrupt flag. Add `std::uint32_t safepoint_countdown_ = kSafepointInterval;` (256). Each
  instruction decrements it, and the slow checks run only when it reaches 0, or immediately on
  `Call`/`TailCall`/`Return` and backward `Jump`s. Latency for an interrupt or a finalizer is
  then at most 256 straight-line instructions, and loops still check on every iteration.
- **Kept per instruction** (correctness depends on them):
  - `debug_` (the debugger must stop on exact lines)
  - `pending_run_loop_status_` (the yield point)
- **Tests:** `driver_jupyter_test/request_interrupt_stops_runaway_evaluation_quickly` (a runaway
  loop must still stop promptly), finalizer tests, profiler sampling tests and DAP stepping
  tests.

### Phase A4 (optional): Verifier proves stack depth, then drop the runtime check

- **Verifier.** Add stack-height analysis to `BytecodeSerializer::verify_function`: a worklist
  over the control-flow graph, `stack_operands()` and push counts per opcode, and a consistent
  height at every join (`Jump`, `JumpIfFalse`, `SetupCatch` handlers), with the height never
  below 0 and never above `stack_size`. Reject a file with
  `SerializerError::InvalidBytecode` on any violation.
- **Emitter.** Assert the same property in Debug builds for freshly emitted code.
- **VM.** Keep the per-instruction underflow check only under `#ifndef NDEBUG` (or behind a
  `ETA_VM_CHECKED` option). Release trusts verified or emitted bytecode.
- **Tests:** verifier tests for an unbalanced join, underflow at entry, and overflow past
  `stack_size`. The existing fuzz targets (`qa/fuzz`) must keep passing.

### Done when

- Every A0 benchmark is re-measured and recorded next to the baseline.
- All existing tests pass.
- The profiler no longer shows `pending_finalizer_count`, `add_current_thread_reductions` or
  `runtime_profiler` in the top 10 for `fib`.

---

## Part B: R7RS conformance suite

### Goal

A scored, repeatable answer to "how much of R7RS-small does Eta implement correctly", run in CI.
Every failure ends up as one of:

- **fixed**
- **known-unsupported**, a deliberate gap with a reason (for example, no bignums)
- **Eta-divergent**, a documented design difference (for example, the module syntax)

### Source material

- **Primary:** chibi-scheme's `tests/r7rs-tests.scm`, about 1000 assertions organised by R7RS
  report section. BSD-3 licensed: keep the copyright notice in the ported files, and add a
  `THIRD_PARTY_NOTICES.md` beside `LICENSE.txt` (there is none yet).
- **Secondary, later:** selected files from the Larceny R7RS test suite for areas chibi covers
  thinly (string and char edge cases, `dynamic-wind` with `call/cc`).
- **Performance companion:** ecraven's `r7rs-benchmarks`, for cross-implementation comparison
  against Chibi and Guile. These get an Eta prelude shim; they are kept separate from Part A's
  micro-benchmarks.

### Phase B0: Harness

- **Layout:** `qa/conformance/r7rs/`
  - `README.md`: source, licence, how to run, the scoring rules
  - `r7rs-shim.eta`: module `conformance.r7rs-shim` mapping chibi's test vocabulary onto
    `std.test`:
    - `(test expected expr)` → `assert-equal` (which uses `equal?`)
    - `(test-error expr)` → `catch`, asserting a runtime-error payload
    - `(test-assert expr)` → `assert-true`
    - `(test-begin name)` and `(test-end)` → group construction
    - `(test-skip reason ...)` marks known-unsupported cases, so they are reported, not silently
      dropped
  - `sec-04-expressions.test.eta`, `sec-06-02-numbers.test.eta`, … with one file per R7RS report
    section, each a module that imports the shim and defines `suite`
  - `status.md`: the scorecard (generated, see B3)
- **Runner:** reuse `eta_test` (as `eta_stdlib_tests` does in
  `tools/test_runner/CMakeLists.txt`). Register a CTest entry
  `eta_r7rs_conformance` with `--path` pointing at the stdlib and the conformance directory.
- **Two execution modes:** run the suite once through the Driver (the default, with
  specialisation) and once through `etac -O` compiled output (reusing the
  `cookbook/compiled_example_tests.cpp` pattern). The numerics work showed the two paths can
  disagree.

### Phase B1: Port

Port section by section, in this order: the core sections first, then the ones that exercise the
numerics work.

| Order | R7RS section | Content |
|---|---|---|
| 1 | 4.1–4.3 | `quote`, `lambda`, `if`, `set!`, `cond`, `case`, `and`/`or`, `when`/`unless`, `let` family, `do`, named `let`, `delay`/`force` |
| 2 | 6.1 | `eqv?`, `eq?`, `equal?` |
| 3 | 6.2 | numbers. Cross-check against `qa/test/src/numeric_tests.cpp`; bignum, rational and complex cases become `test-skip` with the reason "numeric tower: no bignums/rationals" |
| 4 | 6.3–6.4 | booleans, pairs and lists (`list-tail`, `memq`/`member`/`assoc` with the optional compare, `list-copy`) |
| 5 | 6.5–6.7 | symbols, characters, strings (case folding, `string-copy!`, `string-fill!`, Unicode) |
| 6 | 6.8–6.9 | vectors, bytevectors |
| 7 | 6.10 | control: `apply`, `map`/`for-each` with multiple lists, `call/cc`, `values`, `dynamic-wind` |
| 8 | 6.11 | exceptions: `with-exception-handler`, `raise`, `raise-continuable`, `error-object?` (map these onto Eta's `catch`/`raise` where semantics match; record the divergences) |
| 9 | 4.2.6 / 4.3 | `define-record-type`, `syntax-rules` (hygiene, ellipsis depth, literals) |
| 10 | 6.13 | ports and I/O (string ports, `read`, `write` with shared structure). Expect the most skips here |

- **Porting rules:**
  - Only the surrounding syntax changes (module wrapper, test vocabulary). Expected values and
    expressions stay exactly as in the source.
  - Any edit to an expression is marked `;; ported:` with the reason.
  - `define-library` and R7RS `import` forms are Eta-divergent. They are recorded once in
    `status.md`, not per test.

### Phase B2: Triage

- **For each failure, open one entry in `status.md`** with the section, the test and the
  classification (bug, unsupported or divergent). Bugs list:
  - the expected vs actual value
  - whether it differs between the Driver mode and the `etac -O` mode
  - a one-line root-cause note once known
- **Fix in priority order:**
  1. wrong answers in core forms and lists
  2. wrong answers in `equal?`/`eqv?`/strings/chars
  3. control (`call/cc`, `dynamic-wind`, `values`)
  4. the rest
- **Each fix gets a regression test** in the relevant `qa/test/src/*_tests.cpp` or
  `stdlib/tests/*.test.eta`. The conformance file shows that a behaviour is correct; the unit
  test pins down why it broke.

### Phase B3: Scorecard

- **Generate a summary:** `eta_test` (or a small script beside it) writes per-section
  pass/fail/skip counts to `qa/conformance/r7rs/status.md`.
- **CI gate:** the pass count must never decrease; new skips need a reason; a fix moves its
  entry from bug to pass.
- **Headline number:** report "N of M applicable assertions pass", where skipped known-unsupported
  features are excluded from M but listed. That's the honest answer to how close Eta is to
  R7RS-small.

### Phase B4 (later): Numeric tower decision

- Use the B3 count of skips under "numeric tower" to decide whether bignums and exact rationals
  are worth implementing, or should stay a documented divergence. That's a separate plan.

### Done when

- Every R7RS section in the table has been ported, and the suite runs in CI in both modes.
- No conformance failure remains unclassified in `status.md`.
- The headline score is recorded, with the date and commit.
