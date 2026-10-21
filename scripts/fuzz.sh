#!/usr/bin/env bash
# Run the libFuzzer targets from eta/qa/fuzz (needs a Clang build with eta_fuzz_* built).
#
#   scripts/fuzz.sh [--build-dir DIR] [--out DIR] [--runs N | --seconds N] [target...]
#
#   target       corpus name: nanbox intern_table heap formatter cst etac wire_format json framing
#                (default: all of them)
#   --runs N     deterministic run: -seed=1, replay the seeds then N mutations (default 20000, used by CI)
#   --seconds N  time-boxed exploration (used by the fuzz workflow)
#   --build-dir  where to look for eta_fuzz_* (default: build)
#   --out        new corpus entries go to OUT/corpus/<target>, crash inputs to
#                OUT/artifacts/<target>/, logs to OUT/<target>.log (default: BUILD/fuzz-out)
#
# Seeds are read from eta/qa/fuzz/samples/<target> and eta/qa/fuzz/corpus/<target>.
# Exits non-zero if any target crashed or could not be found.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
fuzz_src="$repo/eta/qa/fuzz"
build="$repo/build"
out=""
mode_args=(-seed=1 -runs=20000)
targets=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) build="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --runs) mode_args=(-seed=1 "-runs=$2"); shift 2 ;;
        --seconds) mode_args=("-max_total_time=$2"); shift 2 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) targets+=("$1"); shift ;;
    esac
done

all_targets=(nanbox intern_table heap formatter cst etac wire_format json framing)
[[ ${#targets[@]} -eq 0 ]] && targets=("${all_targets[@]}")
[[ -n "$out" ]] || out="$build/fuzz-out"

group() { [[ -n "${GITHUB_ACTIONS:-}" ]] && echo "::group::$1" || echo "== $1"; }
endgroup() { [[ -n "${GITHUB_ACTIONS:-}" ]] && echo "::endgroup::" || true; }

failed=()
for t in "${targets[@]}"; do
    exe="$(find "$build" -type f -name "eta_fuzz_$t" -perm -u+x 2>/dev/null | head -n1)"
    if [[ -z "$exe" ]]; then
        echo "eta_fuzz_$t not found under $build" >&2
        failed+=("$t")
        continue
    fi

    corpus="$out/corpus/$t"
    artifacts="$out/artifacts/$t"
    log="$out/$t.log"
    mkdir -p "$corpus" "$artifacts"

    # -close_fd_mask=3 silences the targets' own stdout/stderr (e.g. framing
    # warnings); libFuzzer and ASan still report through a duplicated fd.
    args=("${mode_args[@]}" -timeout=25 -rss_limit_mb=2048 -close_fd_mask=3
          -print_final_stats=1 "-artifact_prefix=$artifacts/")
    [[ -f "$fuzz_src/dict/$t.dict" ]] && args+=("-dict=$fuzz_src/dict/$t.dict")

    seeds=()
    for d in "$fuzz_src/samples/$t" "$fuzz_src/corpus/$t"; do
        [[ -d "$d" ]] && seeds+=("$d")
    done

    group "eta_fuzz_$t"
    if "$exe" "${args[@]}" "$corpus" "${seeds[@]}" >"$log" 2>&1; then
        grep -E '^stat::(number_of_executed_units|peak_rss_mb)|^Done ' "$log" || true
        echo "eta_fuzz_$t: ok"
    else
        tail -n 80 "$log"
        echo "eta_fuzz_$t: FAILED (log: $log, inputs: $artifacts)"
        failed+=("$t")
    fi
    endgroup
done

if [[ ${#failed[@]} -gt 0 ]]; then
    echo "fuzz failures: ${failed[*]}" >&2
    exit 1
fi
