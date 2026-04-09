# ──────────────────────────────────────────────────────────────────────────
# FetchLibtorch.cmake — Auto-download pre-built libtorch if not found
#
# Called by eta/CMakeLists.txt when ETA_BUILD_TORCH is ON but
# find_package(Torch) failed. Downloads the official pre-built archive
# from https://download.pytorch.org and sets Torch_DIR so a subsequent
# find_package(Torch) succeeds.
#
# Options consumed:
#   ETA_TORCH_BACKEND   "cpu" (default), "cu118", "cu121", or "cu124"
#   ETA_LIBTORCH_VER    Version string, e.g. "2.5.1" (default)
#
# The download is cached in ${CMAKE_BINARY_DIR}/_libtorch_dl/ so
# re-configures don't re-download.
#
# Notes:
#   • CPU build is ~200 MB, works everywhere, no driver needed.
#   • CUDA builds are ~2 GB but ship all runtime libs — only the
#     NVIDIA kernel driver must be installed (no CUDA toolkit needed).
#   • Driver compatibility: a libtorch built against CUDA X.Y works
#     with any driver whose "CUDA Version" (shown by nvidia-smi) is ≥ X.Y.
# ──────────────────────────────────────────────────────────────────────────

if(NOT DEFINED ETA_TORCH_BACKEND)
    set(ETA_TORCH_BACKEND "cpu" CACHE STRING
        "libtorch backend: cpu, cu118, cu121, or cu124")
endif()

if(NOT DEFINED ETA_LIBTORCH_VER)
    set(ETA_LIBTORCH_VER "2.5.1" CACHE STRING
        "libtorch version to download (e.g. 2.5.1)")
endif()

# ── Build the download URL ────────────────────────────────────────────

set(_lt_ver   "${ETA_LIBTORCH_VER}")
set(_lt_back  "${ETA_TORCH_BACKEND}")

# Determine the URL channel (cpu → "cpu", cuXXX → "cuXXX")
if(_lt_back STREQUAL "cpu")
    set(_lt_channel "cpu")
    set(_lt_suffix  "cpu")
else()
    set(_lt_channel "${_lt_back}")
    set(_lt_suffix  "${_lt_back}")
endif()

# Platform-specific archive name
if(WIN32)
    set(_lt_archive "libtorch-win-shared-with-deps-${_lt_ver}%2B${_lt_suffix}.zip")
    set(_lt_ext "zip")
elseif(APPLE)
    # macOS: PyTorch provides arm64 or x86_64 CPU-only builds
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(_lt_archive "libtorch-macos-arm64-${_lt_ver}.zip")
    else()
        set(_lt_archive "libtorch-macos-x86_64-${_lt_ver}.zip")
    endif()
    set(_lt_channel "cpu")
    set(_lt_ext "zip")
else()
    # Linux — use cxx11 ABI by default (matches modern distros / GCC ≥ 5)
    set(_lt_archive "libtorch-cxx11-abi-shared-with-deps-${_lt_ver}%2B${_lt_suffix}.zip")
    set(_lt_ext "zip")
endif()

set(_lt_url "https://download.pytorch.org/libtorch/${_lt_channel}/${_lt_archive}")

# ── Download + extract ────────────────────────────────────────────────

set(_lt_dl_dir   "${CMAKE_BINARY_DIR}/_libtorch_dl")
set(_lt_zip      "${_lt_dl_dir}/libtorch.${_lt_ext}")
set(_lt_root     "${_lt_dl_dir}/libtorch")
set(_lt_stamp    "${_lt_dl_dir}/.stamp_${_lt_ver}_${_lt_back}")

if(NOT EXISTS "${_lt_stamp}")
    message(STATUS "")
    message(STATUS "╔══════════════════════════════════════════════════════════════╗")
    message(STATUS "║  Downloading libtorch ${_lt_ver} (${_lt_back})              ║")
    message(STATUS "╚══════════════════════════════════════════════════════════════╝")
    message(STATUS "  URL: ${_lt_url}")
    message(STATUS "  Destination: ${_lt_dl_dir}")
    message(STATUS "")

    file(MAKE_DIRECTORY "${_lt_dl_dir}")

    # Clean previous extracts if switching backend/version
    if(EXISTS "${_lt_root}")
        file(REMOVE_RECURSE "${_lt_root}")
    endif()

    file(DOWNLOAD
        "${_lt_url}" "${_lt_zip}"
        SHOW_PROGRESS
        STATUS _lt_dl_status
        TLS_VERIFY ON
    )
    list(GET _lt_dl_status 0 _lt_dl_code)
    list(GET _lt_dl_status 1 _lt_dl_msg)
    if(NOT _lt_dl_code EQUAL 0)
        message(WARNING
            "Failed to download libtorch:\n"
            "  URL:    ${_lt_url}\n"
            "  Error:  ${_lt_dl_msg}\n"
            "\n"
            "You can download manually and set -DTorch_DIR=<path>/libtorch/share/cmake/Torch\n"
            "or -DCMAKE_PREFIX_PATH=<path>/libtorch")
        return()
    endif()

    message(STATUS "Extracting libtorch...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf "${_lt_zip}"
        WORKING_DIRECTORY "${_lt_dl_dir}"
        RESULT_VARIABLE _lt_extract_result
    )
    if(NOT _lt_extract_result EQUAL 0)
        message(WARNING "Failed to extract libtorch archive")
        return()
    endif()

    # Remove the zip to save disk space (~200MB–2GB)
    file(REMOVE "${_lt_zip}")

    # Write stamp so we skip on next configure
    file(WRITE "${_lt_stamp}" "${_lt_ver}+${_lt_back}")

    message(STATUS "libtorch ${_lt_ver} (${_lt_back}) ready at ${_lt_root}")
else()
    message(STATUS "Using cached libtorch ${_lt_ver} (${_lt_back}) from ${_lt_root}")
endif()

# ── Point find_package(Torch) at the downloaded tree ──────────────────

set(Torch_DIR "${_lt_root}/share/cmake/Torch" CACHE PATH
    "Path to TorchConfig.cmake (auto-set by FetchLibtorch)" FORCE)

# Also add to CMAKE_PREFIX_PATH for transitive dependencies (Caffe2, etc.)
list(APPEND CMAKE_PREFIX_PATH "${_lt_root}")

