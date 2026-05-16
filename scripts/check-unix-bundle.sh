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
Usage: check-unix-bundle.sh --prefix <install-prefix> [--source-packages-dir <packages-dir>] [--require-package-builds]
EOF
}

PREFIX=""
SOURCE_PACKAGES_DIR=""
REQUIRE_PACKAGE_BUILDS=0
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
        --source-packages-dir)
            if [ $# -lt 2 ]; then
                echo "error: --source-packages-dir requires a value" >&2
                usage
                exit 2
            fi
            SOURCE_PACKAGES_DIR="$2"
            shift 2
            ;;
        --require-package-builds)
            REQUIRE_PACKAGE_BUILDS=1
            shift
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
if [ -n "$SOURCE_PACKAGES_DIR" ] && [ ! -d "$SOURCE_PACKAGES_DIR" ]; then
    echo "error: source packages directory does not exist: $SOURCE_PACKAGES_DIR" >&2
    exit 2
fi

BIN_DIR="$PREFIX/bin"
STDLIB_DIR="$PREFIX/stdlib"
PACKAGES_DIR="$PREFIX/packages"

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

required_stdlib=(
    std/core.eta
    std/core.etac
    std/jupyter.eta
    std/jupyter.etac
)
missing_stdlib=()
for artifact in "${required_stdlib[@]}"; do
    if [ ! -f "$STDLIB_DIR/$artifact" ]; then
        missing_stdlib+=("stdlib/$artifact")
    fi
done

required_sidecar_manifests=(
    stdlib/native/log/eta.toml
    stdlib/native/stats/eta.toml
    stdlib/native/torch/eta.toml
    stdlib/native/nng/eta.toml
)
missing_sidecar_manifests=()
for manifest in "${required_sidecar_manifests[@]}"; do
    if [ ! -f "$PACKAGES_DIR/$manifest" ]; then
        missing_sidecar_manifests+=("packages/$manifest")
    fi
done

case "$(uname -s):$(uname -m)" in
    Linux:x86_64) host_target_triple="x86_64-unknown-linux-gnu" ;;
    Linux:aarch64|Linux:arm64) host_target_triple="aarch64-unknown-linux-gnu" ;;
    Darwin:x86_64) host_target_triple="x86_64-apple-darwin" ;;
    Darwin:arm64|Darwin:aarch64) host_target_triple="aarch64-apple-darwin" ;;
    *) host_target_triple="" ;;
esac

find_host_native_artifact() {
    local manifest_path="$1"
    local host_triple="$2"
    awk -v host="${host_triple}" '
        BEGIN { in_target = 0; triple = ""; artifact = ""; found = 0 }
        /^[[:space:]]*\[\[native.targets\]\][[:space:]]*$/ {
            if (in_target && triple == host && artifact != "") {
                print artifact
                found = 1
                exit
            }
            in_target = 1
            triple = ""
            artifact = ""
            next
        }
        in_target && /^[[:space:]]*triple[[:space:]]*=/ {
            if (match($0, /"[^"]+"/)) {
                triple = substr($0, RSTART + 1, RLENGTH - 2)
            }
            next
        }
        in_target && /^[[:space:]]*artifact[[:space:]]*=/ {
            if (match($0, /"[^"]+"/)) {
                artifact = substr($0, RSTART + 1, RLENGTH - 2)
            }
            next
        }
        in_target && /^[[:space:]]*\[/ {
            if (triple == host && artifact != "") {
                print artifact
                found = 1
                exit
            }
            in_target = 0
            triple = ""
            artifact = ""
            next
        }
        END {
            if (!found && in_target && triple == host && artifact != "") {
                print artifact
            }
        }
    ' "$manifest_path"
}

missing_package_manifests=()
missing_package_builds=()
missing_package_sidecars=()
if [ -n "$SOURCE_PACKAGES_DIR" ]; then
    while IFS= read -r source_manifest; do
        rel_path="${source_manifest#${SOURCE_PACKAGES_DIR}/}"
        bundle_manifest="${PACKAGES_DIR}/${rel_path}"
        if [ ! -f "$bundle_manifest" ]; then
            missing_package_manifests+=("packages/${rel_path}")
            continue
        fi

        package_dir="$(dirname "$bundle_manifest")"
        if [ "$REQUIRE_PACKAGE_BUILDS" -eq 1 ]; then
            if [ -d "${package_dir}/src" ] \
               && find "${package_dir}/src" -type f -name "*.eta" | head -n 1 | grep -q .; then
                if [ ! -d "${package_dir}/.eta/target/release" ] \
                   || ! find "${package_dir}/.eta/target/release" -type f -name "*.etac" | head -n 1 | grep -q .; then
                    missing_package_builds+=("packages/${rel_path} -> .eta/target/release/*.etac")
                fi
            fi

            if grep -Eq '^[[:space:]]*kind[[:space:]]*=[[:space:]]*"sidecar"[[:space:]]*$' "$bundle_manifest"; then
                if [ -z "$host_target_triple" ]; then
                    missing_package_sidecars+=("packages/${rel_path} -> unsupported host triple")
                else
                    artifact_rel="$(find_host_native_artifact "$bundle_manifest" "$host_target_triple")"
                    if [ -z "$artifact_rel" ]; then
                        missing_package_sidecars+=("packages/${rel_path} -> missing native target for ${host_target_triple}")
                    elif [ ! -f "${package_dir}/${artifact_rel}" ]; then
                        missing_package_sidecars+=("packages/${rel_path} -> ${artifact_rel}")
                    fi
                fi
            fi
        fi
    done < <(find "$SOURCE_PACKAGES_DIR" -type f -name "eta.toml" | sort)
fi

# Cookbook notebooks (referenced from README.md / TLDR.md / docs/).
# Regression guard: the CMake install rule used to filter on *.eta only,
# silently dropping every *.ipynb. Make sure they're present.
COOKBOOK_DIR="$PREFIX/cookbook"
required_notebooks=(
    notebooks/LanguageBasics.ipynb
    notebooks/AAD.ipynb
    notebooks/Portfolio.ipynb
)
missing_notebooks=()
for nb in "${required_notebooks[@]}"; do
    if [ ! -f "$COOKBOOK_DIR/$nb" ]; then
        missing_notebooks+=("cookbook/$nb")
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
if [ ${#missing_notebooks[@]} -gt 0 ]; then
    ok=0
    echo "[FAIL] Missing cookbook notebooks: ${missing_notebooks[*]}" >&2
fi
if [ ${#missing_sidecar_manifests[@]} -gt 0 ]; then
    ok=0
    echo "[FAIL] Missing native sidecar package manifests: ${missing_sidecar_manifests[*]}" >&2
fi
if [ ${#missing_package_manifests[@]} -gt 0 ]; then
    ok=0
    echo "[FAIL] Missing package manifests from bundle: ${missing_package_manifests[*]}" >&2
fi
if [ ${#missing_package_builds[@]} -gt 0 ]; then
    ok=0
    echo "[FAIL] Missing package build artifacts: ${missing_package_builds[*]}" >&2
fi
if [ ${#missing_package_sidecars[@]} -gt 0 ]; then
    ok=0
    echo "[FAIL] Missing package sidecar artifacts: ${missing_package_sidecars[*]}" >&2
fi

if [ "$ok" -eq 1 ]; then
    echo "[OK] Bundle has required executables, stdlib artifacts, package manifests, and package artifacts."
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
