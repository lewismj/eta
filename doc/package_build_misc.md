# Package Build Misc Notes

## Incident: `eta_http` failed to link on Linux

Date: 2026-05-20

Failure signature:

```text
relocation R_X86_64_PC32 ... can not be used when making a shared object; recompile with -fPIC
```

Root cause:

- `eta_http` builds as a `MODULE` shared library (`libeta_http.so` on Linux).
- When `ETA_HTTP_FETCH_UPSTREAM=ON`, libcurl was fetched and built as a static archive (`libcurl.a`).
- That static archive was not guaranteed to be built with PIC, so linking into the module failed.

Fix applied in this repo:

1. `packages/net/native/http/cmake/FetchLibcurl.cmake`
   - force `POSITION_INDEPENDENT_CODE ON` for fetched curl static/object targets.
2. `packages/net/native/http/CMakeLists.txt`
   - add a configure-time guard that fails early if fetched static `CURL::libcurl` is not PIC.

## Rule (do this for all native sidecars)

If a package builds a `MODULE`/`SHARED` target and links a static dependency, that static dependency must be PIC.

## Implementation checklist for new/updated package deps

1. Immediately after `FetchContent_MakeAvailable(...)`, set PIC on upstream static/object targets:

   ```cmake
   set_target_properties(<static_or_object_target> PROPERTIES
       POSITION_INDEPENDENT_CODE ON
   )
   ```

2. Add a configure-time assertion near `target_link_libraries(...)`:
   - if dep target type is `STATIC_LIBRARY`, require `POSITION_INDEPENDENT_CODE` to be true.
3. Keep this requirement local to package fetch scripts; do not rely on global project-wide PIC defaults.
4. In CI, include at least one Linux build path that exercises fetched upstream deps (`ETA_*_FETCH_UPSTREAM=ON`).

## Quick triage for future linker failures

1. Confirm the failing output target is `MODULE`/`SHARED`.
2. Inspect linked `.a` inputs in the failing link line.
3. For each fetched static lib, verify PIC is explicitly enabled in its fetch CMake.
4. Reconfigure from a clean build directory after CMake changes, then rebuild.

## Incident: package build script failed to configure `http` on Windows CI

Date: 2026-05-20

Failure signature:

```text
Could NOT find Boost (missing: Boost_INCLUDE_DIR) ... CMake configure failed for http
```

Root cause:

- `eta_http` compiles through `eta_core` headers that require Boost headers.
- `scripts/build_packages.ps1` only injected Boost hints for `duckdb`/`lightgbm`, not `http`.
- CI exported `VCPKG_DIR`, but auto-detection only checked `VCPKG_ROOT`, so no fallback include path was discovered.

Fix applied in this repo:

1. `scripts/build_packages.ps1`
   - accept both `VCPKG_ROOT` and `VCPKG_DIR`;
   - compute host-native vcpkg triplet (`x64-windows`, `arm64-windows`, etc.);
   - infer `CMAKE_PREFIX_PATH`, `Boost_DIR`, and `Boost_INCLUDE_DIR` from `installed/<triplet>`;
   - pass Boost args to `http` configure, same as other native packages.
2. `scripts/build_packages.sh`
   - same host-triplet + vcpkg-root detection logic;
   - pass Boost args to `http`.

## Packaging script guardrails

1. Keep native package configure argument wiring consistent across `build_packages.ps1` and `build_packages.sh`.
2. Treat `VCPKG_ROOT` and `VCPKG_DIR` as equivalent roots for CI compatibility.
3. Resolve package-manager install paths from the current host architecture, not from unrelated env defaults.
4. If a sidecar includes `eta_core` runtime headers, assume Boost headers are required unless proven otherwise.

## Incident: package build script resolved `eta_core` from hardcoded `out/...`

Date: 2026-05-21

Failure signature:

```text
Could not resolve eta_core for eta_http. Build eta_core first or pass -DETA_CORE_LIBRARY=...
```

Root cause:

- Sidecar package CMake files had hardcoded fallback paths rooted at `out/...`.
- CI layout used `build/...`, so package-local configure could not locate `eta_core`.
- `build_packages.{ps1,sh}` did not pass `ETA_CORE_LIBRARY` explicitly.

Fix applied in this repo:

1. `scripts/build_packages.ps1`
   - resolve `ETA_CORE_LIBRARY` from `-EtaExecutable` path (with optional explicit override),
   - pass `-DETA_CORE_LIBRARY=...` to `http`, `duckdb`, and `lightgbm`.
2. `scripts/build_packages.sh`
   - same explicit `ETA_CORE_LIBRARY` resolution and wiring for all native packages.
3. package CMake files (`http`, `duckdb`, `lightgbm`)
   - removed hardcoded `out/...` fallback lookup for `eta_core`,
   - now use either in-tree `eta_core` target or explicit `ETA_CORE_LIBRARY`.

## Core-library resolution rule

Native package builds must not assume a specific top-level build directory name (`out`, `build`, etc.).
Always pass `-DETA_CORE_LIBRARY` from the packaging script.
