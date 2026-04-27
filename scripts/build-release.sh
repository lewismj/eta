#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────
# build-release.sh — Build an Eta release bundle on Linux / macOS / WSL
#
# Usage:
#   ./scripts/build-release.sh                          # auto version + dir
#   ./scripts/build-release.sh -v v0.3.0                # explicit version
#   ./scripts/build-release.sh /opt/eta                 # explicit dir
#   ./scripts/build-release.sh -v v0.3.0 /opt/eta      # both
#
# The bundle directory is named  eta-<version>-<platform>  by default
# so that different releases can live side-by-side.
#
# Prerequisites:
#   - CMake ≥ 3.28
#   - C++23 compiler (Clang 17+ or GCC 13+)
#   - Boost ≥ 1.88 (unit_test_framework)
#   - Node.js ≥ 18, npm
# ──────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-release"

# ── Argument handling ─────────────────────────────────────────────────
VERSION=""
INSTALL_DIR=""
ENABLE_TORCH=0
TORCH_BACKEND="cpu"

while [ $# -gt 0 ]; do
    case "$1" in
        -v|--version) VERSION="$2"; shift 2 ;;
        -t|--torch) ENABLE_TORCH=1; shift ;;
        --torch-backend) TORCH_BACKEND="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [-v VERSION] [-t|--torch] [--torch-backend BACKEND] [install-dir]"
            echo ""
            echo "  -v, --version TAG       Version tag (e.g. v0.3.0)."
            echo "                          Auto-detected from git / CMakeLists.txt."
            echo "  -t, --torch             Enable libtorch bindings."
            echo "                          libtorch is auto-downloaded if not installed."
            echo "  --torch-backend BACK    cpu (default), cu126, cu128, or cu130."
            echo "                          CUDA variants need an NVIDIA driver ≥ that CUDA"
            echo "                          version but do NOT need the CUDA toolkit."
            echo "  install-dir             Directory for the bundle."
            echo "                          Defaults to dist/eta-<version>-<platform>."
            exit 0 ;;
        *) INSTALL_DIR="$1"; shift ;;
    esac
done

# ── Detect platform ──────────────────────────────────────────────────
ARCH="$(uname -m)"
case "$(uname -s)" in
    Linux*)  PLATFORM="linux-${ARCH}" ;;
    Darwin*) PLATFORM="macos-${ARCH}" ;;
    *)       PLATFORM="unknown-${ARCH}" ;;
esac

# ── Resolve version tag ──────────────────────────────────────────────
if [ -z "$VERSION" ]; then
    VERSION="$(git -C "$PROJECT_ROOT" describe --tags --abbrev=0 2>/dev/null || true)"
fi
if [ -z "$VERSION" ]; then
    VERSION="$(sed -n 's/.*project\s*(\s*eta\s\+VERSION\s\+\([0-9.]*\).*/\1/p' "$PROJECT_ROOT/CMakeLists.txt" 2>/dev/null || true)"
fi
if [ -z "$VERSION" ]; then
    VERSION="unknown"
fi

# ── Resolve install dir ──────────────────────────────────────────────
if [ -z "$INSTALL_DIR" ]; then
    INSTALL_DIR="${PROJECT_ROOT}/dist/eta-${VERSION}-${PLATFORM}"
fi
PREFIX="$(mkdir -p "$INSTALL_DIR" && cd "$INSTALL_DIR" && pwd)"

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Eta Release Build                                         ║"
echo "╠══════════════════════════════════════════════════════════════╣"
echo "║  Version  : ${VERSION}"
echo "║  Platform : ${PLATFORM}"
echo "║  Install  : ${PREFIX}"
echo "║  Jobs     : ${JOBS}"
if [ "$ENABLE_TORCH" -eq 1 ]; then
echo "║  Torch    : Enabled (${TORCH_BACKEND})"
fi
echo "╚══════════════════════════════════════════════════════════════╝"
echo

# ── Build torch flags ────────────────────────────────────────────────
TORCH_FLAGS=""
if [ "$ENABLE_TORCH" -eq 1 ]; then
    TORCH_FLAGS="-DETA_BUILD_TORCH=ON -DETA_TORCH_BACKEND=${TORCH_BACKEND}"
fi

# ── 1. Configure ─────────────────────────────────────────────────────
echo "▸ [1/6] Configuring CMake (Release)..."
cmake -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      $TORCH_FLAGS \
      "$PROJECT_ROOT"

# ── 2. Build ─────────────────────────────────────────────────────────
echo "▸ [2/6] Building (${JOBS} parallel jobs)..."
cmake --build "$BUILD_DIR" -j"$JOBS"

# ── 3. Install binaries + stdlib ─────────────────────────────────────
echo "▸ [3/6] Installing to ${PREFIX}..."
cmake --install "$BUILD_DIR"

# Verify required runtime binaries are present in the bundle.
echo "  Verifying runtime binaries..."
for bin in etac etai eta_repl eta_lsp eta_dap eta_jupyter; do
    p="${PREFIX}/bin/${bin}"
    if [ ! -x "$p" ]; then
        echo "error: missing required binary: ${p}" >&2
        exit 1
    fi
done

# Verify xeus runtime shared libs landed in lib/ -- without these
# eta_jupyter fails to start with "libxeus.so.N: cannot open shared
# object file: No such file or directory" on a clean target machine.
echo "  Verifying xeus runtime libraries..."
case "$(uname -s)" in
    Darwin*) _libext="dylib" ;;
    *)       _libext="so"    ;;
