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
            echo "  --torch-backend BACK    cpu (default), cu118, cu121, or cu124."
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

# ── 4. Build VS Code extension ───────────────────────────────────────
VSCODE_SRC="${PROJECT_ROOT}/editors/vscode"
EDITORS_DIR="${PREFIX}/editors"
VSIX_DEST="${EDITORS_DIR}/eta-lang.vsix"

echo "▸ [4/6] Building VS Code extension..."
mkdir -p "$EDITORS_DIR"
(
    cd "$VSCODE_SRC"
    npm ci --silent
    npx @vscode/vsce package -o "$VSIX_DEST" --skip-license
)

# ── 5. Copy helpers + docs ───────────────────────────────────────────
echo "▸ [5/6] Copying install script and docs..."
[ -f "$PROJECT_ROOT/scripts/install.sh" ]  && cp "$PROJECT_ROOT/scripts/install.sh" "$PREFIX/" && chmod +x "$PREFIX/install.sh"
[ -f "$PROJECT_ROOT/TESTING.md" ]          && cp "$PROJECT_ROOT/TESTING.md" "$PREFIX/"

# Make binaries executable
chmod +x "$PREFIX/bin/"* 2>/dev/null || true

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

