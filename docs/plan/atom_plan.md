# Eta Atom Plan

[Back to README](../../README.md) ·
[Next Steps](../next-steps.md) ·
[Modules Reference](../guide/reference/modules.md)

---

## 1) Objective

Implement hosted-platform Phase H2 Atom support: a single-cell mutable
reference with CAS semantics, shipped as `std.atom` with runtime
primitives and test/docs coverage.

Target API surface:

- `(atom v)`
- `(atom? a)` (with compatibility notes below)
- `(deref a)`
- `(reset! a v)`
- `(swap! a fn args ...)`
- `(compare-and-set! a old new)`

---

## 2) Constraints and compatibility

### 2.1 Existing `atom?` conflict

`std.core` already exports `atom?` with Scheme semantics ("not a pair").
So Atom implementation must not silently break existing prelude users.

Recommended compatibility approach:

1. Keep `std.core:atom?` unchanged.
2. Introduce runtime primitives with `%atom-*` internal names.
3. Add `std.atom` wrapper module with:
   - collision-safe names: `atom:atom?`, `atom:deref`, `atom:reset!`,
     `atom:swap!`, `atom:compare-and-set!`
   - Clojure-style aliases: `atom`, `atom?`, `deref`, `reset!`, `swap!`,
     `compare-and-set!` (opt-in via explicit import only).
4. Do not auto-import `std.atom` in `std.prelude`.

This preserves old code while enabling the requested API.

### 2.2 CAS equality semantics

Use raw `LispVal` bitwise equality for CAS (`eq?`-style identity/value-bit
comparison), not deep structural `equal?`.

### 2.3 `swap!` semantics

`swap!` must be implemented as a CAS retry loop. The callback may run more
than once under contention; docs/tests should state this explicitly.

---

## 3) Runtime design

## 3.1 New heap type

Add new runtime type:

- `eta/core/src/eta/runtime/types/atom.h`

Proposed payload:

```cpp
struct Atom {
    std::atomic<LispVal> cell{nanbox::Nil};
    explicit Atom(LispVal initial) : cell(initial) {}
};
```

Notes:

- Keep payload minimal in v1: one atomic cell only.
- No watcher chain in v1.

## 3.2 Object-kind and visitor plumbing

Update:

- `eta/core/src/eta/runtime/memory/heap.h`
  - add `ObjectKind::Atom`
  - add enum-to-string mapping
- `eta/core/src/eta/runtime/types/types.h`
  - include `atom.h`
- `eta/core/src/eta/runtime/memory/heap_visit.h`
  - add `visit_atom(...)`
  - wire `ObjectKind::Atom` dispatch
- `eta/core/src/eta/runtime/memory/mark_sweep_gc.h`
  - implement `visit_atom`: mark current stored value

GC behavior:

- Atom cell is a strong reference.
- During mark, load `cell` and visit it as a root edge.

## 3.3 Factory helpers

Update:

- `eta/core/src/eta/runtime/factory.h`

Add:

- `make_atom(heap, initial)` helper.

Implementation note:

- `std::atomic` objects are not copyable; allocate Atom with direct ctor
  args (avoid aggregate copy patterns that assume movable payloads).

## 3.4 Formatter and diagnostics

Update:

- `eta/core/src/eta/runtime/value_formatter.cpp`

Add Atom formatting, e.g.:

- `#<atom>`

Optional (nice-to-have): include current value in write mode for debugging.

---

## 4) Primitive API implementation

Implement in:

- `eta/core/src/eta/runtime/core_primitives.h`
- `eta/core/src/eta/runtime/builtin_names.h`

Internal primitive names:

- `%atom-new`
- `%atom?`
- `%atom-deref`
- `%atom-reset!`
- `%atom-compare-and-set!`
- `%atom-swap!`

### 4.1 Primitive contracts

`%atom-new`:

- args: `(value)`
- returns: atom object

`%atom?`:

- args: `(x)`
- returns: `#t` iff x is Atom object

`%atom-deref`:

- args: `(a)`
- returns current cell value

`%atom-reset!`:

- args: `(a value)`
- stores `value` (seq-cst), returns `value`

`%atom-compare-and-set!`:

- args: `(a old new)`
- CAS from `old` to `new` using raw `LispVal` equality
- returns `#t` on success, `#f` otherwise

`%atom-swap!`:

