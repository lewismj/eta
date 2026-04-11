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

if(ETA_VSIX_VERSION STREQUAL "latest")
    # Local build — keep package.json version as-is
    add_custom_target(eta_editor_package
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
        COMMAND npm ci --ignore-scripts
        COMMAND npx vsce package -o "${VSIX_OUTPUT}" --skip-license
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
        COMMAND npx vsce package -o "${VSIX_OUTPUT}" --skip-license
        WORKING_DIRECTORY "${VSCODE_EXT_DIR}"
        COMMENT "Packaging VS Code extension ${ETA_VSIX_VERSION} → bin/eta-lang-${ETA_VSIX_VERSION}.vsix"
        VERBATIM
    )
endif()


