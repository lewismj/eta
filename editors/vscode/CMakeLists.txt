cmake_minimum_required(VERSION 3.20)

# ============================================================================
# VS Code extension — build and install targets
#
# Targets (when Node/npm are available on PATH):
#
#   eta_vsix
#       Runs npm ci (once) then vsce package → <build>/editors/vscode/eta-lang.vsix
#       Re-runs incrementally when any .ts / grammar / package.json changes.
#
#   eta_vsix_install
#       Calls `code --install-extension …` to hot-reload the extension into a
#       running VS Code.  Depends on eta_vsix so the .vsix is always fresh.
#
#   cmake --install
#       Copies the .vsix to <prefix>/editors/eta-lang.vsix alongside the
#       binaries (OPTIONAL — silently skipped if eta_vsix was never built).
#
# ============================================================================

# ── Find required tools ──────────────────────────────────────────────────────
# find_program caches NOTFOUND results, so a re-configure after installing
# Node would still report "not found".  Unset stale cache entries first so
# every configure gets a fresh search.

foreach(_var ETA_NODE_EXE ETA_NPM_EXE)
    if(DEFINED CACHE{${_var}} AND NOT EXISTS "${${_var}}")
        unset(${_var} CACHE)
    endif()
endforeach()

if(WIN32)
    # Build a hint list covering every common Windows Node install layout:
    #   - System-wide installer  (C:/Program Files/nodejs)
    #   - nvm-windows current    (%APPDATA%/nvm/current  or  %LOCALAPPDATA%/nvm/…)
    #   - Volta shims            (%LOCALAPPDATA%/Volta/bin)
    #   - fnm shims              (%LOCALAPPDATA%/fnm/aliases/default/installation)
    #   - Scoop                  (%USERPROFILE%/scoop/shims)
    set(_node_hints
        "$ENV{ProgramFiles}/nodejs"
        "$ENV{ProgramW6432}/nodejs"
        "$ENV{LOCALAPPDATA}/nvm/current"
        "$ENV{APPDATA}/nvm/current"
        "$ENV{LOCALAPPDATA}/Volta/bin"
        "$ENV{LOCALAPPDATA}/fnm/aliases/default/installation"
        "$ENV{USERPROFILE}/scoop/shims"
        "$ENV{USERPROFILE}/.nvm/current"
    )
    find_program(ETA_NODE_EXE NAMES node.exe node HINTS ${_node_hints})
    find_program(ETA_NPM_EXE  NAMES npm.cmd  npm  HINTS ${_node_hints})
else()
    # On UNIX Node is usually on PATH; also check nvm/volta shim dirs.
    set(_node_hints
        "$ENV{HOME}/.nvm/current/bin"
        "$ENV{HOME}/.volta/bin"
        "$ENV{HOME}/.local/bin"
    )
    find_program(ETA_NODE_EXE node HINTS ${_node_hints})
    find_program(ETA_NPM_EXE  npm  HINTS ${_node_hints})
endif()

if(NOT ETA_NODE_EXE OR NOT ETA_NPM_EXE)
    # Show every path that was searched so the user knows what to fix.
    message(STATUS "eta_vsix: Node.js/npm not found — searched PATH plus:")
    foreach(_h IN LISTS _node_hints)
        message(STATUS "  ${_h}")
    endforeach()
    message(STATUS
        "eta_vsix: VS Code extension targets skipped.  "
        "Options: install Node >=20 so it is on PATH, "
        "or set ETA_NODE_EXE / ETA_NPM_EXE as CMake cache variables.")
    return()
endif()

message(STATUS "eta_vsix: node = ${ETA_NODE_EXE}")
message(STATUS "eta_vsix: npm  = ${ETA_NPM_EXE}")

# ── Paths ────────────────────────────────────────────────────────────────────

set(_EXT_SRC   "${CMAKE_CURRENT_SOURCE_DIR}")
set(_EXT_BIN   "${CMAKE_CURRENT_BINARY_DIR}")
set(_VSIX_OUT  "${_EXT_BIN}/eta-lang.vsix")
set(_NPM_STAMP "${_EXT_BIN}/npm_ci.stamp")

# ── Step 1 — Restore npm dependencies ───────────────────────────────────────
# Tracked by package-lock.json: only reruns when lockfile or manifest changes.

add_custom_command(
    OUTPUT  "${_NPM_STAMP}"
    COMMAND "${ETA_NPM_EXE}" ci
    COMMAND "${CMAKE_COMMAND}" -E touch "${_NPM_STAMP}"
    WORKING_DIRECTORY "${_EXT_SRC}"
    DEPENDS
        "${_EXT_SRC}/package.json"
        "${_EXT_SRC}/package-lock.json"
    COMMENT "eta_vsix: npm ci — restoring extension dependencies..."
    VERBATIM
)

# ── Step 2 — Bundle + package ────────────────────────────────────────────────
# `vsce package` triggers the vscode:prepublish hook which runs
# `npm run bundle` (esbuild), so no separate bundle step is needed.
# The locally-installed vsce binary (from npm ci) is invoked via `npm exec`.

file(GLOB_RECURSE _EXT_SOURCES CONFIGURE_DEPENDS
    "${_EXT_SRC}/src/*.ts"
    "${_EXT_SRC}/syntaxes/*.json"
    "${_EXT_SRC}/language-configuration.json"
)

add_custom_command(
    OUTPUT  "${_VSIX_OUT}"
    COMMAND "${ETA_NPM_EXE}" exec -- vsce package
            --out          "${_VSIX_OUT}"
            --skip-license
    WORKING_DIRECTORY "${_EXT_SRC}"
    DEPENDS
        "${_NPM_STAMP}"
        "${_EXT_SRC}/package.json"
        ${_EXT_SOURCES}
    COMMENT "eta_vsix: vsce package → eta-lang.vsix"
    VERBATIM
)

# ── eta_vsix — explicit build target (not added to ALL) ──────────────────────

add_custom_target(eta_vsix
    DEPENDS "${_VSIX_OUT}"
)

# ── cmake --install — copy .vsix into <prefix>/editors/ ──────────────────────

install(
    FILES       "${_VSIX_OUT}"
    DESTINATION editors
    COMPONENT   vscode_extension
    OPTIONAL          # skip silently when eta_vsix has not been built
)

# ── eta_vsix_install — hot-reload extension into running VS Code ─────────────

find_program(ETA_CODE_EXE NAMES code code-insiders code.cmd)
if(ETA_CODE_EXE)
    add_custom_target(eta_vsix_install
        COMMAND "${ETA_CODE_EXE}" --install-extension "${_VSIX_OUT}" --force
        DEPENDS "${_VSIX_OUT}"
        COMMENT "eta_vsix_install: loading extension into VS Code (${ETA_CODE_EXE})..."
        VERBATIM
    )
    message(STATUS "eta_vsix_install: code CLI = ${ETA_CODE_EXE}")
else()
    message(STATUS
        "eta_vsix_install: VS Code CLI not found — after building eta_vsix run: "
        "  code --install-extension \"${_VSIX_OUT}\"")
endif()