esac
_missing=""
ls "${PREFIX}/lib/"libxeus.*"${_libext}"*       >/dev/null 2>&1 || _missing="${_missing} libxeus.${_libext}"
ls "${PREFIX}/lib/"libxeus-zmq.*"${_libext}"*   >/dev/null 2>&1 || _missing="${_missing} libxeus-zmq.${_libext}"
ls "${PREFIX}/lib/"libzmq.*"${_libext}"*        >/dev/null 2>&1 || _missing="${_missing} libzmq.${_libext}"
if [ -n "$_missing" ]; then
    echo "  [WARN] xeus runtime libraries missing from lib/ -- eta_jupyter will not run on a clean machine:" >&2
    for m in $_missing; do echo "         - $m" >&2; done
fi
unset _libext _missing

# Keep Jupyter kernelspec logos next to eta_jupyter so `eta_jupyter --install`
# can copy them on target machines (without source-tree paths).
JUPYTER_RES_SRC="${PROJECT_ROOT}/eta/jupyter/resources"
if [ -d "$JUPYTER_RES_SRC" ]; then
    mkdir -p "${PREFIX}/bin/resources"
    cp -f "${JUPYTER_RES_SRC}/logo-32x32.png" "${PREFIX}/bin/resources/" 2>/dev/null || true
    cp -f "${JUPYTER_RES_SRC}/logo-64x64.png" "${PREFIX}/bin/resources/" 2>/dev/null || true
fi

# ── 4. Build VS Code extension ───────────────────────────────────────
VSCODE_SRC="${PROJECT_ROOT}/editors/vscode"
EDITORS_DIR="${PREFIX}/editors"

# Derive semver from VERSION (strip leading 'v'), or fall back to "latest"
SEMVER="${VERSION#v}"
if echo "$SEMVER" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+'; then
    VSIX_LABEL="$SEMVER"
else
    VSIX_LABEL="latest"
fi
VSIX_DEST="${EDITORS_DIR}/eta-lang-${VSIX_LABEL}.vsix"

echo "▸ [4/6] Building VS Code extension (${VSIX_LABEL})..."
mkdir -p "$EDITORS_DIR"
(
    cd "$VSCODE_SRC"
    npm ci --silent
    npm run bundle
    if [ "$VSIX_LABEL" != "latest" ]; then
        npm version "$VSIX_LABEL" --no-git-tag-version --allow-same-version
    fi
    npx @vscode/vsce package -o "$VSIX_DEST" --skip-license
)

# ── 5. Copy helpers + docs ───────────────────────────────────────────
echo "▸ [5/6] Copying install script, docs, and examples..."
[ -f "$PROJECT_ROOT/scripts/install.sh" ]  && cp "$PROJECT_ROOT/scripts/install.sh" "$PREFIX/" && chmod +x "$PREFIX/install.sh"
[ -f "$PROJECT_ROOT/docs/quickstart.md" ]  && cp "$PROJECT_ROOT/docs/quickstart.md" "$PREFIX/"

# Copy examples/
if [ -d "$PROJECT_ROOT/examples" ]; then
    echo "  Copying examples..."
    cp -r "$PROJECT_ROOT/examples" "$PREFIX/examples"
fi

# Make binaries executable
chmod +x "$PREFIX/bin/"* 2>/dev/null || true

# ── 5b. Prune to minimal Linux/macOS layout ──────────────────────────
# Keep only:  bin/  editors/  stdlib/  examples/  lib/  (lib/ holds libtorch .so's
# when torch is enabled — RPATH is set to $ORIGIN/../lib).  Within lib/
# we further drop CMake config packages and pkgconfig files which are
# build-time artifacts not needed at runtime.
echo "  Pruning non-essential install directories..."
for d in "$PREFIX"/*/; do
    name="$(basename "$d")"
    case "$name" in
        bin|editors|stdlib|examples|lib) ;;
        *) echo "    - removing ${name}/"; rm -rf "$d" ;;
    esac
done
if [ -d "$PREFIX/lib" ]; then
    rm -rf "$PREFIX/lib/cmake" "$PREFIX/lib/pkgconfig" 2>/dev/null || true
    # If lib/ ended up empty (no torch), drop it.
    rmdir "$PREFIX/lib" 2>/dev/null || true
fi

# ── 6. Create archive ────────────────────────────────────────────────
BUNDLE_NAME="$(basename "$PREFIX")"
ARCHIVE_PATH="$(dirname "$PREFIX")/${BUNDLE_NAME}.tar.gz"

echo "▸ [6/6] Creating archive ${BUNDLE_NAME}.tar.gz..."
tar -czf "$ARCHIVE_PATH" -C "$(dirname "$PREFIX")" "$BUNDLE_NAME"

# ── Done ─────────────────────────────────────────────────────────────
echo
echo "════════════════════════════════════════════════════════════════"
echo "✓ Release bundle ready!"
echo ""
echo "  Directory : ${PREFIX}"
echo "  Archive   : ${ARCHIVE_PATH}"
echo ""
echo "  To install on a target machine:"
echo "    tar xzf $(basename "$ARCHIVE_PATH")"
echo "    cd ${BUNDLE_NAME}"
echo "    ./install.sh"
echo "════════════════════════════════════════════════════════════════"
