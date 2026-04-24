# REPL Redefinition Fix — Plan (Option A: shadow prior REPL bindings)

[← Back to README](../README.md) · [Modules & Stdlib](modules.md) ·
[Architecture](architecture.md)

---

## Problem

The REPL refuses to let users redefine an existing name:

```text
eta> (defun plus_one (x) (my_add 1 x))
... user later redefines plus_one ...
eta> (defun plus_one (x) (my_add 1 x))
error [file 0:12:11-12:20]: imported name 'plus_one' conflicts with
  local define in module '__repl_15'
```

### Root cause

Each REPL submission is wrapped in a fresh module `__repl_N`
([`eta/interpreter/src/eta/interpreter/main_repl.cpp`](../eta/interpreter/src/eta/interpreter/main_repl.cpp)
≈ lines 338–390). For each submission the REPL:

1. Auto-exports every user-defined name via the synthesised
   `(export …)` clause (lines 376–382).
2. Imports every previous REPL module wholesale via blanket
   `(import __repl_K)` clauses (lines 372–374).

When the user redefines `plus_one` in `__repl_15`, the freshly
generated module both:

- defines `plus_one` locally, and
- imports `plus_one` (via `import __repl_14`, which exported it).

The module linker treats this as an error in
[`eta/core/src/eta/reader/module_linker.cpp`](../eta/core/src/eta/reader/module_linker.cpp)
lines 268–272:

```cpp
for (const auto& [local, remote] : map) {
    if (tgt.defined.contains(local)) {
        return std::unexpected(LinkError{LinkError::Kind::ConflictingImport, pi.where,
            std::string("imported name '") + local + "' conflicts with local define in module '" + tgt.name + "'"});
    }
    ...
}
```

A related latent bug: redefining the same name across multiple
submissions (`(define x 1)` then `(define x 2)`) eventually trips
the import-vs-import check at line 281
(`conflicting imports for 'x' from '__repl_1' and '__repl_2'`) on
the *next* submission, because two prior modules both export `x`.

We want: every REPL submission to behave as though only the
**most recent** definition of any name is in scope, with all
earlier homonyms shadowed.

---

## Approach: Option A — Shadow at the REPL level

Keep the linker strict (its current diagnostic is genuinely useful
for real source files). Fix the REPL by stopping it from importing
names that are about to be — or have already been — redefined.

The REPL fabricates the `__repl_N` modules itself, so it is the
right place to enforce shadowing semantics. Concretely, swap the
blanket `(import __repl_K)` clauses for selective
`(import __repl_K (only n1 n2 …))` clauses that omit any name
shadowed by a later module (or by the submission being built).

This also fixes the latent multi-redefinition bug for free,
because each name is only imported from the single most recent
module that still owns it.

---

## Implementation Stages

### S1 — Track per-module exports alongside `prior_modules`

**File:** [`eta/interpreter/src/eta/interpreter/main_repl.cpp`](../eta/interpreter/src/eta/interpreter/main_repl.cpp)

Replace:

```cpp
std::vector<std::string> prior_modules;
```

with a structure that remembers each prior module's exported
names:

```cpp
struct PriorModule {
    std::string name;                  ///< e.g. "__repl_14"
    std::vector<std::string> exports;  ///< names this module exported
};
std::vector<PriorModule> prior_modules;
```

After a successful submission (line ~401, where we currently do
`prior_modules.push_back(module_name);`), push the new record:

```cpp
prior_modules.push_back(PriorModule{module_name, user_defines});
```

`user_defines` is already collected for the auto-export clause
(line ~345), so no extra work is needed to gather it.

### S2 — Compute the live-binding set when generating imports

Before emitting the `imports` string (currently lines 367–374),
compute which names each prior module should still contribute.

Walk `prior_modules` **newest-first** and pick names that have not
yet been shadowed. The shadow set is seeded with `user_defines`
of the *current* submission so the new local definitions never
collide with their own older versions.

