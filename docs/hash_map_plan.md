# Hash Map / Hash Set — Implementation Plan (Phase H2a)

[Back to README](../README.md) · [Architecture](architecture.md) ·
[Runtime and GC](runtime.md) · [Modules and Stdlib](modules.md) ·
[Optimisations](optimisations.md) · [Next Steps](next-steps.md)

---

## 1. Overview & Motivation

Eta currently models every dictionary-shaped value as an **association
list**: `((key . val) ...)` with `assoc` / `assq` / `cdr` lookup. This
is O(n) per access and O(n) per `cons`-style update. The capability
matrix in [next-steps.md](next-steps.md) flags this as the single
largest data-structure gap, and Phase H2 of the Hosted-Platform Layer
roadmap commits to closing it.

This document is the **implementation plan for the hash-map and
hash-set deliverables of Phase H2** only. The companion **Atom**
deliverable (CAS cell with `std::atomic<LispVal>` and trail
integration) shares the H2 phase but is intentionally **out of scope
here** — it has different concurrency, GC-barrier and trail concerns
and warrants its own plan.

Goal targets carried forward from `next-steps.md`:

| Item | Scope of this plan |
|---|---|
| Hash map (`make-hash-map`, `hash-map?`, `hash-map-ref`, `hash-map-assoc`, `hash-map-dissoc`, `hash-map-keys`, `hash-map-values`, `hash-map-size`, `hash-map->list`, `list->hash-map`) | ✅ in scope |
| `hash` generic over symbol / string / number / bytevector | ✅ in scope |
| Hash set (`make-hash-set`, `hash-set?`, `hash-set-add`, `hash-set-remove`, `hash-set-contains?`, `hash-set-union`, `hash-set-intersect`, `hash-set-diff`) | ✅ in scope |
| `std.hashmap`, `std.hashset` modules + tests + docs | ✅ in scope |
| Atom (`atom`, `deref`, `reset!`, `swap!`, `compare-and-set!`) | ❌ out of scope (separate H2 sub-plan) |
| Reader literal syntax `{…}` / `#{…}` | ❌ deferred (printer-only `#hashmap{…}` form considered) |
| HAMT / structural sharing / transients | ❌ deferred (v2 trigger criteria below) |

Total estimated effort: **~9 working days** across five milestones
(M1–M5).

---

## 2. Design Decisions

### 2.1 Persistence model — v1 = immutable open-addressing with full copy

- **v1** = an immutable hash map backed by a flat open-addressed
  key/value array. `hash-map-assoc` / `hash-map-dissoc` allocate a new
  buffer and full copy. This is O(n) per update but gives O(1)-amortised
  lookup and a tiny, auditable C++ surface (~250 LoC vs ~700 LoC for
  HAMT).
- The data structure is **immutable from the Eta side**. There is no
  `hash-map-set!`. Every update returns a new map. This matches Clojure
  semantics and means there is no GC write barrier or trail-entry to
  worry about.
- **v2 (deferred)** = Bagwell HAMT with structural sharing.
  **Upgrade trigger**: when the benchmark in M5 shows `hash-map-assoc`
  of a 10k-element map dominating any real workload (≥ 5 % of wall time
  in `eta_qp_bench` or in the upcoming `std.json` parser). Until then,
  full-copy is the right complexity/perf trade.

### 2.2 Equality semantics

- Key equality follows **`equal?`** — structural for cons, byte-wise
  for strings/bytevectors, tag-equal for symbols/fixnums/chars.
- Numeric subtlety: in v1 we treat `1` (Fixnum) and `1.0` (NaN-boxed
  double) as **distinct keys** (matches Clojure). The constraint is:
  *a key collides iff `equal?` says it does*. Document this clearly.
  A future `=`-keyed variant can come later without migration risk
  because the public API does not promise numeric coercion.
- Allowed key types: anything for which `hash` is defined — initially
  **symbol, string, fixnum, character, bytevector**, plus any
  `equal?`-comparable nested cons of those. Tensor / port / closure
  keys raise `(error 'hash-map "unhashable key" k)`.

### 2.3 `hash` generic

- One C++ entry point `uint64_t hash_value(LispVal, intern_table&)`
  dispatches on `Tag` / `ObjectKind`.
- **Strings & bytevectors**: SipHash-2-4 with a process-wide random
  seed established at `Driver` construction time (defends against
  HashDoS for hosted webhook workloads). xxHash3 is faster but adds a
  dependency; SipHash is ~120 LoC pure C++. Pick **SipHash-2-4**.
