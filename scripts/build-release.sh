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

while [ $# -gt 0 ]; do
    case "$1" in
        -v|--version) VERSION="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [-v VERSION] [install-dir]"
            echo ""
            echo "  -v, --version TAG   Version tag (e.g. v0.3.0)."
            echo "                      Auto-detected from git / CMakeLists.txt."
            echo "  install-dir         Directory for the bundle."
            echo "                      Defaults to dist/eta-<version>-<platform>."
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
echo "╚══════════════════════════════════════════════════════════════╝"
echo

# ── 1. Configure ─────────────────────────────────────────────────────
echo "▸ [1/6] Configuring CMake (Release)..."
cmake -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      "$PROJECT_ROOT"

# ── 2. Build ─────────────────────────────────────────────────────────
echo "▸ [2/6] Building (${JOBS} parallel jobs)..."
cmake --build "$BUILD_DIR" -j"$JOBS"

# ── 3. Install binaries + stdlib ─────────────────────────────────────
echo "▸ [3/6] Installing to ${PREFIX}..."
cmake --install "$BUILD_DIR"

# ── 4. Build VS Code extension ───────────────────────────────────────
VSCODE_SRC="${PROJECT_ROOT}/editors/vscode"
VSCODE_DEST="${PREFIX}/editors/vscode"

echo "▸ [4/6] Building VS Code extension..."
(
    cd "$VSCODE_SRC"
    npm ci --silent
    npm run compile
)

mkdir -p "$VSCODE_DEST/out" "$VSCODE_DEST/syntaxes" "$VSCODE_DEST/bin"
cp -r "$VSCODE_SRC/out/"*                      "$VSCODE_DEST/out/"
cp -r "$VSCODE_SRC/syntaxes/"*                 "$VSCODE_DEST/syntaxes/"
cp    "$VSCODE_SRC/package.json"               "$VSCODE_DEST/"
cp    "$VSCODE_SRC/tsconfig.json"              "$VSCODE_DEST/"
cp    "$VSCODE_SRC/language-configuration.json" "$VSCODE_DEST/"

# Bundle eta_lsp binary into the extension
for f in "$PREFIX/bin/eta_lsp" "$PREFIX/bin/eta_lsp.exe"; do
    [ -f "$f" ] && cp "$f" "$VSCODE_DEST/bin/"
done

# Production npm deps for the extension
(cd "$VSCODE_DEST" && npm install --omit=dev --silent 2>/dev/null || true)

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