- args: `(a fn . rest)`
- loop:
  1. read old
  2. compute candidate `new = (apply fn (old . rest))`
  3. CAS old -> new
  4. on success return new, else retry
- closure invocation via VM (`vm->call_value`) when available
- if VM is null, allow primitive-only fallback (same pattern as
  `hash-map-fold`)

### 4.2 Rooting and GC safety in `%atom-swap!`

Protect transient values (`old`, `new`, callable, args) with
`heap.make_external_root_frame()` across retry iterations, so retries are
GC-safe even when callback allocates.

---

## 5) `std.atom` module

Add:

- `stdlib/std/atom.eta`

Module responsibilities:

1. Provide public wrapper API over `%atom-*` primitives.
2. Export both collision-safe names and Clojure-style aliases.

Suggested exports:

- `atom:new`, `atom:atom?`, `atom:deref`, `atom:reset!`,
  `atom:swap!`, `atom:compare-and-set!`
- `atom`, `atom?`, `deref`, `reset!`, `swap!`, `compare-and-set!`

Do **not** add `std.atom` to `std.prelude` in v1.

---

## 6) Tests

## 6.1 C++ runtime tests

Add:

- `eta/qa/test/src/atom_tests.cpp`

Update:

- `eta/qa/test/CMakeLists.txt` (add `atom_tests.cpp`)

Coverage:

1. Builtin registration (`%atom-*` present, arities correct).
2. Type and arity errors.
3. `deref` / `reset!` behavior.
4. CAS success and failure paths.
5. `swap!` with:
   - primitive callback
   - closure callback (VM-enabled path)
   - extra args forwarding
6. GC reachability:
   - store heap object in atom
   - force GC
   - assert value remains live and readable.

## 6.2 Stdlib tests

Add:

- `stdlib/tests/atom.test.eta`

Coverage:

1. wrapper smoke (`atom`, `deref`, `reset!`)
2. `compare-and-set!` true/false cases
3. `swap!` update semantics
4. collision-safe alias behavior (`atom:*` symbols)

---

## 7) Docs

Add:

- `docs/guide/reference/atom.md`

Update:

- `docs/guide/reference/modules.md` (new `std.atom` section)
- `docs/guide/reference/README.md` (index row)

Post-landing updates:

- `docs/next-steps.md`:
  - move Atom from outstanding H2 gap to delivered.
- `docs/release-notes.md`:
  - add Atom release entry.

---

## 8) Rollout plan

### A1 - Runtime type and GC plumbing

- add `types::Atom`, `ObjectKind::Atom`, factory helper, visitor hooks.

Gate:

- `eta_core_test` builds and existing suites stay green.

### A2 - Core primitives

- implement `%atom-*` primitives and register names in runtime + analysis.

Gate:

- new `atom_tests.cpp` passes basic API tests.

### A3 - Stdlib module

- add `stdlib/std/atom.eta` wrappers/aliases.

Gate:

- `stdlib/tests/atom.test.eta` passes in `eta_test`.

### A4 - Docs and references

- add reference page and module index updates.

Gate:

- docs link check/manual pass for new page references.

### A5 - Next-steps and release-note updates

- mark H2 Atom slice delivered and document shipped API/tests.

Gate:

- docs diff reviewed and consistent with shipped behavior.

---

## 9) Risks and mitigations

1. Name collisions with `std.core:atom?`
   - Mitigation: keep `std.atom` opt-in, provide `atom:*` canonical names,
     avoid prelude auto-import.

2. `swap!` callback side effects under contention
   - Mitigation: document retry semantics clearly and test deterministic
     single-thread behavior only.

3. GC race/missed roots during `swap!`
   - Mitigation: external-root frame around retry-loop transient values;
     explicit GC regression test.

4. VM-null call path (`register_builtin_names` / non-VM contexts)
   - Mitigation: primitive-only fallback path and explicit type errors for
     closure callbacks without VM context.

---

## 10) Acceptance criteria

Atom is considered shipped when:

1. Runtime object kind + GC traversal for Atom is in place.
2. `%atom-*` primitives are implemented and registered in both runtime and
   `builtin_names.h`.
3. `std.atom` module exists with documented API.
4. New test coverage is green:
   - `eta_core_test` (including `atom_tests.cpp`)
   - `eta_test` (`stdlib/tests/atom.test.eta`)
5. Reference docs are published and indexed.
6. `next-steps.md` and `release-notes.md` reflect delivery.

