# std.bitset Plan

[Back to README](../../README.md) ·
[Stdlib Reference](../stdlib.md) ·
[Architecture](../architecture.md)

> **Status.** Authoritative implementation plan for the `std.bitset`
> standard-library module and its supporting runtime primitives. An
> implementer should be able to execute this plan end-to-end without
> any other planning document.

---

## 1) Goals and non-goals

### Goals

1. Provide an idiomatic, fixed-capacity **bitset** data structure under
   `std.bitset`, suitable for masking, set membership over small integer
   universes, bitmap indices, and CLP-adjacent uses.
2. Expose a fast, allocation-free **64-bit fast path**: `popcount64`,
   `ctz64`, `clz64`, `bit-and64`, `bit-or64`, `bit-xor64`, `bit-not64`,
   `shift-left64`, `shift-right64`, operating on plain integers — this
   is the most common use (masks, flags).
3. Cover the standard bitset surface: construction, predicates,
   accessors, mutation + pure-functional set algebra, popcount/ctz/clz,
   first/next-set scans, shifts, fold/any/every iteration helpers.
4. Fit existing stdlib conventions: `(module std.bitset …)`, `?` for
   predicates, `!` for mutators, `%`-prefixed runtime builtins,
   matching test layout under `stdlib/tests/`, doc page under
   `docs/stdlib/`.

### Non-goals

1. No arbitrary-precision (growable) bitsets in v1; capacity is fixed at
   construction. (A growable variant can be a v2 add-on.)
2. No SIMD/AVX intrinsics; the C++ side uses portable
   `std::popcount` / `std::countl_zero` / `std::countr_zero` (C++20).
3. No bitset-of-symbols / interned-set semantics; that's `std.hashset`'s
   job. `std.bitset` indexes by non-negative integer position only.
4. No persistent (structural-sharing) variant; mutating ops alias, pure
   ops copy.

---

## 2) Where this slots in the codebase

| Concern                         | Path                                                  | Action |
| ------------------------------- | ----------------------------------------------------- | ------ |
| Module source                   | `stdlib/std/bitset.eta`                               | NEW    |
| Test suite                      | `stdlib/tests/bitset.test.eta`                        | NEW    |
| Reference doc                   | `docs/stdlib/bitset.md`                               | NEW    |
| Stdlib index                    | `docs/stdlib.md`                                      | EDIT   |
| Prelude re-export (optional)    | `stdlib/std/prelude.eta`                              | EDIT   |
| Runtime primitives header       | `eta/core/src/eta/runtime/bit_primitives.h`           | NEW    |
| Primitive registration glue     | `eta/core/src/eta/runtime/core_primitives.h` (or driver) | EDIT |
| Analysis-only name table        | `eta/core/src/eta/runtime/builtin_names.h`            | EDIT   |
| Reader/expander known-symbols   | `eta/core/src/eta/reader/expander.cpp`                | EDIT (if needed for known-name list) |
| Stdlib build script             | `scripts/build_stdlib_etac.py`                        | NONE — auto-discovers `*.eta` |
| CMake                           | `CMakeLists.txt`, `eta/core/CMakeLists.txt`           | NONE — header-only addition follows existing pattern |

Other stdlib modules (`std.hashset`, `std.csv`, `std.regex`,
`std.fact_table`) follow the same pattern: thin `.eta` wrappers over
`%`-prefixed builtins registered in `core_primitives.h` and mirrored in
`builtin_names.h` so the LSP/semantic analyser knows about them.

---

## 3) Internal representation

### 3.1 Backing storage

A bitset is a record-like vector containing:

1. A small header (sentinel symbol so `bitset?` can recognise it
   without a record system, mirroring how `std.hashset` works).
2. The declared **bit-length** `N` (integer).
3. A **vector of 64-bit words** holding `ceil(N / 64)` words, low bit of
   word 0 = bit index 0.

Layout (Scheme-level): `#(<bitset-tag> <N> <words-vector>)`.

