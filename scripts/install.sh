#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────
# install.sh — Install Eta from an extracted release bundle.
#
# Usage (from inside the extracted bundle):
#   ./install.sh                     # install in-place (adds THIS dir to PATH)
#   ./install.sh /usr/local          # copies bin/ + stdlib/ to /usr/local
#   ./install.sh ~/.local            # copies to ~/.local/bin, ~/.local/stdlib
#
# When called with no argument, the bundle directory itself is used;
# bin/ is added to PATH and ETA_MODULE_PATH is set in your shell rc.
#
# When called with a <prefix>, files are copied:
#   <prefix>/bin/         ← etai, eta_repl, eta_lsp, eta_dap
#   <prefix>/stdlib/      ← prelude.eta, std/*.eta
#   <prefix>/editors/     ← VS Code extension (optional)
# ──────────────────────────────────────────────────────────────────────
set -euo pipefail

BUNDLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Parse argument ────────────────────────────────────────────────────
TARGET=""
if [ $# -ge 1 ]; then
    TARGET="$1"
fi

# ── If a target prefix was given, copy files there first ──────────────
if [ -n "$TARGET" ]; then
    echo "▸ Copying files to ${TARGET}..."
    mkdir -p "$TARGET/bin" "$TARGET/stdlib"
    cp -f "$BUNDLE_DIR/bin/"*           "$TARGET/bin/"        2>/dev/null || true
    cp -rf "$BUNDLE_DIR/stdlib/"*       "$TARGET/stdlib/"     2>/dev/null || true
    if [ -d "$BUNDLE_DIR/editors" ]; then
        mkdir -p "$TARGET/editors"
        cp -rf "$BUNDLE_DIR/editors/"*  "$TARGET/editors/"    2>/dev/null || true
    fi
    chmod +x "$TARGET/bin/"* 2>/dev/null || true
    BIN_DIR="$(cd "$TARGET/bin" && pwd)"
    STDLIB_DIR="$(cd "$TARGET/stdlib" && pwd)"
    VSIX_PATH="$TARGET/editors/eta-lang.vsix"
else
    BIN_DIR="${BUNDLE_DIR}/bin"
    STDLIB_DIR="${BUNDLE_DIR}/stdlib"
    VSIX_PATH="${BUNDLE_DIR}/editors/eta-lang.vsix"
fi

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Eta Installer                                             ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo
echo "  bin     : ${BIN_DIR}"
echo "  stdlib  : ${STDLIB_DIR}"
echo

# ── 1. Detect shell config ───────────────────────────────────────────
SHELL_NAME="$(basename "${SHELL:-/bin/bash}")"
case "$SHELL_NAME" in
    zsh)  RC_FILE="$HOME/.zshrc" ;;
    fish) RC_FILE="$HOME/.config/fish/config.fish" ;;
    *)    RC_FILE="$HOME/.bashrc" ;;
esac

# ── 2. Add to PATH + ETA_MODULE_PATH ─────────────────────────────────
MARKER_BEGIN="# >>> eta >>>"
MARKER_END="# <<< eta <<<"
if grep -q "$MARKER_BEGIN" "$RC_FILE" 2>/dev/null; then
    echo "▸ Updating Eta PATH entry in ${RC_FILE}..."
    # Remove the old marker block (everything between >>> eta >>> and <<< eta <<<)
    sed -i.bak "/$MARKER_BEGIN/,/$MARKER_END/d" "$RC_FILE"
    rm -f "${RC_FILE}.bak"
else
    echo "▸ Adding Eta to PATH in ${RC_FILE}..."
fi
{
    echo ""
    echo "$MARKER_BEGIN"
    echo "export PATH=\"${BIN_DIR}:\$PATH\""
    echo "export ETA_MODULE_PATH=\"${STDLIB_DIR}\""
    echo "$MARKER_END"
} >> "$RC_FILE"
echo "  ✓ Done."

# ── 3. Install VS Code extension ─────────────────────────────────────
if command -v code &>/dev/null && [ -f "$VSIX_PATH" ]; then
    echo "▸ Installing VS Code extension..."
    code --install-extension "$VSIX_PATH" --force
    echo "  ✓ VS Code extension installed."
elif [ ! -f "$VSIX_PATH" ]; then
    echo "▸ VS Code extension not in bundle — skipping."
    echo "    Set eta.lsp.serverPath in VS Code settings to:"
    echo "    ${BIN_DIR}/eta_lsp"
    echo "    Set eta.dap.executablePath in VS Code settings to:"
    echo "    ${BIN_DIR}/eta_dap"
else
    echo "▸ 'code' not on PATH — skipping VS Code extension install."
    echo "    To install manually: code --install-extension \"${VSIX_PATH}\" --force"
fi

# ── 4. Smoke test ─────────────────────────────────────────────────────
echo
echo "▸ Verifying..."
for bin in etai eta_repl eta_lsp eta_dap; do
    p="${BIN_DIR}/${bin}"
    if [ -x "$p" ]; then
        echo "  ✓ ${bin}"
    else
        echo "  ✗ ${bin} — not found or not executable"
    fi
done

# ── Done ──────────────────────────────────────────────────────────────
echo
echo "✓ Done!  Open a new terminal (or run: source ${RC_FILE}) then try:"
echo ""
echo "    etai --help"
echo "    eta_repl"
echo ""

