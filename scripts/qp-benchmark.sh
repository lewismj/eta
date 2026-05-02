#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-./out/wsl-clang-release}"
SIZES="${SIZES:-8,16,24,32}"
REPEATS="${REPEATS:-25}"
LAMBDA="${LAMBDA:-2.0}"
UPPER="${UPPER:-0.35}"
GATE="${GATE:-0}"
OUTPUT_FILE="${OUTPUT_FILE:-}"

if [[ $# -ge 1 ]]; then
  BUILD_DIR="$1"
fi

EXE_CANDIDATES=(
  "${BUILD_DIR}/eta/qa/bench/eta_qp_bench"
  "${BUILD_DIR}/eta/qa/bench/Release/eta_qp_bench"
)

EXE=""
for p in "${EXE_CANDIDATES[@]}"; do
  if [[ -x "$p" ]]; then
    EXE="$p"
    break
  fi
done

if [[ -z "$EXE" ]]; then
  echo "eta_qp_bench not found under '${BUILD_DIR}'. Build eta_all first." >&2
  exit 1
fi

ARGS=(
  --sizes "$SIZES"
  --repeats "$REPEATS"
  --lambda "$LAMBDA"
  --upper "$UPPER"
)

if [[ "$GATE" == "1" ]]; then
  ARGS+=(--gate)
fi

if [[ -z "$OUTPUT_FILE" ]]; then
  "$EXE" "${ARGS[@]}"
  exit $?
fi

"$EXE" "${ARGS[@]}" | tee "$OUTPUT_FILE"
