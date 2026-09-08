#!/usr/bin/env bash
# Animus Evaluation Kit -- turnkey SPSC ring buffer demo.
#
# Builds bench_ring_buffer (Release, -O3/-march=native, LTO/IPO -- see
# CMakeLists.txt) and runs it with its default core pinning: producer on
# core 2, consumer on core 3 (README.md's Linux kernel prerequisites
# section explains why -- invariant TSC, isolcpus, performance governor).
# Pass a different core pair as this script's own arguments, e.g.
# ./run_demo.sh 4 5.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Animus Evaluation Kit -- Ring Buffer Demo ==="
echo

echo "--- Configuring: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ---"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

echo
echo "--- Building: cmake --build build -j --target bench_ring_buffer ---"
cmake --build build -j --target bench_ring_buffer

BIN="build/bench_ring_buffer"
if [[ ! -x "$BIN" && -x "build/bench_ring_buffer.exe" ]]; then
    BIN="build/bench_ring_buffer.exe"
fi
if [[ ! -x "$BIN" ]]; then
    echo "error: build succeeded but $BIN was not produced." >&2
    exit 1
fi

echo
echo "--- Running: $BIN $* ---"
echo
exec "$BIN" "$@"
