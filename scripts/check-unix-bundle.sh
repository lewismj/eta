#!/usr/bin/env bash
# Validate a Linux/macOS Eta install bundle.
#
# Usage:
#   bash scripts/check-unix-bundle.sh --prefix <install-prefix>
#
# Exit codes:
#   0  bundle looks good
#   1  required artifacts missing
#   2  invalid arguments

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: check-unix-bundle.sh --prefix <install-prefix>
EOF
}

PREFIX=""
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix|-p)
            if [ $# -lt 2 ]; then
                echo "error: --prefix requires a value" >&2
                usage
                exit 2
            fi
            PREFIX="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

if [ -z "$PREFIX" ]; then
    echo "error: --prefix is required" >&2
    usage
    exit 2
fi

if [ ! -d "$PREFIX" ]; then
    echo "error: install prefix does not exist: $PREFIX" >&2
    exit 2
fi

BIN_DIR="$PREFIX/bin"
STDLIB_DIR="$PREFIX/stdlib"

if [ ! -d "$BIN_DIR" ]; then
    echo "error: bundle bin/ directory missing: $BIN_DIR" >&2
    exit 1
fi

required_bins=(eta etac etai eta_test eta_repl eta_lsp eta_dap eta_jupyter)
missing_bins=()
for bin in "${required_bins[@]}"; do
    if [ ! -x "$BIN_DIR/$bin" ]; then
        missing_bins+=("$bin")
    fi
done

required_stdlib=(prelude.eta prelude.etac)
missing_stdlib=()
for artifact in "${required_stdlib[@]}"; do
    if [ ! -f "$STDLIB_DIR/$artifact" ]; then
        missing_stdlib+=("stdlib/$artifact")
    fi
done

case "$(uname -s)" in
    Darwin*) libext="dylib" ;;
    *)       libext="so" ;;
esac

lib_dir="$PREFIX/lib"
missing_jupyter_libs=()
if [ ! -d "$lib_dir" ]; then
    missing_jupyter_libs+=("libxeus.${libext}" "libxeus-zmq.${libext}" "libzmq.${libext}")
else
    ls "${lib_dir}/"libxeus.*"${libext}"* >/dev/null 2>&1 || missing_jupyter_libs+=("libxeus.${libext}")
    ls "${lib_dir}/"libxeus-zmq.*"${libext}"* >/dev/null 2>&1 || missing_jupyter_libs+=("libxeus-zmq.${libext}")
    ls "${lib_dir}/"libzmq.*"${libext}"* >/dev/null 2>&1 || missing_jupyter_libs+=("libzmq.${libext}")
fi

ok=1
if [ ${#missing_bins[@]} -gt 0 ]; then
    ok=0
    echo "[FAIL] Missing executables: ${missing_bins[*]}" >&2
fi
if [ ${#missing_stdlib[@]} -gt 0 ]; then
    ok=0
    echo "[FAIL] Missing stdlib artifacts: ${missing_stdlib[*]}" >&2
fi

if [ "$ok" -eq 1 ]; then
    echo "[OK] Bundle has required executables and stdlib artifacts."
else
    echo
    echo "Current bin/ contents:" >&2
    ls -la "$BIN_DIR" >&2 || true
    exit 1
fi

if [ ${#missing_jupyter_libs[@]} -gt 0 ]; then
    echo "[WARN] Missing eta_jupyter runtime libraries in ${lib_dir}:" >&2
    for lib in "${missing_jupyter_libs[@]}"; do
        echo "       - ${lib}" >&2
    done
fi