- **Symbols**: hash the interned `Symbol*` pointer mixed via
  `splitmix64`. Symbol equality is pointer equality so this is exact
  and cheap.
- **Fixnum / Char**: `splitmix64` of the 47-bit payload.
- **Double**: `bit_cast<uint64_t>` then `splitmix64`. NaN
  canonicalisation already happens at `nanbox::ops::encode<double>` so
  all NaNs hash equal.
- **Cons / nested**: recursive hash with a depth bound (default 8) to
  prevent pathological cyclic-data DoS.
- Expose `(hash x)` as a builtin returning a fixnum (low 47 bits of
  the 64-bit result).

### 2.4 Probing, load factor, tombstones

- **Open addressing with linear probing** (cache-friendly; v1 maps are
  small and short-lived, so the simplicity wins).
- **Tombstones** for deletes; rebuild on tombstone fraction > 25 %.
- **Load factor**: grow at 0.7, shrink at 0.15 (with a floor of 8
  slots).
- Robin Hood is *not* worth it at v1 size; revisit if HAMT migration
  is also deferred and average map size grows past ~10 k entries.

### 2.5 GC integration

A new `ObjectKind::HashMap` (and `HashSet`) is added to the enum in
`eta/core/src/eta/runtime/memory/heap.h` and the
`ETA_ENUM_TO_STRING` block. Concretely:

- New struct `eta::runtime::types::HashMap` in
  `eta/core/src/eta/runtime/types/hash_map.h`. Fields:
  `std::vector<LispVal> keys; std::vector<LispVal> values;
  std::vector<uint8_t> state;` (state ∈ {empty, occupied, tombstone});
  `std::size_t size; std::size_t tombstones; uint64_t seed;`.
- A new `visit_hash_map` method on `HeapVisitor` in
  `eta/core/src/eta/runtime/memory/heap_visit.h` and a corresponding
  `case HashMap:` in `visit_heap_object`. The mark traversal walks all
  `occupied` slots and recurses into both key and value LispVals via
  the existing visitor pattern (mirrors how `visit_fact_table` walks
  `columns`).
- `HashSet` reuses the same structure with `values` empty (or, cleaner,
  a separate `HashSet` struct that internally holds a `HashMap` with
  `values.empty()`). Recommended: **separate struct** for clarity,
  sharing inline helpers in `hash_table.h`.
- **Concurrency**: immutable maps are trivially safe for concurrent
  reads. The hazard-pointer read path in `Heap::try_get_as` already
  protects the `HeapEntry::ptr` lookup; the underlying `HashMap` is
  never mutated after construction so no further synchronisation is
  required.
- **Trail integration**: none. `hash-map-assoc` allocates a fresh map;
  the old map is unaffected so no `trail-mark` entry is needed for
  either bindings or attributes.

### 2.6 Reader / printer syntax

- **No new reader syntax in v1.** Adding `{…}` / `#{…}` literals would
  touch the lexer (`reader/`), the parser, and every grammar consumer
  (LSP, formatter, syntax highlighter, snippets). Instead expose the
  constructor `(hash-map k1 v1 k2 v2 …)` and `(hash-set k1 k2 …)`,
  parallel to `vector` / `bytevector`.
- **Printer**: extend `value_formatter.cpp` with a new branch that
  prints `#hashmap{k1 v1, k2 v2}` and `#hashset{k1 k2}`. These forms
  are **not** reader-roundtripable in v1 — document the
  `(hash-map …)` constructor as the canonical written form.
- A future Phase H2b can revisit literal syntax once we know real
  usage patterns; deferring it now is reversible.

### 2.7 Equality of hash maps themselves

- Two hash maps are `equal?` iff their (size, key-set) match and
  `equal?` on each value pair. Implementation: iterate the smaller,
  look up in the larger.
- Hash maps **are themselves hashable**: hash = XOR of
  `(hash k) ^ rotl(hash v, 13)` over all entries. Order-independent.
  This unlocks nested-map keys later without an API break.

---

## 3. C++ Implementation Plan

### 3.1 Files to add / modify