`<bitset-tag>` is the symbol `'%bitset` (same trick `std.atom` and
`std.fact_table` use to brand records). Mutating ops mutate the inner
words vector in place; pure ops copy it.

### 3.2 Why 64-bit words

1. Hardware popcount (`POPCNT`, `CNT`) is 64-bit on every supported
   target.
2. The user has called out 64-bit popcount as the dominant case;
   matching the underlying word width keeps the fast path zero-overhead.
3. Eta's fixnum representation already uses 64-bit `int_val`, so a
   "word" round-trips through the VM without truncation.

### 3.3 High-bit mask

When `N` is not a multiple of 64, the **last** word has unused high
bits. Invariant: those bits are always zero. All mutating ops MUST mask
the tail word after the operation:

```
tail_mask = (N mod 64 == 0) ? ~0 : ((1 << (N mod 64)) - 1)
words[last] &= tail_mask
```

This keeps `popcount`, `equal?`, `bitset-full?`, etc. correct without
per-bit conditionals.

### 3.4 Standalone 64-bit fast path

Bare integer ops are exposed alongside the bitset record so callers who
just want a flag mask never allocate:

```
popcount64       :: int -> int        ; Hamming weight, bits 0..63
ctz64            :: int -> int        ; count trailing zeros, 64 if zero
clz64            :: int -> int        ; count leading zeros, 64 if zero
bit-and64        :: int int -> int
bit-or64         :: int int -> int
bit-xor64        :: int int -> int
bit-not64        :: int -> int        ; ~x masked to 64 bits
bit-shift-left64 :: int int -> int    ; masked to 64 bits
bit-shift-right64 :: int int -> int   ; logical, fills with 0
bit-test64       :: int int -> bool   ; (bit-test64 x i) = ((x >> i) & 1)
bit-set64        :: int int -> int
bit-clear64      :: int int -> int
bit-flip64       :: int int -> int
```

These are the **only** symbols a user normally needs for "I have a
64-bit mask, count its set bits".

---

## 4) Runtime primitives (C++ side)

New header `eta/core/src/eta/runtime/bit_primitives.h` follows the
shape of `core_primitives.h` and registers:

```
%bit-and          (2, false)   ; logical AND of two fixnums
%bit-or           (2, false)
%bit-xor          (2, false)
%bit-not          (1, false)   ; one's complement, masked to 64 bits
%bit-shift-left   (2, false)   ; (x, k) -> (x << k) & 0xFFFFFFFFFFFFFFFF
%bit-shift-right  (2, false)   ; logical right shift
%popcount         (1, false)   ; std::popcount on uint64_t view
%ctz              (1, false)   ; std::countr_zero, returns 64 for 0
%clz              (1, false)   ; std::countl_zero, returns 64 for 0
```

Implementation notes:

