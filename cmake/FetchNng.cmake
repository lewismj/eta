# ──────────────────────────────────────────────────────────────────────────
# FetchNng.cmake — Fetch nng (nanomsg-next-gen) via FetchContent
#
# Called by eta/CMakeLists.txt when ETA_BUILD_NNG is ON.
# Downloads nng from GitHub and makes the `nng` target available.
#
# On Windows/MSVC nng is built as a SHARED library (DLL) to avoid
# persistent __declspec(dllimport) / static-lib linker mismatches.
# On Linux/macOS it stays static (no issues there).
# ──────────────────────────────────────────────────────────────────────────

include(FetchContent)

# On Windows/MSVC, nng's check_symbol_exists() calls fail because the
# cmake try-compile doesn't link kernel32.lib.  Pre-seed the cache
# variables so the checks are skipped — these APIs are guaranteed to
# exist on Vista+ / UCRT which we require.
if(WIN32 AND MSVC)
    set(NNG_HAVE_CONDVAR        1 CACHE INTERNAL "")
    set(NNG_HAVE_SNPRINTF       1 CACHE INTERNAL "")
    set(NNG_HAVE_TIMESPEC_GET   1 CACHE INTERNAL "")
endif()

FetchContent_Declare(
    nng
    GIT_REPOSITORY https://github.com/nanomsg/nng.git
    GIT_TAG        v1.9.0
)

# Disable nng's own tests and tools — we only need the library.
set(NNG_TESTS OFF CACHE BOOL "" FORCE)
set(NNG_TOOLS OFF CACHE BOOL "" FORCE)

# On Windows, build nng as a shared library (DLL) so the default
# __declspec(dllimport) decorations in nng.h match the import library.
# Save/restore BUILD_SHARED_LIBS so we don't affect other FetchContent targets.
if(WIN32)
    set(_eta_old_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS ON)
endif()

FetchContent_MakeAvailable(nng)

if(WIN32)
    set(BUILD_SHARED_LIBS "${_eta_old_BUILD_SHARED_LIBS}")
endif()

if(MSVC AND TARGET nng)
    # nng v1.9.0 triggers C4022 in win_tcpconn.c on CancelIoEx calls.
    # Keep suppression local to third-party nng sources.
    target_compile_options(nng PRIVATE
        $<$<COMPILE_LANGUAGE:C>:/wd4022>)
endif()

message(STATUS "nng fetched — nng target available")
