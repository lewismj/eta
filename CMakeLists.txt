cmake_minimum_required(VERSION 3.28)
project(eta VERSION 0.1.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# ── VS Code extension build target ──────────────────────────────────
# Runs `npm run bundle` in editors/vscode to produce out/extension.js.
# Usage:  cmake --build <build-dir> --target eta_editor_package
set(VSCODE_EXT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/editors/vscode")
add_custom_target(eta_editor_package
    COMMAND npm run bundle
    WORKING_DIRECTORY "${VSCODE_EXT_DIR}"
    COMMENT "Bundling VS Code extension (editors/vscode → out/extension.js)"
    VERBATIM
)


