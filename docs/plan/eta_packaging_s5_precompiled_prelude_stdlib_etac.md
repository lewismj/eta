## Eta Packaging S5 Precompiled Prelude + Bundled stdlib `.etac`

This note records the S5 "precompiled prelude + bundled stdlib `.etac`"
implementation from `docs/plan/eta_packaging_plan.md`.

### Landed scope

- Added stdlib artifact generation in CMake:
  - new build step runs `scripts/build_stdlib_etac.py`
  - mirrors stdlib sources into `${CMAKE_BINARY_DIR}/stdlib`
  - compiles stdlib `.eta` files to `.etac` with `-O --no-debug`
- Added embedded prelude blob generation:
  - `scripts/embed_blob.py` converts `prelude.etac` to C++ byte array source
  - new `eta_embedded_prelude` target provides the blob to runtime binaries
  - embedding is enabled via `ETA_HAS_EMBEDDED_PRELUDE`
- Updated runtime prelude loading order in `Driver::load_prelude()`:
  - embedded blob first
  - then `prelude.etac` from module search roots
  - then `prelude.eta` source fallback
  - repeated `load_prelude()` calls are idempotent and do not reload `std.prelude`
- Refactored `.etac` execution path:
  - shared `execute_deserialized_etac(...)` helper now drives both file-based
    `.etac` execution and embedded-prelude execution
- Added `etac --no-prelude` for bootstrap-safe stdlib artifact builds.
- Updated install layout:
  - existing stdlib `.eta` install remains
  - build-tree stdlib `.etac` artifacts are now installed under `stdlib/`
- Updated test/runtime compile definitions to use the build stdlib root
  (`ETA_STDLIB_RUNTIME_DIR`) where precompiled artifacts are materialized.

### S5 test coverage additions

- `packaging_contract_tests` now includes:
  - embedded prelude preference check
  - stdlib `.etac` artifact load smoke test asserting no stale fallback warning

### S5 test gate

The S5 gate is satisfied when all of these are green:

- `eta_core_test`
- `eta_test`
- `eta_pkg_test`
- `eta_cli_test`