1. Reinterpret the fixnum's `int_val` as `uint64_t` for shift / not /
   popcount (keeps the bit pattern intact for negative inputs treated
   as two's-complement masks).
2. Shift counts outside `[0, 64]` raise a `RuntimeError` with
   `RuntimeErrorCode::TypeError` (mirrors `modulo`'s div-by-zero check).
3. Use `std::popcount` (`<bit>`, C++20) — already in the project's C++
   standard since `core_primitives.h` uses C++20 features (`std::expected`).
4. Register in `register_core_primitives` immediately after the existing
   numeric block (`modulo`/`remainder`), and **mirror the order** in
   `builtin_names.h` between the `r("remainder", …)` line and the
   transcendentals (`r("sin", …)`).

### 4.1 Multi-word primitives (C++ fast path)

It would be self-defeating to pull in `<bit>` for hardware-issued
`POPCNT` / `CTZ` / `CLZ` and then bury them under a per-word
interpreted loop that allocates a fresh fixnum each iteration. The
inner-loop bulk ops are therefore implemented in C++ from day one,
operating directly on the underlying word vector. They live in
`bit_primitives.h` next to the scalar ops and are registered as part
of the same block:

```
%bitset-words-and!         (2, false)   ; (dst, src) -> dst, in place AND
%bitset-words-or!          (2, false)
%bitset-words-xor!         (2, false)
%bitset-words-andnot!      (2, false)
%bitset-words-not!         (2, false)   ; (dst, tail-mask) -> dst
%bitset-words-copy!        (2, false)   ; (dst, src) -> dst
%bitset-words-equal?       (2, false)   ; word-wise vector equality
%bitset-words-popcount     (1, false)   ; sum of std::popcount over all words
%bitset-words-ctz          (2, false)   ; (words, capacity) -> trailing zeros
%bitset-words-clz          (2, false)   ; (words, capacity) -> leading zeros
%bitset-words-first-set    (1, false)   ; index of lowest set bit, or -1
%bitset-words-next-set     (2, false)   ; (words, start) -> index or -1
%bitset-words-shift-left!  (3, false)   ; (dst, src, k); dst may alias src
%bitset-words-shift-right! (3, false)
```

Implementation notes:

1. Each `%bitset-words-*` takes Eta vectors of fixnum words (the
   `words-vector` slot of the bitset record from §3.1). The C++ side
   reinterprets each fixnum's `int_val` as `uint64_t`, performs the
   bulk operation against a contiguous stack buffer or in place, and
   writes the results back. This is one allocation per call (the
   result vector, when not mutating) instead of one per word.
2. Loops are written so the optimiser can vectorise them: a tight
   `for (size_t i = 0; i < n; ++i) dst[i] = a[i] OP b[i];` over
   `uint64_t*`. With `-O2` and AVX2/NEON enabled, MSVC/Clang/GCC will
   emit packed 256-bit ops; `std::popcount` reductions lower to
   `VPOPCNTQ` where available and to a chained `POPCNT` otherwise.
3. The mutating variants (`…!`) require equal vector lengths and
   raise `RuntimeErrorCode::TypeError` on mismatch — the `.eta`
   wrapper has already enforced capacity equality, so this is a
   defensive check.
4. `%bitset-words-not!` takes the tail-mask (§3.3) as its second
   argument so the C++ side can apply it to the last word in the
   same pass and preserve the high-bit-zero invariant without a
   second round-trip.
5. `%bitset-words-shift-left!` / `…-shift-right!` implement the
   word-shift + bit-shift algorithm from §6.4 in C++; they handle
   `k == 0` and `bit-shift == 0` specially to avoid the undefined
   `>> 64`. They permit `dst == src` aliasing and process words in
   the safe direction accordingly.
6. `%bitset-words-first-set` / `…-next-set` return `-1` (a fixnum)
   for "no set bit"; the `.eta` wrapper translates that to `#f` so
   the public API matches §5.6.

The scalar `%bit-*`, `%popcount`, `%ctz`, `%clz` primitives from §4
remain — they back the standalone 64-bit fast path in §5.6 and the
single-bit `bitset-set!` / `-clear!` / `-flip!` mutators where the
overhead of crossing into C++ for one word is not worth a dedicated
primitive.

---

## 5) Public API surface

All names live in the `std.bitset` module. Predicates end in `?`,
mutators end in `!`. Pure variants return a fresh bitset and never
mutate.

### 5.1 Construction

| Procedure                          | Signature                              | Doc |
| ---------------------------------- | -------------------------------------- | --- |
| `make-bitset n`                    | `int -> bitset`                        | All-zero bitset of capacity `n` (n ≥ 0). |
| `bitset n . indices`               | `int int … -> bitset`                  | Capacity `n`, with each given index pre-set. |
| `bitset-from-integer n word`       | `int int -> bitset`                    | Capacity `n`, low `min(n,64)` bits taken from `word`. |
| `bitset-from-list n indices`       | `int (list int) -> bitset`             | Like `bitset` but accepts a list. |
| `bitset-copy bs`                   | `bitset -> bitset`                     | Independent copy. |

### 5.2 Predicates

| Procedure                       | Signature                  | Doc |
| ------------------------------- | -------------------------- | --- |
| `bitset? x`                     | `any -> bool`              | True iff `x` is a bitset. |
| `bitset-empty? bs`              | `bitset -> bool`           | All bits zero. |
| `bitset-full? bs`               | `bitset -> bool`           | All bits one (within capacity). |
| `bitset=? a b`                  | `bitset bitset -> bool`    | Same capacity and same bits. |
| `bitset-subset? a b`            | `bitset bitset -> bool`    | Every bit set in `a` also set in `b`. |
| `bitset-disjoint? a b`          | `bitset bitset -> bool`    | No bit set in both. |

### 5.3 Accessors

| Procedure                       | Signature                  | Doc |
| ------------------------------- | -------------------------- | --- |
| `bitset-length bs`              | `bitset -> int`            | Capacity in bits, **not** popcount. |
| `bitset-ref bs i`               | `bitset int -> bool`       | Bit at index `i`; error if out of range. |
| `bitset->integer bs`            | `bitset -> int`            | Bits 0..63 packed into a fixnum (errors if `bitset-length > 64`). |
| `bitset->list bs`               | `bitset -> (list int)`     | Ascending indices of set bits. |
| `bitset->string bs`             | `bitset -> string`         | Big-endian-display string of `0`/`1`, length = `bitset-length`. |

### 5.4 Mutation and pure variants

For each `op` ∈ {`set`, `clear`, `flip`}:

| Mutating                 | Pure                     | Signature                | Doc |
| ------------------------ | ------------------------ | ------------------------ | --- |
| `bitset-set! bs i`       | `bitset-set bs i`        | `bitset int -> bitset`   | Set bit `i` to 1. |
| `bitset-clear! bs i`     | `bitset-clear bs i`      | `bitset int -> bitset`   | Clear bit `i`. |
| `bitset-flip! bs i`      | `bitset-flip bs i`       | `bitset int -> bitset`   | Toggle bit `i`. |

### 5.5 Bulk set operations

For each `op` ∈ {`and`, `or`, `xor`, `andnot`}:

| Mutating                  | Pure                  | Signature                       |
| ------------------------- | --------------------- | ------------------------------- |
| `bitset-and! a b`         | `bitset-and a b`      | `bitset bitset -> bitset`       |
| `bitset-or!  a b`         | `bitset-or  a b`      | `bitset bitset -> bitset`       |
| `bitset-xor! a b`         | `bitset-xor a b`      | `bitset bitset -> bitset`       |
| `bitset-andnot! a b`      | `bitset-andnot a b`   | `bitset bitset -> bitset`       |
| `bitset-not! bs`          | `bitset-not bs`       | `bitset -> bitset`              |

All bulk ops require **equal capacity**; mismatched sizes raise
`error` with the message
``"std.bitset: capacity mismatch (got <a> vs <b>)"``.

### 5.6 Bit counting and scanning

| Procedure                         | Signature                | Doc |
| --------------------------------- | ------------------------ | --- |
| `popcount x`                      | `int -> int`             | Hamming weight of low 64 bits of `x`. **Fast path** — wraps `%popcount`. |
| `popcount64 x` *(alias)*          | `int -> int`             | Explicit 64-bit name; identical. |
| `ctz64 x`                         | `int -> int`             | Trailing-zero count, 64 if zero. |
| `clz64 x`                         | `int -> int`             | Leading-zero count, 64 if zero. |
| `bitset-popcount bs`              | `bitset -> int`          | Total set bits across all words. |
| `bitset-count-leading-zeros bs`   | `bitset -> int`          | Number of leading zeros up to `bitset-length`. |
| `bitset-count-trailing-zeros bs`  | `bitset -> int`          | Number of trailing zeros, or `bitset-length` if empty. |
| `bitset-first-set bs`             | `bitset -> int or #f`    | Index of lowest set bit, or `#f` if empty. |
| `bitset-next-set bs i`            | `bitset int -> int or #f`| Lowest set index ≥ `i`, or `#f`. |
| `bitset-for-each-set-bit bs proc` | `bitset (int -> any) -> void` | Calls `proc` on each set index in ascending order. |

### 5.7 Shifts

| Procedure                   | Signature                | Doc |
| --------------------------- | ------------------------ | --- |
| `bitset-shift-left bs k`    | `bitset int -> bitset`   | Logical left shift by `k`; new bits cleared; bits shifted past `bitset-length` discarded. |
| `bitset-shift-right bs k`   | `bitset int -> bitset`   | Logical right shift by `k`; high bits cleared. |

Both must correctly handle `k ≥ 64` (cross-word) and `k > bitset-length`
(returns empty). Mutating variants `bitset-shift-left!`,
`bitset-shift-right!` provided alongside.

### 5.8 Iteration helpers

| Procedure                   | Signature                                  | Doc |
| --------------------------- | ------------------------------------------ | --- |
| `bitset-fold bs f init`     | `bitset (acc int -> acc) acc -> acc`       | Left fold over set indices. |
| `bitset-any? bs pred`       | `bitset (int -> bool) -> bool`             | True if `pred` holds for any set index. |
| `bitset-every? bs pred`     | `bitset (int -> bool) -> bool`             | True if `pred` holds for every set index. |

---

## 6) Algorithm notes

### 6.1 popcount

1. **Primitive path:** `%popcount` calls `std::popcount(uint64_t)`.
   On any modern x86-64 / ARMv8 build this lowers to a single
   instruction.
2. **Fallback if the builtin is ever unavailable** (e.g. analysis mode):
   the SWAR Hamming-weight algorithm in pure Eta:
   ```
   x -= (x >> 1) & 0x5555555555555555
   x  = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333)
   x  = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0F
   return (x * 0x0101010101010101) >> 56
   ```
   Implemented in `bitset.eta` as `%popcount-swar` and used only when
   `%popcount` is absent at link time. The dispatch is a single
   `if (procedure? %popcount)` at module load.

### 6.2 ctz / clz

1. **Primitive path:** `std::countr_zero` / `std::countl_zero` from
   `<bit>`. Both return `64` for input `0`, matching our spec.
2. **Fallback ctz (SWAR):** isolate lowest bit with `x & -x`, then
   `popcount(x & -x) - 1`. Or branch-free de-Bruijn lookup.
3. **Fallback clz:** binary search on word halves (5 compare-shift
   rounds). Both fallbacks live alongside the popcount fallback.

### 6.3 Multi-word scan

`bitset-next-set bs i`:

1. Let `w = i / 64`, `b = i mod 64`.
2. Mask off the low `b` bits of `words[w]`; if non-zero, return
   `w*64 + ctz(masked)`.
3. Else loop forward through subsequent words; first non-zero word `w'`
   gives `w'*64 + ctz(words[w'])`.
4. Return `#f` if no word past `w` has a set bit, or if the index would
   exceed `bitset-length`.

### 6.4 Multi-word shift

For `bitset-shift-left bs k`:

1. `word-shift = k / 64`, `bit-shift = k mod 64`.
2. From high word to low word, write
   `out[i] = (in[i - word-shift] << bit-shift) | (in[i - word-shift - 1] >> (64 - bit-shift))`
   with bounds-checked reads returning `0`.
3. Special-case `bit-shift == 0` to skip the right-shift (which would
   be `>> 64`, undefined in C and trapped in our `%bit-shift-right`).
4. Mask the tail word per §3.3.

`bitset-shift-right` is the mirror image.

---

## 7) Error handling

Conventions match other stdlib modules (which use `(error "msg" …)`):

1. **Out-of-range index**: `bitset-ref`, `bitset-set!`, `bitset-clear!`,
   `bitset-flip!` raise
   `"std.bitset: index <i> out of range [0, <N>)"`.
2. **Capacity mismatch** for binary ops: see §5.5.
3. **Negative capacity / negative shift**: raised on construction /
   shift call with
   `"std.bitset: <param> must be non-negative (got <v>)"`.
4. **Type errors**: raised by the underlying `%bit-*` primitive with
   `RuntimeErrorCode::TypeError`; the wrapper does not double-check.
5. **`bitset->integer` capacity > 64**: raises
   `"std.bitset: bitset->integer requires capacity ≤ 64 (got <N>)"`.

No exceptions are caught inside the module; errors propagate via the
runtime's standard error channel.

---

## 8) Test plan

File: `stdlib/tests/bitset.test.eta`, registered automatically by the
test runner pattern used by every other module. Structure mirrors
`stdlib/tests/hashset.test.eta`: `(module bitset.tests (import std.test
std.bitset) …)` ending with `(print-tap (run suite))`.

### 8.1 Construction and predicates

1. `make-bitset 0` — empty, length 0, popcount 0, `bitset->list` is `'()`.
2. `make-bitset 1` — single bit zero; after `bitset-set! _ 0`, popcount 1.
3. `make-bitset 64` — exact word boundary; full after 64 sets.
4. `make-bitset 65` — straddles words; verify tail-mask invariant
   (`bitset-popcount` after `bitset-not` on empty bitset returns 65).
5. `make-bitset 1000` — multi-word; spot-check far-end indices.
6. `bitset-from-integer 64 0xFFFFFFFFFFFFFFFF` round-trips through
   `bitset->integer`.
7. `bitset-from-list 16 '(0 5 15)` → popcount 3, list round-trip.

### 8.2 64-bit fast path

1. `popcount 0` = 0.
2. `popcount 1` = 1.
3. `popcount 0xFFFFFFFFFFFFFFFF` = 64.
4. `popcount 0xAAAAAAAAAAAAAAAA` = 32 (alternating).
5. `popcount 0x5555555555555555` = 32.
6. `ctz64 0` = 64; `ctz64 1` = 0; `ctz64 0x80…00` = 63.
7. `clz64 0` = 64; `clz64 1` = 63; `clz64 0x80…00` = 0.
8. `bit-shift-left64 1 63` = `0x80…00`; `… 1 64` = 0 (masked).

### 8.3 Mutation and pure ops

1. `bitset-set!` mutates; `bitset-set` does **not** mutate the input
   (assert original popcount unchanged after calling pure variant).
2. `bitset-flip!` is its own inverse (apply twice ⇒ original).
3. Out-of-range index raises (use `assert-error` or the project's
   equivalent helper).

### 8.4 Bulk set ops

1. `bitset-and` of complement bitsets is empty; `bitset-or` is full.
2. `bitset-xor a a` is empty.
3. `bitset-andnot a b` ≡ `bitset-and a (bitset-not b)`.
4. Capacity mismatch raises.

### 8.5 Scans

1. `bitset-first-set` of empty = `#f`.
2. `bitset-first-set` after `set! _ 999` on a 1024-bit set = 999.
3. `bitset-next-set` correctly crosses word boundaries (set bits at
   63, 64, 127, 128 — verify each successor).
4. `bitset-for-each-set-bit` collects the same list as `bitset->list`.

### 8.6 Shifts

1. `bitset-shift-left bs 0` = identity.
2. Shift left by 1 of `bitset 64 0` has bit 1 set.
3. Shift left by 64 across word boundary on a 128-bit set with bit 0
   produces bit 64 set, popcount 1.
4. Shift left by `bitset-length` produces empty.
5. Shift right is the inverse of shift left within capacity.

### 8.7 Iteration helpers

1. `bitset-fold bs (lambda (acc i) (cons i acc)) '()` reverses
   `bitset->list`.
2. `bitset-any?`, `bitset-every?` short-circuit (verify with a
   counter-side-effect closure).

### 8.8 Properties (lightweight)

1. `bitset->list` then `bitset-from-list` round-trips.
2. `bitset->string` length always equals `bitset-length`.
3. Random sampled property: for random N ∈ [0, 256] and random bit
   patterns, `bitset-popcount = (length (bitset->list bs))`.

---

## 9) Documentation deliverables

### 9.1 New file `docs/stdlib/bitset.md`

Follows the shape of `docs/stdlib/hashset.md`:

1. One-line summary at the top.
2. "Synopsis" code block with three or four idiomatic examples
   (popcount of a mask, building a bitset of indices, set algebra,
   iterating set bits).
3. Section per category (Construction / Predicates / Accessors /
   Mutation / Bulk ops / Counting / Shifts / Iteration /
   64-bit fast path) listing each procedure with signature and
   one-sentence doc. Content sourced verbatim from §5 of this plan.
4. "Errors" section enumerating the messages from §7.
5. "Performance notes" calling out the 64-bit fast path and the
   `std::popcount` lowering.

### 9.2 Edit `docs/stdlib.md`

Insert under "Data structures":

```
- [std.bitset](stdlib/bitset.md) — Fixed-capacity bitsets and 64-bit
  bit-twiddling helpers (popcount, ctz, clz).
```

### 9.3 Optional: `stdlib/std/prelude.eta`

Add `(import std.bitset)` and re-export `popcount`, `popcount64`,
`ctz64`, `clz64`, `make-bitset`, `bitset?`, `bitset-popcount`,
`bitset-ref`, `bitset-set!`, `bitset-clear!`, `bitset-or`,
`bitset-and`, `bitset->list`, and the dependency line in the comment
header. (Prelude additions are common — see how `std.log` is included.)

---

## 10) Build / packaging touchpoints

1. **CMake:** No change. `bit_primitives.h` is a new header included by
   `core_primitives.h` (or by the runtime driver), the same way
   `csv_primitives.h` etc. are wired today. The library it lives in is
   already built.
2. **Stdlib build script:** No change. `scripts/build_stdlib_etac.py`
   recursively globs `stdlib/**/*.eta` (excluding `tests/`) and will
   pick up `bitset.eta` automatically.
3. **`builtin_names.h`:** add the eight `r("%bit-…", …)` and three
   `r("%popcount" / "%ctz" / "%clz", 1, false)` lines in the same
   relative position as the registration in `core_primitives.h`. The
   header has a strict ordering invariant; add at the documented
   numeric block (§4).
4. **Reader/expander known-symbols list** in `expander.cpp` (lines
   around the existing numeric word `"modulo","remainder",…`): include
   the new `%bit-*`/`%popcount`/`%ctz`/`%clz` if that list is
   significant. Verify by searching the file before editing.
5. **LSP / semantic analyser:** picks up the new names automatically
   via `register_builtin_names`. No further work.

---

## 11) Phased delivery roadmap

### M1 — Core representation and 64-bit fast path

1. Add `bit_primitives.h` with `%bit-and/or/xor/not/shift-left/shift-right`,
   `%popcount`, `%ctz`, `%clz`. Register in `core_primitives.h` and
   mirror in `builtin_names.h`.
2. Land `stdlib/std/bitset.eta` skeleton: tag/record helpers,
   `make-bitset`, `bitset?`, `bitset-length`, `bitset-ref`,
   `bitset-set!`, `bitset-clear!`, `bitset-flip!`, plus the bare
   `popcount`, `popcount64`, `ctz64`, `clz64`, `bit-and64`, etc.
3. Write tests covering §8.1, §8.2, §8.3.

Gate: `popcount 0xFFFFFFFFFFFFFFFF == 64` round-trips through the VM;
`bitset-set!`/`bitset-ref` work over a 1000-bit set.

### M2 — Bulk set operations

1. Implement `bitset-and!`/`-or!`/`-xor!`/`-andnot!`/`-not!` plus pure
   variants as thin wrappers over the `%bitset-words-*` primitives
   from §4.1; pure variants allocate via `%bitset-words-copy!` then
   apply the in-place op.
2. Implement `bitset=?`, `bitset-subset?`, `bitset-disjoint?`,
   `bitset-empty?`, `bitset-full?`.
3. Add `bitset-from-integer`, `bitset-from-list`, `bitset-copy`,
   `bitset->list`, `bitset->integer`, `bitset->string`, the variadic
   `bitset` constructor.
4. Tests §8.4.

Gate: De Morgan identities verified on randomised inputs.

### M3 — Scans and shifts

1. Implement `bitset-popcount`, `bitset-count-leading-zeros`,
   `bitset-count-trailing-zeros`, `bitset-first-set`,
   `bitset-next-set`, `bitset-for-each-set-bit`.
2. Implement `bitset-shift-left[!]`, `bitset-shift-right[!]` per §6.4.
3. Implement iteration helpers `bitset-fold`, `bitset-any?`,
   `bitset-every?`.
4. Tests §8.5, §8.6, §8.7.

Gate: word-boundary-crossing shifts and scans pass.

### M4 — Documentation and polish

1. Write `docs/stdlib/bitset.md`.
2. Update `docs/stdlib.md` index.
3. Optional prelude re-export.
4. Add property tests §8.8.
5. CI green; `eta_lint` (when available) clean on the new module.

Gate: documentation merged; module discoverable from the stdlib index.

### M5 *(optional)* — SIMD / hardware-specific tuning

The §4.1 multi-word primitives already compile to vectorised loops
under `-O2`. If profiling on a specific target shows headroom, this
phase introduces hand-tuned intrinsic paths (AVX-512 `VPOPCNTQ`,
ARM SVE) selected at runtime via CPU feature detection. Public API
and the C++ primitive surface stay unchanged; only the inner kernel
swaps.

---

## 12) Open questions and risks

1. **Fixnum width assumption.** This plan assumes `int_val` is exactly
   64-bit two's complement. Confirm by reading
   `eta/core/src/eta/runtime/value.h` (or equivalent) before M1; if
   fixnums are 63-bit tagged, the high-bit handling in `%bit-not` /
   `%bit-shift-left` and `popcount` needs adjusting (`std::popcount`
   on 63-bit values is still fine, but `bit-not64` of 0 must return
   `0xFFFFFFFFFFFFFFFF` even if that doesn't fit a tagged fixnum —
   we may need to widen to a boxed integer for the not/shift results,
   or document that ops are over 63-bit values).
2. **No record type system in stdlib.** `std.bitset` follows the
   `vector + tag symbol` brand pattern. If a `define-record-type`
   lands later, migrate `bitset?` / accessors to it without changing
   the public API.
3. **`assert-error` in `std.test`.** Confirm the helper exists; if
   not, M1 should add it (otherwise §8.3 third bullet collapses to
   "calling raises and the test framework reports the unhandled
   error" — acceptable but uglier).
4. **Naming collisions.** `popcount` is unprefixed for ergonomics
   (matching `gcd`, `lcm`, `square` in `std.math`). Risk that user
   code already binds `popcount`; mitigated because the module is
   opt-in via `(import std.bitset)`.
5. **Error message format.** This plan proposes
   `"std.bitset: …"` prefixes. Other modules vary
   (`hash-set-…`, `csv:…`). Decide on house style during M1; align
   the rest of the module to match.
6. **Prelude pollution.** Adding `popcount` / `ctz64` / `clz64` to
   `std.prelude` is convenient but globalises three new symbols. If
   that's too aggressive, leave them out of prelude and require an
   explicit `(import std.bitset)`.
7. **Endianness of `bitset->string`.** This plan picks
   big-endian display (bit `N-1` first), matching how integer literals
   are read. Confirm the desired display orientation before locking
   the docs.

---

## 13) Acceptance checklist

- [ ] `stdlib/std/bitset.eta` exists, exports every symbol in §5.
- [ ] `stdlib/tests/bitset.test.eta` exists; all cases in §8 pass under
      `etai`/CTest.
- [ ] `eta/core/src/eta/runtime/bit_primitives.h` registers eight
      `%bit-*` plus `%popcount` / `%ctz` / `%clz`.
- [ ] `builtin_names.h` mirrors the registration order.
- [ ] `popcount 0xFFFFFFFFFFFFFFFF` returns `64` from `etai`.
- [ ] A 1024-bit set with bits at 0, 63, 64, 127, 128, 1023 produces
      `(0 63 64 127 128 1023)` from `bitset->list`.
- [ ] Capacity-mismatch and out-of-range errors carry the documented
      messages.
- [ ] `docs/stdlib/bitset.md` written; `docs/stdlib.md` updated.
- [ ] Module imports cleanly with no warnings under the LSP.

