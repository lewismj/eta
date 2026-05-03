## Rename `examples` to `cookbook` Plan

## 1. Objective

Rename the repository root `examples/` directory to `cookbook/`, improve its internal structure, and group tests into sub-directories without breaking QA automation.

## 2. Current State (checked before planning)

In this repo, QA references are under `eta/qa/test` (not `qa/tests`).

Key hard references to `examples`:

- Root build config:
  - `CMakeLists.txt`
    - `set(ETA_EXAMPLES_SOURCE_DIR "${ETA_REPO_ROOT_DIR}/examples")`
    - `install(DIRECTORY examples/ DESTINATION examples ...)`
- QA test wiring:
  - `eta/qa/test/CMakeLists.txt`
    - compile definition `ETA_EXAMPLES_DIR="${ETA_EXAMPLES_SOURCE_DIR}"`
  - `eta/qa/test/src/example_runner_tests.cpp`
    - `examples_dir()` fallback search includes `.../examples`
  - `eta/qa/test/src/compiled_example_tests.cpp`
    - same fallback and collection logic
- Release packaging scripts:
  - `scripts/build-release.ps1` copies/prunes `examples`
  - `scripts/build-release.sh` copies/prunes `examples`
- Documentation and guides: many `examples/...` links and commands.

Current `examples/` structure is mixed:
- many top-level runnable `.eta` files
- domain folders (`do-calculus`, `xva-wwr`, `notebooks`)
- one test folder (`torch_tests`)
- an empty `energy-stress/` directory

## 3. Target Structure

Top-level rename:

```text
examples/  ->  cookbook/
```

Test grouping (inside cookbook):

```text
cookbook/
  tests/
    torch/
    integration/      (new bucket for future test-style scripts)
```

Minimum concrete move in this change:

```text
cookbook/torch_tests/*.eta  ->  cookbook/tests/torch/*.eta
```

Optional (same migration or follow-up, depending on scope):
- Move test-like `.eta` scripts into additional `cookbook/tests/<topic>/` folders.
- Keep user-facing runnable demos outside `cookbook/tests/`.

QA source organization (to align with test grouping goal):

```text
eta/qa/test/src/cookbook/
  example_runner_tests.cpp
  compiled_example_tests.cpp
```

(Update `eta/qa/test/CMakeLists.txt` source paths accordingly.)

## 4. Migration Strategy (phased)

### Phase 0: Pre-flight inventory

1. Generate a full reference inventory with `rg -n "examples"`.
2. Label each hit as:
   - runtime/build critical
   - packaging
   - docs/examples text only
3. Confirm no external CI scripts in this repo expect a literal `examples/` artifact path.

### Phase 1: Compatibility-first QA update

1. Introduce `ETA_COOKBOOK_SOURCE_DIR` in root CMake.
2. Keep transitional alias support for `ETA_EXAMPLES_SOURCE_DIR` while migration is in progress.
3. In `eta/qa/test/CMakeLists.txt`, provide cookbook path define(s) to example tests.
4. Update `examples_dir()` logic in:
   - `example_runner_tests.cpp`
   - `compiled_example_tests.cpp`
   to search `cookbook` first, then `examples` as fallback during transition.
5. Update test messages from "examples" to "cookbook/examples" wording for clarity.

### Phase 2: Directory rename + test regrouping

1. `git mv examples cookbook`
2. `git mv cookbook/torch_tests cookbook/tests/torch`
3. Create `cookbook/tests/integration` (empty placeholder with README or keep absent until first file).
4. Move QA cookbook-related C++ tests into `eta/qa/test/src/cookbook/`.

### Phase 3: Reference updates

1. Build/install:
   - root `CMakeLists.txt`: source var + install destination (`cookbook`)
2. QA compile defines and test discovery:
   - `eta/qa/test/CMakeLists.txt`
   - both cookbook runner tests
3. Packaging scripts:
   - `scripts/build-release.ps1`
   - `scripts/build-release.sh`
   - update copy/prune allow-lists from `examples` to `cookbook`
4. Repo docs and command snippets:
   - `README.md`, `TLDR.md`, `docs/*`
5. Source comments/sample strings that still mention `examples/...` where user-facing.

### Phase 4: Cleanup (post-rename)

1. Remove fallback to `examples` from QA test path discovery.
2. Remove `ETA_EXAMPLES_*` transitional naming from CMake/tests.
3. Enforce single canonical directory name: `cookbook`.

## 5. Verification Checklist

1. Static checks:
   - `rg -n "examples/"` returns only intentional historical references (or zero).
   - `rg -n "ETA_EXAMPLES"` returns zero after cleanup phase.
2. Build/tests:
   - configure + build succeeds
   - `eta_core_test` cookbook example runner tests pass
   - compiled cookbook round-trip tests pass (including optimized modes)
3. Packaging:
   - release script outputs include `cookbook/` (not `examples/`)
4. Docs:
   - key quickstart commands run using `cookbook/basics/hello.eta`
   - markdown links resolve for moved files.

## 6. Risks and Mitigations

- Risk: large doc-link churn causes broken references.
  - Mitigation: scripted replace + markdown link validation + focused manual spot-check.
- Risk: QA silently scans wrong directory if fallback remains too long.
  - Mitigation: keep fallback only in transition PR, then remove in cleanup PR.
- Risk: packaging scripts diverge across PowerShell and shell versions.
  - Mitigation: update both scripts in the same PR and validate both paths.

## 7. Delivery Plan (recommended PR split)

1. PR 1: Compatibility plumbing in CMake + QA discovery (no move yet).
2. PR 2: `examples -> cookbook` rename + `torch_tests -> cookbook/tests/torch` + QA test file regrouping.
3. PR 3: docs and packaging sweep, then remove transition aliases/fallbacks.

