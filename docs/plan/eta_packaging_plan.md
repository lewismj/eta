## Eta Packaging — Comprehensive Design Plan

**Goal.** Give Eta a real package system: a manifest, a lockfile, a
deterministic resolver, a per-project module root, an
`ETA_MODULE_PATH`-driven runtime lookup that understands compiled
`.etac` artifacts, a precompiled stdlib, and an `eta` umbrella CLI
(`new`, `build`, `add`, `test`, `publish`, …). v1 ships **pure-Eta
packages only**; the design leaves explicit hooks for native (C++)
bindings — torch / eigen / nng — in a later milestone without
re-architecting the manifest, resolver, or `.etac` format.

The plan is opinionated. Where there is more than one reasonable
choice, it picks one and explains why.

---

### Table of contents

1. [Goals & non-goals](#1-goals--non-goals)
2. [Current state & gaps](#2-current-state--gaps)
3. [Terminology](#3-terminology)
4. [Package layout on disk](#4-package-layout-on-disk)
5. [Module naming & namespacing](#5-module-naming--namespacing)
6. [Resolution algorithm](#6-resolution-algorithm)
7. [Compilation & artifacts (`.etac`)](#7-compilation--artifacts-etac)
8. [Dependency management](#8-dependency-management)
9. [CLI design — the `eta` umbrella](#9-cli-design--the-eta-umbrella)
10. [Stdlib & prelude precompilation](#10-stdlib--prelude-precompilation)
11. [Tooling integration: REPL, LSP, DAP, Jupyter](#11-tooling-integration)
12. [Security model](#12-security-model)
13. [Registry (future)](#13-registry-future)
14. [Migration & compatibility](#14-migration--compatibility)
15. [Testing strategy](#15-testing-strategy)
16. [Phased roadmap (M1–M6)](#16-phased-roadmap)
17. [Open questions & risks](#17-open-questions--risks)
18. [Appendix: end-to-end example](#18-appendix-end-to-end-example)

---

## 1. Goals & non-goals

### v1 Goals

- A single, declarative **manifest** (`eta.toml`) per project / package.
- A reproducible **lockfile** (`eta.lock`).
- A **per-project** dependency root (`./.eta/modules/`), à la
  `node_modules` / `vendor/`, plus a **shared user cache**
  (`~/.eta/cache/`), à la `~/.cargo/registry`.
- Runtime resolution that prefers **`.etac` over `.eta`** when both are
  present and source-hash matches.
- An `eta` umbrella CLI that wraps existing `etac` / `etai` / `eta_repl`.
- A **precompiled stdlib** shipped in the install bundle, with hashed
  fall-back recompilation if the bundled artifacts mismatch.
- A versioned `.etac` ABI: existing `FORMAT_VERSION` and `num_builtins`
  fields already give us the substrate; we extend with a compiler-id
  fingerprint and a per-package manifest-hash.

### v1 Non-goals

- **Native extensions.** No `cargo build`-style C++ build orchestration.
  Native bindings remain compiled into the runtime as today
  (`eta_torch`, `eta_nng`, `eta_stats`, `eta_log`); packages can
  *declare* a dependency on a native capability (`requires-native =
  ["torch"]`) and refuse to load on a runtime that does not advertise
  it.
- **A live registry.** v1 supports `path`, `git` (commit-pinned),
  and `tarball` sources. A central registry (`registry.eta-lang.org`)
  is sketched in §13 but is post-v1.
- **Cross-compilation / fat artifacts.** `.etac` is platform-neutral
  today (pure VM bytecode); we keep that property explicitly (§7.5).
- **Code signing.** Checksums in v1; signatures in a future milestone.

---

## 2. Current state & gaps

| Subsystem | Today | Gap closed by this plan |
|---|---|---|
| Module syntax | `(module name (export …) (import …) (begin …))` — see [`module_linker.h`](../../eta/core/src/eta/reader/module_linker.h) | Unchanged; package metadata is *out-of-band* (manifest), not new syntax. |
| Name → path | `std.foo` → `std/foo.eta` via [`module_path.h`](../../eta/core/src/eta/interpreter/module_path.h) (`ModulePathResolver`) | Adds `.etac` lookup, project-local root, and per-package namespace prefix. |
| Search path | `ETA_MODULE_PATH` env var + bundled stdlib + build-time fallback | Formalised precedence (§6); project-root injection; `--module-path` CLI flag everywhere. |
| Bytecode | `ETAC` magic, `FORMAT_VERSION=3`, source hash, builtin-count check ([`bytecode_serializer.h`](../../eta/core/src/eta/runtime/vm/bytecode_serializer.h)) | Adds compiler fingerprint, manifest hash, dep-set hash; defines staleness rules. |
| Prelude load | `Driver::load_prelude()` — re-parses & re-compiles `prelude.eta` every cold start | Bundled `prelude.etac`, lazy recompile on hash mismatch. |
| CLI | `etac`, `etai`, `eta_repl`, `eta_lsp`, `eta_dap`, `eta_jupyter` | Adds `eta` umbrella with subcommands; existing binaries keep their CLIs. |
| Project metadata | None — projects are loose `.eta` files in a tree | `eta.toml` + `eta.lock`. |
| Per-project deps | None — `examples/xva-wwr/ml-calibration-test.eta` manually sets `$env:ETA_MODULE_PATH` | Implicit project root injection by `eta` CLI. |
| Native deps | Hard-coded into the runtime; `BuiltinCountMismatch` makes a torch-built `.etac` un-loadable on a non-torch runtime | Manifest-level `requires-native` declaration; runtime self-describes its capability set. |

---

## 3. Terminology

| Term | Definition |
|---|---|
| **Module** | A single `(module name …)` form. Unit of import. |
| **File** | A `.eta` source file or `.etac` bytecode file. May contain ≥1 modules (today the stdlib already does this — `prelude.eta` declares only `std.prelude`, and `std/causal/identify.eta` is a separate file but a sibling module of `std.causal`). |
| **Package** | A directory containing `eta.toml`. The unit of versioning, distribution, and dependency. A package owns one or more modules under a single **module-name prefix** declared in its manifest. |
| **Project** | A package that is the *current working* one (has an `eta.lock`). Distinguished from a *dependency* package only by role. |
| **Distribution** | A built artifact — a tarball or directory — produced by `eta build --release` for installation or upload. Contains `.etac` files and a frozen manifest. |
| **Workspace** | A top-level directory whose `eta.toml` declares `[workspace] members = [...]`, à la Cargo workspaces. Out of v1 scope but the manifest grammar reserves the key. |

---

## 4. Package layout on disk

### 4.1 Source-tree layout (in a developer's working copy)

```text
mathx/                       ← package root (the directory that contains eta.toml)
├── eta.toml                 ← manifest (REQUIRED)
├── eta.lock                 ← lockfile (generated; commit to VCS for apps, optional for libs)
├── README.md
├── LICENSE.txt
├── src/                     ← module sources; module-name prefix is implied by manifest [package].name
│   ├── mathx.eta            ← the root module:   (module mathx …)
│   ├── mathx/               ← submodules:        (module mathx.linalg …) → src/mathx/linalg.eta
│   │   ├── linalg.eta
│   │   └── linalg/
│   │       └── solve.eta    ← (module mathx.linalg.solve …)
│   └── _internal/
│       └── trig.eta         ← (module mathx._internal.trig …) — leading "_" = private convention
├── tests/                   ← run by `eta test`; can `(import mathx)` directly
│   └── linalg.test.eta
├── examples/                ← run by `eta run --example NAME`
│   └── solve_demo.eta
├── bench/                   ← run by `eta bench`
│   └── matmul_bench.eta
├── docs/                    ← rendered by `eta doc` (post-v1)
└── .eta/                    ← per-project cache (gitignored)
    ├── modules/             ← installed deps, ready for the resolver
    │   └── stats-1.4.2/
    │       ├── eta.toml
    │       ├── eta.lock
    │       └── target/release/   ← pre-compiled .etac files (mirrors src/ tree)
    ├── target/              ← THIS package's build output
    │   ├── debug/   …
    │   └── release/ …
    └── registry-cache/      ← raw downloaded tarballs
```

**Rationale for `src/`.** Mirrors Rust/most modern build systems and
avoids the "prelude.eta sits at the package root" awkwardness the
current stdlib has. The `src/<name>.eta` ↔ module name mapping is
mechanical: a manifest with `name = "mathx"` *requires* `src/mathx.eta`
to exist and to declare `(module mathx …)`; it *may* additionally
declare `(module mathx.foo …)` etc. This convention is checked by
`eta build`.

**Rationale for `.eta/` cache.** A single, predictable place for *all*
generated state. Maps onto the existing `out/` convention used by the
Eta repo today. Adding it to `.gitignore` is a one-liner.

### 4.2 Installed (binary) package layout

`eta install` and `eta vendor` produce this:

```text
.eta/modules/<name>-<version>/
├── eta.toml              ← FROZEN manifest (no path-deps, all version ranges resolved)
├── eta.lock              ← copy of the publishing project's lock for reproducibility
├── CHECKSUMS             ← blake3(file) per shipped file; signed (post-v1)
├── src/                  ← OPTIONAL — present iff published with `--with-source`
└── target/release/
    ├── <name>.etac
    └── <name>/
        └── …             ← mirrors module tree
```

`src/` shipping is opt-in:

- **Apps / executables:** ship without sources by default.
- **Libraries:** ship sources by default — needed for source maps in
  DAP stack traces and for the LSP's "go-to-definition".

### 4.3 Manifest: `eta.toml`

We pick **TOML** over an Eta-native `package.eta` for three reasons:

1. **Tooling reach** — every editor and CI system parses TOML.
   An Eta-evaluated manifest would be circular: you would need a
   working interpreter to discover what packages to compile.
2. **Hermeticity** — manifests must be statically analyzable
   (resolver, registry, lockfile diff). An evaluable DSL invites
   manifest-time side effects.
3. **Precedent** — Cargo, Poetry, Hatch, Hex (`mix.exs` is the only
   counter-example among popular ecosystems and it has well-documented
   pain).

#### Schema (full grammar)

```toml
# === Identity ===
[package]
name        = "mathx"           # REQUIRED. Lowercase, [a-z][a-z0-9_-]{0,63}.
                                # Must equal the root module name (no dots).
                                # Hyphens in name → underscores in module path:
                                #   name "my-pkg" ⇒ (module my_pkg …)
version     = "1.4.2"           # REQUIRED. Semver 2.0 (MAJOR.MINOR.PATCH[-pre][+build]).
authors     = ["A <a@x>"]
license     = "MIT OR Apache-2.0"   # SPDX expression
description = "Matrix and linear-algebra helpers."
homepage    = "https://…"
repository  = "https://github.com/…"
readme      = "README.md"
keywords    = ["math", "linalg"]
categories  = ["science::mathematics"]

# === Compiler/runtime compatibility ===
[compatibility]
eta            = ">=0.6, <0.8"   # accepted compiler/runtime versions (semver range)
etac-format    = 3                # minimum bytecode format version this pkg may run on
requires-native = []              # [] | ["torch"] | ["nng"] | ["torch", "eigen"]
                                  # advertised by runtime; resolver refuses if absent.

# === Module surface ===
[modules]
# Optional override. By default, every (module mathx[…] …) under src/ is an export.
# Use this to restrict the public surface or to re-export.
public  = ["mathx", "mathx.linalg"]
private = ["mathx._internal.*"]   # glob patterns; consumed by docs + lints

# === Dependencies ===
[dependencies]
# Three source kinds. Each `name = …` entry is one dep.
stats     = "^1.2"                                  # registry (post-v1) or local mirror
nng-utils = { version = "0.4.1", registry = "default" }
fastmat   = { path = "../fastmat" }                 # workspace / monorepo
plotty    = { git = "https://…/plotty.git", rev = "9c1a…b3" }
csvkit    = { tarball = "https://…/csvkit-0.3.tar.gz", sha256 = "…" }

# Feature flags (enable only optional deps when chosen):
[dependencies.torch-extras]
version  = "0.2"
optional = true

[features]
default = []
gpu     = ["torch-extras"]
mkl     = []                                        # pure feature flag, no dep

[dev-dependencies]   # built only for `eta test` / `eta bench`
test-helpers = { path = "../test-helpers" }

# === Entry points ===
[[bin]]                          # zero or more binaries
name = "matsolve"
main = "mathx.bin.matsolve"      # module that defines a `(define (main args) …)`

[lib]
# Implicit; this section can be omitted. Present to let a package opt out:
# enabled = false   ← then [[bin]] entries are mandatory.

# === Build settings ===
[build]
optimize  = "O1"     # "O0" | "O1" | "O2"   (corresponds to existing etac -O knob)
strip     = false    # strip debug spans (mirrors etac --no-debug)
emit-stats = false

[build.release]      # override section, applied for `eta build --release`
optimize  = "O2"
strip     = true

# === Scripts (sandboxed; see §12) ===
[scripts]
fmt   = "etac --fmt src/"
gen   = "python ./tools/gen_tables.py"   # external scripts allowed; explicit opt-in flag at install time

# === Workspaces (reserved for v2) ===
# [workspace]
# members = ["packages/*"]
```

#### Required fields & validation

| Field | Required | Validation |
|---|---|---|
| `package.name` | yes | regex above; must equal directory's `src/<name>.eta` root module |
| `package.version` | yes | semver 2.0 parse OK |
| `package.license` | yes | SPDX expression parse OK; warning if missing |
| `compatibility.eta` | yes | semver range parse OK; resolver checks against running `etac --version` |
| `dependencies.<n>` | no | each spec must specify exactly one of `version`/`path`/`git`/`tarball` |

### 4.4 Lockfile: `eta.lock`

Generated by the resolver; **commit for apps, optional for libraries**.
Format is also TOML, but treated as a machine artifact (do not
hand-edit; `eta` rewrites it on every modification).

```toml
# Auto-generated by eta — DO NOT EDIT.
version = 1                                # lockfile schema version

[[package]]
name     = "mathx"
version  = "1.4.2"
source   = "root"

[[package]]
name     = "stats"
version  = "1.2.7"
source   = "registry+default"
checksum = "blake3:9c1a…b3"
dependencies = ["nng-utils@0.4.1"]

[[package]]
name     = "nng-utils"
version  = "0.4.1"
source   = "git+https://…/nng-utils.git#9c1a…b3"
checksum = "blake3:7e88…aa"
dependencies = []
```

**Why TOML and not JSON?** Consistency with `eta.toml`; lockfile diffs
are read by humans during code review.

**Why `[[package]]` and not nested dep trees?** Flat tables make merge
conflicts trivial. The resolver is the source of truth for the graph.

---

## 5. Module naming & namespacing

### 5.1 Mapping rules

| Manifest `package.name` | Module root | File on disk |
|---|---|---|
| `mathx` | `mathx` | `src/mathx.eta` |
| `mathx` | `mathx.linalg` | `src/mathx/linalg.eta` |
| `my-pkg` | `my_pkg` | `src/my_pkg.eta` (hyphen → underscore in module space; the directory still uses the package name `my-pkg/`) |

Eta's reader already accepts `.`-separated dotted symbols; we add a
**linter rule** in `eta build`: every `(module X …)` declared by
package `P` must satisfy `X = P` or `X = P.<...>`. Cross-package
modules with the same prefix are a hard error caught at install time.

### 5.2 Re-exports

Today's `(import (rename …))` and `(import (only …))` already give us
re-export semantics inside a module body. We adopt the convention that
**a package's root module re-exports** its public surface so consumers
can write `(import mathx)` and get the curated set, à la `prelude.eta`.

### 5.3 Collision handling

The linker already raises `ConflictingImport`. Our additional
package-level rule: two installed packages **must not** declare modules
under the same prefix. This is a static check on the resolved
dependency set and runs in `eta build` before any compilation begins.

### 5.4 Prelude injection

`std.prelude` continues to be auto-loaded (existing
`Driver::load_prelude()` behavior). We make it **opt-out** via:

```toml
[build]
implicit-prelude = false   # default: true
```

Test files in the workspace can already opt out by not importing
`std.prelude`, but for benchmarking and minimal-footprint binaries the
manifest knob matters.

---

## 6. Resolution algorithm

This is the v1 contract that the runtime and the CLI both honour.

### 6.1 Search-root precedence

When resolving an `(import M)`, the resolver tries roots in this
order; **the first hit wins** (no shadow warnings — deterministic):

| # | Root | Origin |
|---|---|---|
| 1 | `<project>/src/` | Implicit when CWD or `--manifest-path` resolves to a package |
| 2 | `<project>/.eta/modules/<name>-<ver>/{target/release,src}/` | One entry per resolved dep, in lockfile order |
| 3 | Each colon/semicolon entry of `$ETA_MODULE_PATH` | User override; explicit |
| 4 | `~/.eta/cache/<name>-<ver>/{target/release,src}/` | Shared user cache |
| 5 | Compile-time `ETA_STDLIB_DIR` | Dev binaries only |
| 6 | `<exe>/../stdlib/` (bundled) | Production binaries |

(The last two already exist in
[`module_path.h`](../../eta/core/src/eta/interpreter/module_path.h);
we only insert layers 1, 2, and 4 in front.)

The resolver **always** walks the full ordered list to verify no
*later* root contains a *different* file with the same module name; on
mismatch it emits a warning (configurable to `error` via
`build.shadow-policy = "error"`).

### 6.2 Source-vs-bytecode preference per root

For each root, candidate paths in order:

1. `<root>/<rel>.etac`   ← prefer pre-compiled
2. `<root>/<rel>.eta`    ← fall back to source

If a `.etac` is found, validate (cheaply) its header against the
runtime's `(format_version, num_builtins, compiler_fingerprint,
package_manifest_hash)` and the source-hash of the sibling `.eta` if
present. On any mismatch:

- If a sibling `.eta` exists → recompile into the user cache.
- If no sibling `.eta` exists → hard error (`StaleArtifact`); user must
  reinstall the dependency.

### 6.3 Worked trace

Given:

```
project: /work/myapp     (manifest: name="myapp", deps: stats="^1")
ETA_MODULE_PATH=/extra
~/.eta/cache contains stats-1.2.7
```

`(import stats)` resolves as:

```
1. /work/myapp/src/stats.etac          → not found
2. /work/myapp/src/stats.eta           → not found
3. /work/myapp/.eta/modules/stats-1.2.7/target/release/stats.etac  ← HIT (hash OK) ✓
   (linker also checks 4–6 to confirm no shadow.)
```

### 6.4 Determinism guarantees

- Root order is **fully** determined by `(CWD, ETA_MODULE_PATH, lockfile, exe-path)`.
- Within a root, the file system is not consulted twice for the same
  name within a process; resolutions are memoised in
  `Driver`'s session.
- The resolver is the sole producer of the linker's input form list;
  it constructs that list in topological dependency order so
  `index_modules()` sees deps before consumers (matching today's
  stdlib invariant).

### 6.5 Project-local vs user-global

We recommend the **per-project `.eta/modules/`** model, à la
`node_modules`, with a content-addressed user cache for de-duplication:

| Aspect | Per-project `.eta/modules/` | User cache `~/.eta/cache/` |
|---|---|---|
| Reproducibility | ✅ project owns deps | ⚠ subject to user state |
| Disk efficiency | ❌ duplicated | ✅ deduped |
| CI cold-start | slow first time | mountable for warm starts |

We get both: `.eta/modules/<name>-<ver>/` is a **junction / symlink**
into `~/.eta/cache/<name>-<ver>/` on platforms that support it; on
Windows without privileges we fall back to a directory copy. The
user cache uses content-addressed names
`<name>-<ver>-<blake3-of-source-tarball>` so two projects depending on
the same `(name, ver, source)` share bytes.

---

## 7. Compilation & artifacts (`.etac`)

### 7.1 Existing format (recap)

`bytecode_serializer.h` already gives us magic `ETAC`, version 3,
`source_hash`, an `imports[]` list, modules table, function registry,
and the `BuiltinCountMismatch` check. We **extend** it without
breaking version 3 by appending fields in a new section guarded by a
new format flag.

### 7.2 Extended `.etac` v4 layout

```
[ Header           ] magic "ETAC", u16 format_version=4, u16 flags
                     flags: HAS_DEBUG | HAS_PACKAGE_META | HAS_DEPHASH
[ Source hash      ] u64 (blake3-truncated of original .eta source)
[ Builtin count    ] u32 (existing)
[ Compiler ID      ] NEW: 16 bytes — blake3(compiler-version || feature-flags)
[ Package meta     ] NEW (if HAS_PACKAGE_META):
                       string name
                       string version
                       u64    manifest_hash
[ Dep-hash table   ] NEW (if HAS_DEPHASH): repeated { string dep, u64 etac_hash }
[ Imports          ] vector<string>  (existing)
[ Modules          ] vector<ModuleEntry> (existing)
[ Function registry] (existing) — bytecode + constants + per-fn metadata
[ Debug spans      ] (existing, optional)
```

A v3 reader rejects v4 with `VersionMismatch`; a v4 reader accepts v3
in "untrusted" mode (must recompile if `compiler-id` would have been
required).

### 7.3 Recompile-on-stale rules

A `.etac` is **fresh** iff *all* hold:

1. `magic == "ETAC"` and `format_version` is supported.
2. `compiler_id` matches the running `etac` build.
3. `num_builtins` equals the runtime's advertised count
   (already enforced).
4. If a sibling `.eta` exists: `source_hash == hash(.eta)`.
5. For every dep in dep-hash table: the resolved dep's `.etac`
   has matching `etac_hash`.
6. `manifest_hash` matches the package's current manifest
   (catches `[build] optimize` flips and feature changes).

Otherwise, `eta build` recompiles into `.eta/target/<profile>/`. We
*never* mutate files inside `.eta/modules/<name>-<ver>/` — installed
deps are immutable; staleness against a dep means `eta install --force`.

### 7.4 Build pipeline

```
.eta source ─► lex ─► parse ─► expand ─► link ─► analyze ─► emit ─► serialize ─► .etac
                                                                       │
                                                                       ├─► CHECKSUMS entry (blake3)
                                                                       └─► dep-hash table populated
```

Identical to today's `etac` invocation; the new step is the
**dep-hash table population**, which the build orchestrator (the
`eta` CLI) fills after compiling deps in topological order.

### 7.5 Cross-platform & determinism

The bytecode serializer (`FORMAT_VERSION=3`) is already documented as
**LE-canonical**. We keep `.etac` fully platform-neutral:

- All multi-byte fields little-endian, regardless of host.
- No file paths, no timestamps, no env-vars stored in the artifact.
- Function-table ordering is source-order (already true).
- Hashes use blake3 (deterministic, no salt).

Result: the same source + same compiler + same flags ⇒ **byte-for-byte
identical** `.etac` on Windows / Linux / macOS. This is required to
let CI build once and serve to many platforms (post-v1 registry).

### 7.6 Source-vs-bytecode trade-offs

| Distribute as | Pro | Con |
|---|---|---|
| Source only | Simple, debuggable, format-version-immune | Slow cold-start; user needs full toolchain |
| `.etac` only | Fast load, no parse cost, no front-end deps | Loses source maps unless `--debug`; format-bound |
| Source + `.etac` (recommended) | Fast in production; falls back transparently | Larger artifact; needs hash checks (we have them) |

**Recommendation.** Libraries default to **source + `.etac` with debug
spans**. Apps default to **`.etac` only**, optionally `--with-source`.

---

## 8. Dependency management

### 8.1 Version ranges

Caret ranges (`^1.4`) and tilde (`~1.4.2`) à la npm/Cargo, plus exact
(`= 1.4.2`), comparators (`>=1, <2`), wildcards (`1.*`), and the
boolean operators in TOML strings. Pre-releases follow semver (do not
match unless explicitly requested).

### 8.2 Resolver algorithm

We implement **PubGrub** (the algorithm Dart's pub and Python's poetry
use). Properties that matter for Eta:

- Linear-time on conflict-free graphs (the common case).
- High-quality conflict explanations — critical because Eta has fewer
  packages today and users will hit edge cases.
- No SAT solver dependency.

PubGrub-lite v1 supports: caret/tilde/exact, single-registry,
plus the local sources `path` / `git#rev` / `tarball+sha256`. Optional
deps are toggled by feature flag selection (computed *before* the
solve).

### 8.3 Source kinds

| Kind | Pinning | Notes |
|---|---|---|
| `path = "../foo"` | n/a | Bypasses lockfile checksums; dev only; refused by `eta publish`. |
| `git = …, rev = "<sha>"` | full SHA required | Branches & tags rejected — non-reproducible. |
| `tarball = "https://…", sha256 = "…"` | sha256 mandatory | Offline-mirrorable. |
| `version = "^1"` (registry, post-v1) | semver | Resolver picks max-satisfying. |

### 8.4 Lockfile interaction

- `eta build` honours `eta.lock` if present and consistent with
  `eta.toml`; on inconsistency, prints the offending dep + range and
  refuses (use `eta update <pkg>` to advance).
- `eta add` mutates both files atomically (write-temp → rename).
- `eta update` re-solves only the requested deps, leaving the rest
  pinned (PubGrub supports this directly).
- `eta vendor` materialises the lockfile contents into
  `.eta/modules/`, suitable for offline / air-gapped builds.

### 8.5 Dev vs optional vs feature

- `[dev-dependencies]` only built for `eta test` / `eta bench`; not in
  the dep graph of `eta build --release`.
- `optional = true` deps not built unless a feature lists them.
- Features are additive monotone; a dep tree with `[gpu]` enabled in
  one consumer and not another is solved with the union (so package
  caches are reusable across feature configurations).

---

## 9. CLI design — the `eta` umbrella

A single `eta` binary that dispatches to subcommands. It **wraps** the
existing tools (`etac`, `etai`, `eta_repl`) — does not replace them —
so users with muscle memory and CI scripts keep working.

### 9.1 Subcommand reference

| Subcommand | Inputs | Outputs / Side-effects |
|---|---|---|
| `eta new <name> [--bin\|--lib]` | dir name | Creates `<name>/` with `eta.toml`, `src/<name>.eta`, `tests/`, `.gitignore`, `README.md`. |
| `eta init [--bin\|--lib]` | (cwd) | As above but in current dir. |
| `eta build [--release] [--bin NAME]` | (manifest) | Writes `.eta/target/<profile>/*.etac`; updates `eta.lock` if needed. |
| `eta run [--bin NAME] [--example NAME] [-- args…]` | (manifest) | Runs `etai` on the chosen entry under the project's resolved module path. |
| `eta test [filter]` | (manifest, `tests/`) | Compiles & runs every `tests/**/*.test.eta`; emits TAP/JUnit; exit code = #failures. |
| `eta bench [filter]` | (manifest, `bench/`) | Same as test but with `bench` profile and timing harness. |
| `eta repl` | (manifest, optional) | Launches `eta_repl` with the project's resolved module path pre-loaded. |
| `eta fmt [--check]` | sources | Wraps `etac --fmt`. Non-zero exit on diff under `--check`. |
| `eta add <pkg>[@<range>] [--dev] [--features=…]` | manifest, network | Inserts dep into `eta.toml`, runs resolver, updates `eta.lock`, downloads. |
| `eta remove <pkg>` | manifest | Reverse of `add`. |
| `eta update [<pkg>…]` | manifest, network | Re-solves; rewrites `eta.lock`. |
| `eta install [<pkg>] [--global]` | (registry) | `--global`: installs binary into `~/.eta/bin/`. Without `<pkg>`: installs current project's `[[bin]]`s. |
| `eta publish [--dry-run]` | manifest | Builds release artifact, signs (post-v1), pushes to registry. Refuses `path = …` deps. |
| `eta vendor [--target DIR]` | manifest, lockfile | Materialises all deps locally; suitable for air-gapped CI. |
| `eta tree [--depth N]` | lockfile | Prints the resolved dependency tree. |
| `eta clean [--all]` | (manifest) | Removes `.eta/target/`; `--all` also removes `.eta/modules/`. |
| `eta doc [--open]` | sources | Renders module-level docstrings to `docs/api/`. (post-v1) |

### 9.2 Compatibility

- `eta build` shells out to `etac` per module file; this isolates
  bugs and lets users diagnose with the underlying tool.
- `eta run hello.eta` (no manifest) degrades to `etai hello.eta` —
  scripts keep working.
- Existing tests using bare `etai`/`etac` keep working unchanged; the
  new resolver activates only when an `eta.toml` is found by walking
  upward from CWD or `--manifest-path` is given.

### 9.3 Implementation note

The `eta` umbrella is itself a small C++ binary (target `eta` in
`eta/cli/`) that re-uses `eta_core` for parsing the manifest TOML and
calling into a new `eta::package::Resolver`. The umbrella exec's the
existing tool binaries — no behavior is reimplemented.

---

## 10. Stdlib & prelude precompilation

This is the highest-impact single decision in the plan, and the one
the prompt asked us to be opinionated about.

### 10.1 Options

| Option | Mechanism | Pros | Cons |
|---|---|---|---|
| **(a)** Ship precompiled `.etac` next to binary | `cmake --install` packages `stdlib/*.etac` produced at build time | Fast cold start; deterministic; debuggable (sources also shipped) | Release pipeline must run a bootstrap-compiled `etac` over `stdlib/` for every platform |
| **(b)** Compile on first run into user cache | `Driver::load_prelude()` falls back to `~/.eta/cache/stdlib-<ver>/` if no bundled `.etac` | Zero release-pipeline cost | First run pays full compile cost (several seconds for the full prelude); permission issues on locked-down systems |
| **(c)** Embed prelude bytecode into the interpreter binary | `stdlib/prelude.etac` linked as a resource (`xxd -i` / `embed`) | Zero-IO cold start of prelude | Couples runtime binary size to stdlib; harder to ship a stdlib hot-fix |

### 10.2 Recommendation: hybrid

> **Embed the prelude (option c), ship the rest as bundled `.etac`
> next to the binary (option a), and lazily recompile into the user
> cache (option b) on hash mismatch.**

Why each layer:

- **Embedded prelude.** `prelude.eta` is loaded by *every* invocation
  of `etai`, `eta_repl`, `eta_lsp`, `eta_dap`, `eta_jupyter`, every
  test in `eta_test`, and every JIT-launched `spawn` actor. Cutting
  this from "parse + expand + link + analyze + emit" to "memcpy + load"
  is the largest cold-start win and removes the `Driver::load_prelude()`
  fallibility entirely.
- **Bundled stdlib `.etac`.** The non-prelude stdlib (`std.torch`,
  `std.causal.identify`, `std.stats`, …) is large and conditionally
  imported; embedding it would bloat the binary. Shipping `.etac` next
  to the binary preserves fast load while keeping the binary small.
- **Lazy recompile fallback.** Covers the cases where: developer is
  iterating on `stdlib/`, downstream packagers patch the stdlib, or a
  user's shipped `.etac` files were corrupted. The runtime
  transparently rebuilds into `~/.eta/cache/stdlib-<compiler-id>/`.

### 10.3 Build-system integration

Add to `eta/CMakeLists.txt` (sketch):

```cmake
# After etac is built, run it over stdlib/*.eta to produce stdlib/*.etac
# inside the build tree.
add_custom_target(stdlib_etac ALL
    COMMAND ${CMAKE_COMMAND} -E env
            ETA_MODULE_PATH=${CMAKE_SOURCE_DIR}/stdlib
        $<TARGET_FILE:etac> --bootstrap
            --src-root  ${CMAKE_SOURCE_DIR}/stdlib
            --out-root  ${CMAKE_BINARY_DIR}/stdlib
            -O2 --no-debug
    DEPENDS etac)

# Embed prelude.etac into eta_core as a resource.
add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/embed/prelude_blob.cpp
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_blob.py
            --in   ${CMAKE_BINARY_DIR}/stdlib/prelude.etac
            --out  ${CMAKE_BINARY_DIR}/embed/prelude_blob.cpp
            --sym  eta_embedded_prelude
    DEPENDS stdlib_etac)

# Install the rest alongside binaries.
install(DIRECTORY ${CMAKE_BINARY_DIR}/stdlib/
        DESTINATION stdlib
        FILES_MATCHING PATTERN "*.etac")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/stdlib/
        DESTINATION stdlib
        FILES_MATCHING PATTERN "*.eta")
```

The existing `install(DIRECTORY stdlib/ … PATTERN "*.eta")` in the
top-level `CMakeLists.txt` is preserved; we add a parallel `*.etac`
install line.

### 10.4 Bootstrapping the bytecode format

When `FORMAT_VERSION` bumps (e.g. `3 → 4`), the precompiled prelude
the *previous* `etac` produced cannot be loaded by the *new* runtime.
Mitigations:

1. **Stage-0 boot.** The new runtime always carries an *embedded
   v4-format prelude* produced by the same compile; `make` will fail
   loudly if `etac` and the embedded blob disagree on version. This
   is what the CMake target above guarantees.
2. **Cross-version fallback.** If a bundled stdlib `.etac` fails the
   `format_version` check, the runtime emits a one-line warning and
   recompiles from sibling `.eta`. The user never sees a crash.
3. **CI invariant.** `eta_core_test` includes a smoke test that
   loads every shipped `.etac` and asserts no recompile fallback
   triggered.

---

## 11. Tooling integration

### 11.1 REPL (`eta_repl`)

- When launched inside a project (CWD has `eta.toml`), the REPL
  resolves the dep graph and pre-loads the project's modules.
- `:reload <module>` honours the resolver and the `.etac` cache.

### 11.2 LSP (`eta_lsp`)

- The existing `LspServer::preload_prelude()` becomes
  `LspServer::preload_workspace()`, which:
  1. Walks upward from each open document to find an `eta.toml`.
  2. Invokes `Resolver` to populate the search path.
  3. Indexes every reachable module for go-to-definition / hover.
- `textDocument/publishDiagnostics` extends to manifest errors
  (parse, unknown dep, version conflict).
- A new method `eta/lockfile/explain` returns the resolver's
  derivation for a given module — drives an "Explain Resolution" code
  lens.

### 11.3 DAP (`eta_dap`)

- Source maps survive across `.etac` because we keep debug spans (the
  existing `FLAG_HAS_DEBUG`).
- The DAP server resolves stack-frame paths via the resolver, so a
  step into a dep's source jumps to
  `.eta/modules/<name>-<ver>/src/…`. Requires source shipping
  (default for libraries).

### 11.4 Jupyter (`eta_jupyter`)

- Kernel start-up calls the resolver if launched inside a project
  directory; `%load_ext` notebook magics not in v1 scope.
- The kernel embeds the prelude blob, identical to other frontends.

---

## 12. Security model

v1 is conservative; this section is mostly a fence around what we
**do not** support yet.

| Concern | v1 mitigation | Future |
|---|---|---|
| Tarball / git tampering | mandatory `sha256` for tarball, full SHA pinning for git, blake3 `CHECKSUMS` per file in installed packages | Sigstore / minisign signatures (post-v1) |
| Malicious `eta.toml` | TOML is data-only; no code runs at parse time | n/a |
| `[scripts]` execution | `eta` *never* auto-runs scripts during install. They only run when the user types `eta script <name>`. | Sandbox via OS process isolation |
| Build-time code (macros) | macros run in the compiler — same trust as today's `etai source.eta` | Document; consider macro-purity lint |
| Dep capability creep | `compatibility.requires-native = …` is auditable; `eta tree --capabilities` lists every transitive native requirement | Policy file in registry |

---

## 13. Registry (future, post-v1)

Sketch only — not in the v1 milestone set.

- HTTP/JSON read API: `GET /v1/<name>` → JSON with versions, manifests,
  tarball URLs, sha256s.
- Storage: tarballs in object storage, metadata in a small RDBMS.
- Mirroring: every registry response is content-addressed by sha256 of
  the tarball; mirrors can be plain static file servers.
- Offline: the user cache is already a complete registry mirror; an
  air-gapped CI workflow is `eta vendor` on a connected machine →
  `rsync` → `eta build --offline` on the gapped one.
- Naming: reserved prefixes (`std.*`, `eta-*`) controlled by core team.

---

## 14. Migration & compatibility

Concrete steps to shift the existing repo onto the new system without
breaking it.

1. **Wrap the stdlib in a manifest.** Add `stdlib/eta.toml` with
   `name = "std"`, version mirroring the runtime. The `src/` layout
   already exists at `stdlib/std/`; we add a thin `stdlib/src/` that
   symlinks (or, on Windows, mirrors) to `stdlib/std/` *or* simply
   move the tree (preferred long-term, but a follow-up PR).
2. **Examples.** Each `examples/<topic>/` gets an `eta.toml` with a
   single `path` dep on the in-tree stdlib. The current
   `$env:ETA_MODULE_PATH=…` boilerplate at the top of files like
   `examples/xva-wwr/ml-calibration-test.eta` disappears.
3. **Tests.** `stdlib/tests/*.test.eta` are reorganised under the new
   per-package `tests/` convention (see §4.1). The
   `_putenv_s("ETA_MODULE_PATH", …)` invocation in
   `eta/test/src/module_path_tests.cpp` is replaced by the resolver's
   programmatic API.
4. **CI.** Add a `eta build --release` step that exercises the
   precompiled-stdlib fast path; keep the existing `etai`-from-source
   path for one release as a regression net.
5. **Docs.** Cross-link `docs/guide/reference/modules.md` and the new
   `docs/guide/packages.md`; update `TLDR.md` and `README.md`'s
   installer narratives.

The cut-over is staged: the old `etai` / `etac` / `ETA_MODULE_PATH`
contract keeps working through M3, and only deprecates after M5.

---

## 15. Testing strategy

The package system itself needs a self-contained test pack. Mirror the
TAP/JUnit style already used by `std.test`.

| Layer | Test target | Examples |
|---|---|---|
| Manifest parse | `eta_core_test` | malformed TOML, missing required fields, invalid semver, name regex violations |
| Resolver | new `eta_pkg_test` | conflict explanation, optional+feature interaction, lockfile up-to-date check, deterministic ordering |
| Resolution algorithm | `eta_pkg_test` | shadowing, source-vs-bytecode preference, stale-`.etac` recompile, project-root injection |
| `.etac` v4 round-trip | extends `bytecode_serializer_tests.cpp` | new fields encoded + decoded; backward-compat read of v3 |
| End-to-end CLI | new `eta_cli_test` (shell-style) | `eta new`, `eta add`, `eta build`, `eta test`, `eta vendor`, `eta tree` |
| Stdlib precompile | `eta_core_test` | every shipped `.etac` loads with no recompile fallback |
| Cross-platform | CI matrix | Windows / Linux / macOS runs `eta build` on a fixed example and asserts byte-identical `.etac` |

Acceptance bar: ≥ 50 new tests, ≥ 90% line coverage on the new
`eta::package::*` namespace, every example in `examples/` migrates and
keeps running.

---

## 16. Phased roadmap

Each milestone is independently shippable and adds verifiable user
value.

### M1 — Manifest + resolver (P0, ~2 weeks)

- TOML parser integration (vendor `toml++` or equivalent).
- `eta::package::Manifest` + validators.
- PubGrub-lite resolver, `path` and registry-stub source kinds only.
- `eta.lock` writer/reader.
- `eta new`, `eta init`, `eta tree`.

**Deliverable:** `eta tree` on a hand-written manifest prints a
deterministic resolved graph.

### M2 — `.etac` v4 + recompile-on-stale (P0, ~1 week)

- Bump `FORMAT_VERSION` to 4; add compiler-id, package-meta,
  dep-hash sections (§7.2).
- Implement freshness check (§7.3) inside `BytecodeSerializer` and
  the resolver.
- Backward-compatible v3 read.

**Deliverable:** existing tests pass; a stale `.etac` is silently
recompiled; `bytecode_serializer_tests.cpp` extended.

### M3 — `ETA_MODULE_PATH` semantics + project-local root (P0, ~1 week)

- Extend `ModulePathResolver` to know about `.etac` lookup, project
  root, and user cache (§6.1, 6.2).
- Wire the resolver's output into the `Driver`'s search path.

**Deliverable:** an `eta build` of a two-package workspace
(`stats` lib + `myapp` bin) compiles and runs without any
`ETA_MODULE_PATH` env var set.

### M4 — Stdlib precompile + embedded prelude (P0, ~1 week)

- CMake target for stage-0 stdlib build (§10.3).
- `prelude_blob.cpp` embedding; `Driver::load_prelude()` chooses
  embedded blob → bundled `.etac` → recompile in that order.
- Install bundle includes `stdlib/*.etac`.

**Deliverable:** cold `etai hello.eta` startup (clean cache) drops
by ≥ 50% on a stopwatch test.

### M5 — Dependency install (`eta add`/`build`/`vendor`) (P1, ~2 weeks)

- `git` and `tarball` source kinds.
- `eta add`, `eta remove`, `eta update`, `eta install`, `eta vendor`.
- `eta test` + `eta run` honouring deps.
- Examples + stdlib migrated to manifests (§14).

**Deliverable:** a fresh checkout can `git clone … && eta build` and
it works without any extra environment setup.

### M6 — Registry + signing (P2, post-v1, ~4 weeks)

- HTTP read API + `eta publish`.
- Sigstore-based signatures, registry mirroring, `--offline` mode.

---

## 17. Open questions & risks

1. **Manifest format binding** — TOML chosen. *Recommendation:* commit
   to it; do not entertain a separate `package.eta` form to avoid the
   "two manifests" failure mode.
2. **Hyphen-vs-underscore in module names.** Current proposal: `name`
   may have hyphens; module path uses underscores. *Risk:* surprises
   users who expect `my-pkg` to be a valid identifier. *Mitigation:*
   the linter spells out the mapping in error messages.
3. **Workspaces.** Reserved in the manifest grammar but not v1.
   *Recommendation:* design now (table shape), implement when needed.
4. **Native-bindings story.** v1 only *advertises* required native
   capabilities. *Open:* do we eventually want a binary "native
   sidecar" channel that ships per-(OS, arch) shared libraries
   alongside `.etac`? *Recommendation:* yes, but in a "v3" effort
   after the registry stabilises.
5. **Re-exporting from a package root** — convention or compiler-
   enforced? *Recommendation:* convention in v1, lint warning in M5.
6. **Lockfile commit policy for libraries.** Cargo's "libs ignore the
   lockfile" rule catches some users out. *Recommendation:* document
   loudly that *only the consuming app's lockfile decides*; the
   library's own lockfile is for `eta test` reproducibility only.
7. **Capability advertisement granularity.** `requires-native =
   ["torch"]` is coarse; do we need version pinning of the native
   library? *Recommendation:* defer to first user pain point.

---

## 18. Appendix — end-to-end example

Author a `mathx` package, depend on it from a `myapp` project, build,
run.

### 18.1 Create the library

```console
$ eta new mathx --lib
created  mathx/
         ├── eta.toml
         ├── src/mathx.eta
         ├── tests/smoke.test.eta
         └── .gitignore

$ cat mathx/eta.toml
[package]
name        = "mathx"
version     = "0.1.0"
license     = "MIT"
description = "Linear-algebra helpers."

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]

$ cat mathx/src/mathx.eta
(module mathx
  (export square cube)
  (import std.math)
  (begin
    (defun square (x) (* x x))
    (defun cube   (x) (* x x x))))
```

```console
$ cd mathx && eta test
running 1 test
test smoke ... ok

test result: 1 passed; 0 failed
```

### 18.2 Create the app

```console
$ eta new myapp --bin
$ cd myapp
$ eta add mathx --path ../mathx
   Updating eta.lock
   Adding   mathx v0.1.0 (../mathx)

$ cat src/myapp.eta
(module myapp
  (import std.io)
  (import mathx)
  (begin
    (defun main (args)
      (println (square 7)))))

$ eta build --release
   Compiling mathx v0.1.0 (../mathx)         [O2 …  3 fns]
   Compiling myapp v0.1.0 (.)                [O2 …  4 fns]
   Bundling  myapp                           → .eta/target/release/myapp.etac
$ eta run
49
```

### 18.3 Inspect the resolved graph

```console
$ eta tree
myapp v0.1.0 (.)
└── mathx v0.1.0 (../mathx)
    └── std (bundled, embedded prelude)
```

### 18.4 What got produced

```
myapp/
├── eta.toml
├── eta.lock                                ← generated by `eta add`
└── .eta/
    ├── modules/mathx-0.1.0 → ../../mathx   ← path-dep junction
    └── target/release/
        ├── mathx.etac                      ← O2, no debug, dep-hash table populated
        └── myapp.etac
```

The user has not, at any point in this flow, set `ETA_MODULE_PATH`,
edited a Makefile, or invoked `etac` directly. The existing tools
(`etai`, `etac`, `eta_repl`) are still fully functional for one-off
scripts; the package system is layered on top, not in place of, the
present runtime.

