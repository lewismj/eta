#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────
# build-release.sh — Build an Eta release bundle on Linux / macOS / WSL
#
# Usage:
#   ./scripts/build-release.sh <install-dir>
#   ./scripts/build-release.sh /opt/eta
#   ./scripts/build-release.sh ~/eta-release
#
# The script will:
#   1. Configure + build the C++ binaries (Release mode)
#   2. Install binaries + stdlib to <install-dir>
#   3. Build the VS Code extension and bundle it
#   4. Create a .tar.gz archive beside <install-dir>
#
# Prerequisites:
#   - CMake ≥ 3.28
#   - C++23 compiler (Clang 17+ or GCC 13+)
#   - Boost ≥ 1.88 (unit_test_framework)
#   - Node.js ≥ 18, npm
# ──────────────────────────────────────────────────────────────────────
set -euo pipefail

# ── Argument handling ─────────────────────────────────────────────────
if [ $# -lt 1 ]; then
    echo "Usage: $0 <install-dir>"
    echo ""
    echo "  <install-dir>  Directory to install the release bundle into."
    echo "                 Will be created if it does not exist."
    echo ""
    echo "Examples:"
    echo "  $0 ./dist/eta-linux-x86_64"
    echo "  $0 /opt/eta"
    exit 1
fi

PREFIX="$(mkdir -p "$1" && cd "$1" && pwd)"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-release"

# ── Detect platform ──────────────────────────────────────────────────
ARCH="$(uname -m)"
case "$(uname -s)" in
    Linux*)  PLATFORM="linux-${ARCH}" ;;
    Darwin*) PLATFORM="macos-${ARCH}" ;;
    *)       PLATFORM="unknown-${ARCH}" ;;
esac

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Eta Release Build                                         ║"
echo "╠══════════════════════════════════════════════════════════════╣"
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