| File | Action | Notes |
|---|---|---|
| `eta/core/src/eta/runtime/types/hash_map.h` | **new** | `struct HashMap { … }` + inline probing helpers `hm_find`, `hm_insert_no_grow`, `hm_grow`. Mirror style of `types/fact_table.h`. |
| `eta/core/src/eta/runtime/types/hash_set.h` | **new** | Wraps a `HashMap` with no values. |
| `eta/core/src/eta/runtime/types/types.h` | edit | Add `#include "hash_map.h"` and `#include "hash_set.h"`. |
| `eta/core/src/eta/runtime/memory/heap.h` | edit | Add `HashMap`, `HashSet` to `ObjectKind` enum and `ETA_ENUM_TO_STRING` block. |
| `eta/core/src/eta/runtime/memory/heap_visit.h` | edit | Add `visit_hash_map`, `visit_hash_set` virtuals + dispatch cases. |
| `eta/core/src/eta/runtime/factory.h` | edit | Add `make_hash_map` / `make_hash_set` mirroring `make_fact_table`. |
| `eta/core/src/eta/runtime/core_primitives.h` | edit | Register the builtins — see §3.2. |
| `eta/core/src/eta/runtime/value_formatter.cpp` | edit | Add print branches for `HashMap`, `HashSet`. |
| `eta/core/src/eta/runtime/vm/sandbox.cpp` | edit | Extend `values_equal` to recurse into hash-map contents. |
| `eta/core/src/eta/runtime/builtin_names.h` | edit | Append the new builtin names so SemanticAnalyzer sees them. |
| `eta/core/src/eta/runtime/util/siphash.h` | **new** | ~120 LoC SipHash-2-4. Header-only. |
| `eta/core/test/src/...` | **new** test files | Unit tests for the table itself. See §7. |

### 3.2 Builtin registration (mirror the vector pattern)

Existing template in `core_primitives.h`:

```
env.register_builtin("vector", 0, true,
    [&heap](Args args) -> std::expected<LispVal, RuntimeError> { … });
```

New registrations (signatures only — see file for argument-decoding
helpers):

| Name | Arity | Rest | Notes |
|---|---|---|---|
| `hash-map` | 0 | true | k1 v1 k2 v2 … — error on odd arity |
| `make-hash-map` | 0 | false | empty map (capacity 8) |
| `hash-map?` | 1 | false | type predicate |
| `hash-map-ref` | 2 | true | optional default; otherwise raises `(error 'hash-map-ref "key not found" k)` |
| `hash-map-assoc` | 3 | false | returns new map |
| `hash-map-dissoc` | 2 | false | no-op if key absent |
| `hash-map-keys` | 1 | false | list, insertion-order undefined |
| `hash-map-values` | 1 | false | list |
| `hash-map-size` | 1 | false | fixnum |
| `hash-map->list` | 1 | false | `((k . v) …)` for round-trip with alists |
| `list->hash-map` | 1 | false | accepts either `((k . v) …)` or flat list |
| `hash-map-fold` | 3 | false | `(fn acc k v) → acc` — added to avoid materialising key list |
| `hash` | 1 | false | generic dispatch |
| `make-hash-set` | 0 | false | empty set |
| `hash-set` | 0 | true | k1 k2 … |
| `hash-set?` | 1 | false | |
| `hash-set-add` | 2 | false | returns new set |
| `hash-set-remove` | 2 | false | |
| `hash-set-contains?` | 2 | false | |
| `hash-set-union` | 2 | false | |
| `hash-set-intersect` | 2 | false | |
| `hash-set-diff` | 2 | false | |
| `hash-set->list` | 1 | false | |
| `list->hash-set` | 1 | false | |

### 3.3 PrimitiveSpecialisation

Out of scope for v1. The optimiser pipeline
([optimisations.md](optimisations.md)) currently specialises
`+ - * / Eq Cons Car Cdr` to dedicated VM opcodes. `hash-map-ref` is a
strong future candidate (it dominates JSON-config workloads), but
requires PrimitiveSpecialisation to know the kind of its first argument
— that is a separate optimiser feature (flow-sensitive type
specialisation, already on the optimisation backlog). **Do not block
v1 on it.**

### 3.4 Concurrency

Immutable maps are inherently thread-safe; document this in the
hashmap reference. No mutex, no atomic, no trail entry. The only
concurrency invariant is the existing one: the `HeapEntry::ptr` cannot
be freed while a hazard pointer protects it (handled by
`memory/hazard_ptr.h`).

---

## 4. Stdlib Plan

### 4.1 Modules

Following the house style of `stdlib/std/csv.eta` and
`stdlib/std/regex.eta`:

- **`stdlib/std/hashmap.eta`** — `(module std.hashmap (export …)
  (begin …))`. Re-exports the C++ builtins under their canonical
  names plus convenience wrappers:
  - `(hash-map-empty? m)` → `(= 0 (hash-map-size m))`
  - `(hash-map-contains? m k)` → uses a sentinel default to disambiguate
    "missing" from "value is `#f`".
  - `(hash-map-update m k fn)` →
    `(hash-map-assoc m k (fn (hash-map-ref m k)))`.
  - `(hash-map-update-with-default m k fn default)` → same with default.
  - `(hash-map-merge m1 m2)` → m2 wins on conflict; uses
    `hash-map-fold`.
  - `(hash-map-merge-with f m1 m2)` → reducer on conflict.
  - `(hash-map-map fn m)`, `(hash-map-filter pred m)`.
  - `(hash-map->alist m)`, `(alist->hash-map al)` — friendlier names
    alongside the lower-level `…->list`.

- **`stdlib/std/hashset.eta`** — same pattern. Adds:
  - `(hash-set-empty? s)`, `(hash-set-size s)`,
    `(hash-set-subset? a b)`, `(hash-set-equal? a b)`.

- **No prelude exposure in v1.** Keep the names module-qualified to
  avoid colliding with future `(module …)` design changes;
  `(import std.hashmap)` is the entry point.

### 4.2 Test files

- `stdlib/tests/hashmap.test.eta`
- `stdlib/tests/hashset.test.eta`

Following the structure of `stdlib/tests/csv.test.eta`. Coverage
matrix:

| Test | hashmap | hashset |
|---|---|---|
| Empty constructor / size / `?` predicate | ✓ | ✓ |
| Round-trip `list->X` ∘ `X->list` | ✓ | ✓ |
| Key types: symbol, string, fixnum, char, bytevector | ✓ | ✓ |
| `equal?` between two maps with same content | ✓ | ✓ |
| `1` and `1.0` are distinct keys (documents semantics) | ✓ | — |
| Dissoc / remove of missing key is identity | ✓ | ✓ |
| Collision behaviour: insert 1000 keys with crafted collisions | ✓ | ✓ |
| Sanity timing: `(time …)` 10k inserts then 10k lookups under 50 ms | ✓ | ✓ |
| `hash-map-fold` early-aggregate equivalence to `(map …)` over keys | ✓ | — |
| `hash-set-union`, `intersect`, `diff` algebraic identities | — | ✓ |
| Nested map equality (map containing map) | ✓ | — |
| Tombstone rebuild correctness — 1000 add/remove churn | ✓ | ✓ |
| Error path: unhashable key (closure) raises | ✓ | ✓ |
| GC correctness: allocate-discard-allocate inside a loop, no growth | ✓ | ✓ |

Tests are auto-discovered by `eta_test` and run via
`ctest -R eta_stdlib_tests`.

---

## 5. Refactoring Opportunities

Findings from `grep -E '\b(assoc|assq|assv|alist)\b'` over `stdlib/`
and `examples/`. Each row = real usage site, not a guess.

| Site | Current pattern | Why it's slow / awkward | Recommended action | Phase |
|---|---|---|---|---|
| `stdlib/std/db.eta` — `*db-table-status*`, `*db-table-answers*`, `alist-get`, `alist-set` | global mutable alists rebuilt on every status update | O(n) per status read; full re-cons per write; iterating SLG-lite tabling is the hot path | Replace with two `std.hashmap` cells (or, post-Atom, two `atom`s of hash-maps). Largest single win in stdlib. | M3 |
| `stdlib/std/csv.eta` — `csv:opts->alist` and `(assq 'column-names …)` lookups | options list is small (≤10 entries) so alist is fine | nominal | **Leave as alist.** Document the decision. | — |
| `stdlib/std/regex.eta` — named-group lookup `(assoc name (__regex-match-named-groups m))` | rebuilt per match | most regexes have 1–4 named groups → alist is fine; but exposing a `(regex:match-groups m)` returning a `hash-map` is more ergonomic | Add a `regex:match-groups-hash` accessor in `std.regex`; keep alist accessor for compat. | M3 |
| `stdlib/std/stats.eta` — OLS-multi result alist accessors | result is fixed shape (~7 keys); `assoc` is O(7) | not worth migrating | **Leave as alist.** | — |
| `stdlib/std/freeze.eta`, `stdlib/std/clpr.eta`, `stdlib/std/clpb.eta`, `stdlib/std/clp.eta` — `(put-attr v 'clp.prop (cons thunk pending))` / `(get-attr …)` | the *value* of an attribute is currently a list of pending thunks | the **attribute store itself** (in C++) is keyed by symbol per logic var — already hash-shaped at C++ level; the *Eta-side* value (list of pending thunks) is fine to leave as a list | **Leave as list of thunks.** Open a follow-up issue if propagator counts ever exceed 100/var. | — |
| `stdlib/std/db.eta` — `(assoc t env)` substitution lookup inside the unification loop | called at every term walk | already O(1)-amortised hash-map at C++ level; the Eta-level walk is just bookkeeping | **Leave;** the C++ walk already uses a hash. | — |
| `examples/xva-wwr/wwr-causal.eta` — `assoc-num`, `betas`, `mechanisms`, `interventions`, `acc` accumulators all alist-shaped | 50–500 entries per simulation step in a hot loop | clear win; migrate to `std.hashmap` and demonstrate measured speedup in the benchmark | Migrate as part of M4. Doubles as the showcase notebook diff. | M4 |
| `examples/xva-wwr/xva.eta` — `hazards`, `recoveries` per-counterparty alists | small (~10 counterparties) | nominal | Optional migration; mostly demonstrative. | M4 |
| `examples/xva-wwr/symbolic.eta` — `(assoc-ref expr env)` in symbolic evaluator | env grows with binding depth | mid-size win | Migrate; serves as hash-map demo in `examples/symbolic-diff.eta` adjacent file. | M4 |
| `examples/xva-wwr/report.eta` — `assoc-ref` over a small artifact alist | small fixed shape | nominal | **Leave.** | — |

