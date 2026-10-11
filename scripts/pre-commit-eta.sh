#!/usr/bin/env bash
#
# pre-commit-eta.sh - staged-index formatter and linter gate for Eta sources.
#
# This hook checks the *staged index bytes* of every added/copied/modified
# `.eta` file, never the unstaged working tree, so a partial `git add -p`
# stages exactly what is verified. It runs the formatter check first and the
# linter second, both through their `--stdin` contracts, and fixes/stages
# nothing. A non-zero exit blocks the commit.
#
# Windows: run from Git Bash (bundled with Git for Windows) or WSL. The binary
# locations may be supplied through ETA_FMT / ETA_LINT, otherwise the hook looks
# for `eta_fmt` / `eta_lint` on PATH.
#
# Install:
#   ln -s ../../scripts/pre-commit-eta.sh .git/hooks/pre-commit
# or copy it into `.git/hooks/pre-commit` and mark it executable.

set -u

ETA_FMT="${ETA_FMT:-eta_fmt}"
ETA_LINT="${ETA_LINT:-eta_lint}"

if ! command -v "$ETA_FMT" >/dev/null 2>&1; then
    echo "pre-commit-eta: formatter not found (set ETA_FMT or add eta_fmt to PATH)" >&2
    exit 2
fi
if ! command -v "$ETA_LINT" >/dev/null 2>&1; then
    echo "pre-commit-eta: linter not found (set ETA_LINT or add eta_lint to PATH)" >&2
    exit 2
fi

status=0

# -z yields NUL-separated, unquoted paths so spaces and unusual names are safe.
while IFS= read -r -d '' path; do
    case "$path" in
        *.eta) ;;
        *) continue ;;
    esac

    # Read the exact staged bytes for this path from the index.
    staged="$(git show ":$path" 2>/dev/null)"
    if [ $? -ne 0 ]; then
        echo "pre-commit-eta: cannot read staged bytes for $path" >&2
        status=2
        continue
    fi

    if ! printf '%s' "$staged" | "$ETA_FMT" --check --stdin --stdin-filename "$path" >/dev/null; then
        echo "pre-commit-eta: $path is not formatted (run: eta_fmt $path)" >&2
        status=1
    fi

    if ! printf '%s' "$staged" | "$ETA_LINT" --max-warnings 0 --stdin --stdin-filename "$path"; then
        echo "pre-commit-eta: $path has lint findings" >&2
        status=1
    fi
done < <(git diff --cached --name-only --diff-filter=ACM -z)

exit "$status"