```cpp
std::unordered_set<std::string> shadowed(
    user_defines.begin(), user_defines.end());

// module-name -> names still visible from that module
std::unordered_map<std::string, std::vector<std::string>> visible_from;
std::vector<std::string> emit_order; // preserve newest-first ordering

for (auto it = prior_modules.rbegin(); it != prior_modules.rend(); ++it) {
    std::vector<std::string> live;
    for (const auto& name : it->exports) {
        if (shadowed.insert(name).second) {
            live.push_back(name);
        }
    }
    if (!live.empty()) {
        visible_from.emplace(it->name, std::move(live));
        emit_order.push_back(it->name);
    }
}
```

### S3 — Emit selective `(import … (only …))` clauses

Replace the existing blanket-import loop:

```cpp
for (const auto& prev : prior_modules) {
    imports += "  (import " + prev + ")\n";
}
```

with a selective one. Iterate `emit_order` in reverse so the
oldest-still-visible module is imported first (mirrors current
ordering for any diagnostic output that may depend on it):

```cpp
for (auto it = emit_order.rbegin(); it != emit_order.rend(); ++it) {
    const auto& mod = *it;
    const auto& live = visible_from.at(mod);
    imports += "  (import " + mod + " (only";
    for (const auto& n : live) imports += " " + n;
    imports += "))\n";
}
```

If a prior module has no live exports left it is skipped entirely
— the module remains loaded in the runtime (so any closures it
created still work) but contributes nothing to the new module's
visible namespace.

> **Confirm `(only …)` import-spec syntax** matches what the
> reader / linker expect. Search for the production in
> [`eta/core/src/eta/reader/`](../eta/core/src/eta/reader/) and the
> handling in `module_linker.cpp` (`ImportSpec::Kind::Only` is
> referenced near line 232's `Rename` case — the `Only` branch
> sits adjacent). If the keyword spelled in source is different
> (e.g. `:only` or `with`), use that spelling instead.

### S4 — Tests

Add REPL behaviour tests. The REPL itself is interactive, so
tests should drive it via `Driver::run_source` with the same
wrapping the REPL does (or factor the wrapping into a small
testable helper — see S5).

**File (new):** `eta/interpreter/test/repl_redefine_tests.cpp`

Cases:

1. **Simple redefinition.** Submit `(defun f (x) x)`, then submit
   `(defun f (x) (+ x 1))`, then evaluate `(f 10)`. Expect `11`,
   no error.
2. **Redefinition after use.** Submit `(defun f (x) x)`, evaluate
   `(f 5)` → `5`, redefine `(defun f (x) (* x 2))`, evaluate
   `(f 5)` → `10`.
3. **Triple redefinition** (regression for the latent
   import-vs-import bug). `(define x 1)`, `(define x 2)`,
   `(define x 3)`, then evaluate `x` → `3`, no diagnostic.