`grep` returned **no** alist usage in `std.supervisor`, `std.logic`,
or examples like `nqueens.eta` / `boolean-simplifier.eta` /
`unification.eta` / `portfolio.eta` / `causal_demo.eta` — they are
list/cons-shape, not dict-shape, and need no migration. The earlier
guess about `std.supervisor` having a child registry does **not**
match what's in the file; the supervisor uses `spawn-thread` handles
in lists.

---

## 6. Documentation Deliverables

| Doc | Action |
|---|---|
| `docs/hashmap.md` | **new**, user-facing reference: API table, examples, performance notes, "why no `{}` literals yet". Header bar matches existing pages (e.g. `docs/csv.md`). |
| `docs/modules.md` | edit — add `std.hashmap`, `std.hashset` to module table. |
| `docs/next-steps.md` | edit — when shipped, move the hashmap/hashset bullet from the H2 "Outstanding" list into the "Recently Completed" section. Atom remains outstanding. |
| `docs/release-notes.md` | edit — add the milestone entry following the existing dated-headline format. |
| `editors/.../snippets/eta.json` | edit — add snippets for `hash-map`, `hash-set`, `hash-map-fold`. Listed as outstanding work in next-steps "Snippets refresh". |
| `eta/jupyter/src/eta/jupyter/display.cpp` | edit — add a MIME bundle branch for `ObjectKind::HashMap` mirroring the FactTable branch. Recommended representation: collapsible HTML table for ≤ 50 entries, summary + first-N for larger. |
| VS Code Heap Inspector | edit — the inspector already enumerates `ObjectKind` via the `eta/heapSnapshot` DAP extension; the new kinds will surface automatically once the C++ enum has them. Verify the "kind label" table in the inspector script and add row entries. |

---

## 7. Testing & Benchmarking

### 7.1 C++ unit tests (`eta_core_test`)

Add `eta/core/test/src/.../hash_map_tests.cpp` with `doctest`-style
cases (matching the existing core test layout):

- Probing correctness with crafted collisions.
- Tombstone rebuild after high churn.
- Resize at load 0.7; shrink at 0.15.
- SipHash KAT (one or two known-answer vectors against the reference
  implementation).
- Hash distribution sanity: chi-square goodness-of-fit on 10 k random
  keys < 3σ from uniform.
- GC visitor reaches all keys & values (instrument the visitor, count
  visited LispVals).
- Stress: 1 M insert / dissoc cycles, no leak (verify
  `Heap::total_bytes` returns to baseline after `gc()`).

### 7.2 Stdlib tests (`eta_test` → `eta_stdlib_tests`)

See §4.2 matrix.

### 7.3 Benchmark — `eta/bench/src/hash_map_benchmark.cpp`

Mirrors `eta/bench/src/qp_benchmark.cpp`. Compares alist (`assoc`) vs
`hash-map-ref` at sizes 10, 100, 1 000, 10 000:

| Op | Measure |
|---|---|
| Insert N keys | wall-clock per op |
| Lookup hit (random key) | wall-clock per op |
| Lookup miss | wall-clock per op |
| `assoc` / `dissoc` churn (N inserts then N dissocs, alternating) | total wall + heap-bytes peak |
| Iterate all keys | wall-clock |

