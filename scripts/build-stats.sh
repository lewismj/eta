#!/bin/bash
set -e
BUILD_DIR="/mnt/c/Users/lewis/develop/eta/out/wsl-clang-release"
SRC_DIR="/mnt/c/Users/lewis/develop/eta"
LOG="$BUILD_DIR/eta_stats_build.log"

echo "=== Reconfiguring with ETA_BUILD_STATS=ON ===" > "$LOG" 2>&1
cd "$BUILD_DIR"
/usr/local/bin/cmake "$SRC_DIR" -DETA_BUILD_STATS=ON >> "$LOG" 2>&1
echo "=== Building eta_core_test ===" >> "$LOG" 2>&1
/usr/local/bin/cmake --build "$BUILD_DIR" --target eta_core_test -j 14 >> "$LOG" 2>&1
echo "=== BUILD COMPLETE ===" >> "$LOG" 2>&1

