cmake_minimum_required(VERSION 3.28)
project(eta VERSION 0.1.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── MSVC: use /Z7 (embedded debug info) globally ─────────────────────────────
# libtorch's Caffe2Targets.cmake injects /Z7 via INTERFACE_COMPILE_OPTIONS.
# CMake 3.25+ defaults to /Zi unless CMAKE_MSVC_DEBUG_INFORMATION_FORMAT is
# set; having both on the same command line produces warning D9025.
# Setting this before any add_subdirectory() suppresses the warning project-wide.
if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded"
        CACHE STRING "MSVC debug-info format (Embedded=/Z7, ProgramDatabase=/Zi)" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
elseif(MSVC)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON) 
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>")
endif()

add_subdirectory(eta)

# ── Install the standard library alongside binaries ──────────────────
# Layout: <prefix>/bin/etai, <prefix>/stdlib/prelude.eta, <prefix>/stdlib/std/*.eta
install(DIRECTORY stdlib/ DESTINATION stdlib
        FILES_MATCHING PATTERN "*.eta")

# ── Install example programs ─────────────────────────────────────────
install(DIRECTORY examples/ DESTINATION examples
        FILES_MATCHING PATTERN "*.eta")

# ── VS Code extension package target ─────────────────────────────────
# Builds a .vsix and places it in ${CMAKE_BINARY_DIR}/bin/.
#
#   Local:  cmake --build build --target eta_editor_package
#           → build/bin/eta-lang-latest.vsix
#
#   CI:     cmake -DETA_VSIX_VERSION=0.1.8 ...
#           cmake --build build --target eta_editor_package
#           → build/bin/eta-lang-0.1.8.vsix
set(ETA_VSIX_VERSION "latest" CACHE STRING
    "Version label for the .vsix (e.g. 0.1.8 from CI tag, or 'latest' for local builds)")

set(VSCODE_EXT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/editors/vscode")
set(VSIX_OUTPUT    "${CMAKE_BINARY_DIR}/bin/eta-lang-${ETA_VSIX_VERSION}.vsix")

# ── eta_all — rebuild every component in one command ─────────────────
# CMake's built-in "all" target covers normal libraries and executables
# but skips CUSTOM targets (eta_editor_package).  This umbrella target
# lets you build absolutely everything with:
#
#   cmake --build build --target eta_all
#
add_custom_target(eta_all
    COMMENT "Building all Eta components (binaries + tests + VS Code extension)"
)
# Executables + test target (added by subdirectories)
add_dependencies(eta_all
    etai eta_repl etac eta_lsp eta_dap
    eta_core_test
)

set(VSIX_FILENAME "eta-lang-${ETA_VSIX_VERSION}.vsix")

if(ETA_VSIX_VERSION STREQUAL "latest")
    # Local build — keep package.json version as-is.
    # Write the .vsix into the extension working directory first (avoids
    # Node/vsce mangling WSL /mnt/… paths into invalid Windows paths),
    # then cmake -E copy moves it to the final output location.
    add_custom_target(eta_editor_package
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
        COMMAND npm ci --ignore-scripts
        COMMAND npx vsce package -o "${VSIX_FILENAME}" --skip-license
        COMMAND ${CMAKE_COMMAND} -E copy "${VSIX_FILENAME}" "${VSIX_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${VSIX_FILENAME}"
        WORKING_DIRECTORY "${VSCODE_EXT_DIR}"
        COMMENT "Packaging VS Code extension → bin/eta-lang-latest.vsix"
        VERBATIM
    )
else()
    # CI / tagged build — stamp the tag version into package.json first
    add_custom_target(eta_editor_package
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
        COMMAND npm ci --ignore-scripts
        COMMAND npm version "${ETA_VSIX_VERSION}" --no-git-tag-version --allow-same-version
        COMMAND npx vsce package -o "${VSIX_FILENAME}" --skip-license
        COMMAND ${CMAKE_COMMAND} -E copy "${VSIX_FILENAME}" "${VSIX_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${VSIX_FILENAME}"
        WORKING_DIRECTORY "${VSCODE_EXT_DIR}"
        COMMENT "Packaging VS Code extension ${ETA_VSIX_VERSION} → bin/eta-lang-${ETA_VSIX_VERSION}.vsix"
        VERBATIM
    )
endif()

# Wire the VS Code extension into the eta_all umbrella
add_dependencies(eta_all eta_editor_package)