Add a PowerShell + bash wrapper `hash-bench.{ps1,sh}` parallel to
`qp-benchmark.{ps1,sh}`, with optional `--gate` that fails CI if
alist→hash-map crossover regresses below ~30 entries (the size at
which hash-map should always win lookup).

---

## 8. Rollout Phases / Milestones

| M | Scope | Effort |
|---|---|---|
| **M1 – C++ core** | `types/hash_map.h`, `types/hash_set.h`, `util/siphash.h`, `ObjectKind` additions, `HeapVisitor` extensions, `factory.h` helpers, value_formatter print branches, `values_equal` extension, builtin registration, builtin name table, C++ unit tests. | **3–4 days** |
| **M2 – Stdlib + docs** | `std/hashmap.eta`, `std/hashset.eta`, `tests/hashmap.test.eta`, `tests/hashset.test.eta`, `docs/hashmap.md`, snippets refresh, `modules.md` update. | **2 days** |
| **M3 – Internal refactor** | Migrate `std.db` `*db-table-status*` / `*db-table-answers*`; add `regex:match-groups-hash`. | **2 days** |
| **M4 – Examples & notebook** | Migrate `examples/xva-wwr/wwr-causal.eta`, `xva.eta`, `symbolic.eta` to hash-maps. Add `examples/notebooks/hash-map-tour.ipynb` showing alist-vs-hashmap timing. | **1 day** |
| **M5 – Bench & rich display** | `eta/bench/src/hash_map_benchmark.cpp` + scripts; Jupyter MIME bundle in `eta/jupyter/src/eta/jupyter/display.cpp`; mark hashmap/hashset deliverable complete in `next-steps.md` and `release-notes.md`. | **1–2 days** |
| **Total** |  | **~9 days** |

Each milestone is independently shippable behind no feature flag (the
new builtins are pure additions). M3/M4 refactors can land in any
order after M2.

---

## 9. Risks & Open Questions

1. **Numeric key semantics.** Plan recommendation: distinct keys for
   `1` vs `1.0` (Clojure-compatible). Alternative: collapse via `=`.
   Decision needed before M1 lands public API. *Recommend confirm at
   M1 kickoff.*
2. **Future mutable variant.** If a `mutable-hash-map!` is ever added,
   it must register a trail entry per write to roll back under
   backtracking. The unified trail already supports custom restore
   closures via the existing `trail-mark` machinery (per next-steps.md
   "Runtime & GC"). Out of scope for v1 — flag as a known follow-on.
3. **Iteration / reduce protocol.** Whether to expose `hash-map-fold`
   (recommended; included above) vs only `hash-map-keys` + lookup
   (avoids one extra builtin but allocates a key list). Recommendation:
   **ship `hash-map-fold`** — it is the canonical zero-allocation form
   and matches the `csv:fold` precedent in `std/csv.eta`.
4. **HashDoS exposure.** Random per-process SipHash seed defends
   against external-input attacks (relevant once `std.json` and
   `std.http` ship). Document the seeding location.
5. **Reader-syntax backward compatibility.** Once we adopt
   `#hashmap{…}` as a printer-only form, switching to a true reader
   literal later is a non-breaking change (any existing
   `(hash-map …)` calls still work).
6. **Cross-shard allocation.** Allocations in `Heap::allocate` are
   sharded — confirm hash-map-heavy workloads do not concentrate on
   one shard. The shard is `select_shard(id)` which is
   `splitmix64(id) & (NUM_SHARDS-1)` — fine, but worth a sanity check
   in the M5 benchmark.

---

## 10. Out of Scope

The following are explicitly **not** addressed by this plan:

- **Atom** (CAS cell) — separate H2 sub-plan; needs
  `std::atomic<LispVal>`, GC barrier on `reset!`, trail entry under
  backtracking, and a watcher contract.
- **HAMT / persistent vector** — v2 upgrade, gated on M5 benchmark
  evidence.
- **Reader literal syntax** `{…}` and `#{…}` — gated on usage data
  after v1 ships.
- **Transient / mutable hash maps** (Clojure's `transient!` /
  `persistent!` family).
- **Sorted maps** (Eta has no ordering protocol yet).
- **`=`-keyed numeric coercion** — defer until a concrete user
  requests it.
- **PrimitiveSpecialisation opcodes** for `hash-map-ref` — wait for
  flow-sensitive type specialisation in the optimiser pipeline.

---

*End of plan — draft for review.*

