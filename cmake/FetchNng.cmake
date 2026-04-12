# ──────────────────────────────────────────────────────────────────────────
# FetchNng.cmake — Fetch nng (nanomsg-next-gen) via FetchContent
#
# Called by eta/CMakeLists.txt when ETA_BUILD_NNG is ON.
# Downloads nng from GitHub and makes the `nng` target available.
# nng builds as a static library by default — no DLL copying needed.
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

FetchContent_MakeAvailable(nng)

message(STATUS "nng fetched — nng target available")
