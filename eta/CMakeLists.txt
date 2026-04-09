project(eta)


# Specify dependencies.
if(POLICY CMP0167)
    # Boost config/bjam/vcpkg is generally a mess,
    # until Boost gets rid of bjam, set this policy
    # to OLD.
    cmake_policy(SET CMP0167 OLD)
endif()

find_package(Boost 1.88 REQUIRED COMPONENTS unit_test_framework)
if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost not found.")
endif()

# ── Optional libtorch integration ────────────────────────────────────────
option(ETA_BUILD_TORCH "Enable native libtorch bindings (requires libtorch)" ON)

# Backend selector for auto-download: "cpu" (default), "cu118", "cu121", "cu124"
# Only used when libtorch is not already installed.
# CPU works everywhere; CUDA variants need a compatible NVIDIA driver
# (the CUDA toolkit itself is NOT required — libtorch ships all runtime libs).
set(ETA_TORCH_BACKEND "cpu" CACHE STRING
    "libtorch backend for auto-download: cpu, cu118, cu121, or cu124")

if(ETA_BUILD_TORCH)
    find_package(Torch QUIET)
    if(Torch_FOUND)
        message(STATUS "libtorch found — building eta_torch bindings")
    else()
        # libtorch not installed — auto-download the pre-built archive
        message(STATUS "libtorch not found locally — attempting auto-download (backend=${ETA_TORCH_BACKEND})")
        include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FetchLibtorch.cmake")
        find_package(Torch QUIET)
        if(Torch_FOUND)
            message(STATUS "libtorch auto-downloaded — building eta_torch bindings (backend=${ETA_TORCH_BACKEND})")
        else()
            message(STATUS "libtorch auto-download FAILED — skipping eta_torch")
            message(STATUS "  To fix: set -DTorch_DIR=<path>/libtorch/share/cmake/Torch")
            message(STATUS "       or -DCMAKE_PREFIX_PATH=<path>/libtorch")
            set(ETA_BUILD_TORCH OFF CACHE BOOL "" FORCE)
        endif()
    endif()
endif()

add_subdirectory(core)
add_subdirectory(interpreter)
add_subdirectory(compiler)
add_subdirectory(lsp)
add_subdirectory(dap)

if(ETA_BUILD_TORCH)
    add_subdirectory(torch)
endif()

add_subdirectory(test)
add_subdirectory(fuzz)