4. **Mutual independence.** Defining `f`, then `g` that calls `f`,
   then redefining `f` — `g` should still call the *original* `f`
   (because `g`'s closure was compiled against the previous
   `__repl_K`'s `f`). Document this as the expected semantics; it
   matches typical Lisp/Scheme REPL behaviour where redefinition
   only affects new code, not already-compiled call sites that
   captured the old binding.
5. **Selective import correctness.** Submit `(define a 1) (define b 2)`,
   then `(define a 10)`, then evaluate `(+ a b)` → `12` (new `a`,
   original `b`).

If the REPL wrapping is extracted (S5), these can be ordinary
unit tests; otherwise drive end-to-end through stdin via the
existing test harness pattern.

### S5 — (Optional) Extract REPL wrapping into a testable helper

Pure refactor — moves the module-string assembly out of `main()`
so it's directly unit-testable. Suggested signature:

```cpp
// In a new header eta/interpreter/include/eta/interpreter/repl_wrap.h
namespace eta::interpreter {

struct PriorModule {
    std::string name;
    std::vector<std::string> exports;
};

struct ReplWrapResult {
    std::string source;          ///< the synthesised (module …) text
    std::string module_name;     ///< __repl_N
    std::string result_name;     ///< __repl_r_N or "" if no expr
    std::vector<std::string> user_defines;
    bool last_is_expr = false;
};

ReplWrapResult wrap_repl_submission(
    const std::vector<std::string>& forms,
    int repl_id,
    bool prelude_available,
    const std::vector<PriorModule>& prior_modules);

} // namespace eta::interpreter
```

`main_repl.cpp` then becomes a thin loop over `getline`,
`split_toplevel_forms`, `wrap_repl_submission`, and
`driver.run_source`. Recommended but not strictly required for
the fix.

### S6 — Documentation

- **New** `docs/repl.md` (or extend an existing REPL section): a
  short note that "redefinitions in the REPL shadow earlier
  definitions for new code; existing closures retain their
  original bindings". Mention that this matches the
  Scheme/Common-Lisp REPL convention.
- **Update** `docs/quickstart.md` if it currently shows a REPL
  session that would be affected.
- **Update** `docs/next-steps.md` to remove "REPL refuses
  redefinition" from any open-issue list, if listed.

---

## Sequencing & Sizing

| Stage | Description | Effort | Risk |
|---|---|---:|:---:|
| S1 | Track per-module exports | 0.25 d | Low |
| S2 | Compute live-binding set | 0.25 d | Low |
| S3 | Emit `(only …)` imports | 0.25 d | Low — pending `only` syntax confirmation |
| S4 | Tests | 0.5 d | Low |
| S5 | (Optional) extract wrapper | 0.5 d | Low |
| S6 | Docs | 0.25 d | Low |

**Total: ~1.5–2 engineer-days** (excluding optional S5).

---

## Risks & Mitigations

- **`only` import-spec spelling.** If the linker doesn't expose an
  `only` keyword in source syntax, fall back to `(import M (rename
  (n n) …))` listing only the live names — same effect, slightly
  more verbose. As a last resort, add an `only` parser branch
  alongside `Rename` / `Prefix` in the reader.
- **Closures bound to old definitions.** Already-compiled code in
  `__repl_K` still references its own `f`. This is intended Lisp
  semantics (it's what users expect from a REPL: redefining a
  helper does not retroactively rewrite existing call sites). Call
  it out in `docs/repl.md` so it's not surprising.
- **Module-load memory growth.** Modules with all exports shadowed
  remain loaded but unimported. They are still reachable via the
  runtime's module table (their closures may be live), so no
  change to lifetime — just a slow accumulation across very long
  REPL sessions. Acceptable; if it ever becomes a problem, a
  separate "GC unreferenced REPL modules" task can address it.
- **Diagnostic clarity for genuine clashes.** Because the REPL
  now mechanically avoids the clash, users will *never* see the
  linker's `ConflictingImport` diagnostic from the REPL. That's
  the desired behaviour — the diagnostic remains for real source
  files.

---

## Definition of Done

1. The motivating session works:
   ```text
   eta> (defun plus_one (x) (+ 1 x))
   eta> (defun plus_one (x) (+ 1 x))   ;; no error
   eta> (plus_one 41)
   => 42
   ```
2. All five test cases in S4 pass.
3. Triple-redefinition (`(define x 1)` × N) no longer produces
   `conflicting imports for 'x' from '__repl_…' and '__repl_…'`.
4. No change to `module_linker.cpp` behaviour for non-REPL
   modules — file-based compilation still rejects an imported
   name that collides with a local define.
5. `docs/repl.md` documents the new shadowing semantics.

---

## Source Locations Referenced

| Component | File |
|---|---|
| REPL wrapping & module synthesis | [`eta/interpreter/src/eta/interpreter/main_repl.cpp`](../eta/interpreter/src/eta/interpreter/main_repl.cpp) |
| Module linker (conflict diagnostic) | [`eta/core/src/eta/reader/module_linker.cpp`](../eta/core/src/eta/reader/module_linker.cpp) |
| Import-spec parser (for `only` syntax) | [`eta/core/src/eta/reader/`](../eta/core/src/eta/reader/) |
| Driver entry point | [`eta/interpreter/src/eta/interpreter/driver.cpp`](../eta/interpreter/src/eta/interpreter/driver.cpp) |

