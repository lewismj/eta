project(eta)


# Specify dependencies.
if(POLICY CMP0167)
    # Boost config/bjam/vcpkg is generally a mess,
    # until Boost gets rid of bjam, set this policy
    # to OLD.
    cmake_policy(SET CMP0167 OLD)
endif()

# stacktrace_windbg gives rich symbols on Windows; stacktrace_basic is the
# portable fallback on Linux/macOS (addresses only unless addr2line is present).
find_package(Boost 1.88 REQUIRED COMPONENTS unit_test_framework)
if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost not found.")
endif()

# ── libtorch (always required) ────────────────────────────────────────────
set(ETA_TORCH_BACKEND "cpu" CACHE STRING
    "libtorch backend for auto-download: cpu, cu118, cu121, or cu124")

find_package(Torch QUIET)
if(Torch_FOUND)
    message(STATUS "libtorch found — building eta_torch bindings")
else()
    message(STATUS "libtorch not found locally — attempting auto-download (backend=${ETA_TORCH_BACKEND})")
    include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FetchLibtorch.cmake")
    find_package(Torch REQUIRED)
    message(STATUS "libtorch auto-downloaded — building eta_torch bindings (backend=${ETA_TORCH_BACKEND})")
endif()

# ── Utility: copy libtorch DLLs next to a target (Windows only) ──────
# Defined here (parent scope) so all subdirectories can call it.
# Usage: eta_copy_torch_dlls(<target>)
function(eta_copy_torch_dlls target)
    if(WIN32 AND TORCH_INSTALL_PREFIX)
        file(GLOB _torch_dlls "${TORCH_INSTALL_PREFIX}/lib/*.dll")
        foreach(dll IN LISTS _torch_dlls)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll}"
                    $<TARGET_FILE_DIR:${target}>
            )
        endforeach()
        list(LENGTH _torch_dlls _n)
        if(_n GREATER 0)
            message(STATUS "Will copy ${_n} libtorch DLL(s) to ${target} output dir")
        endif()
    endif()
endfunction()

# ── nng (always required) ─────────────────────────────────────────────────
message(STATUS "Fetching nng (nanomsg-next-gen) v1.9.0 ...")
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FetchNng.cmake")
if(NOT TARGET nng)
    message(FATAL_ERROR "nng fetch FAILED — nng is a required dependency")
endif()

# ── Eigen (always required for stats primitives) ──────────────────────────
message(STATUS "Fetching Eigen 3.4.0 (header-only) ...")
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FetchEigen.cmake")
if(NOT TARGET Eigen3::Eigen)
    message(FATAL_ERROR "Eigen fetch FAILED — Eigen is a required dependency")
endif()

# ── Utility: copy nng DLL next to a target (Windows only) ────────────
# On Windows, nng is built as a shared library; the DLL must sit beside
# each executable.  On Linux/macOS nng is static — this is a no-op.
# Usage: eta_copy_nng_dlls(<target>)
function(eta_copy_nng_dlls target)
    if(WIN32 AND TARGET nng)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:nng>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Copying nng DLL to ${target} output dir"
        )
    endif()
endfunction()

# On Linux/macOS, set RPATH so installed binaries find libtorch in ../lib.
# Must be set before add_subdirectory() so it applies to all executable targets.
if(NOT WIN32)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

add_subdirectory(core)
add_subdirectory(torch)
add_subdirectory(nng)
add_subdirectory(stats)
add_subdirectory(interpreter)
add_subdirectory(compiler)
add_subdirectory(lsp)
add_subdirectory(dap)
add_subdirectory(test)
add_subdirectory(test_runner)
add_subdirectory(fuzz)

# Install nng DLL on Windows
if(WIN32 AND TARGET nng)
    install(FILES $<TARGET_FILE:nng> DESTINATION bin)
endif()
