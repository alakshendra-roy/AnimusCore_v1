#!/usr/bin/env bash
# Animus Evaluation Kit -- turnkey NASDAQ TotalView-ITCH 5.0 ingest demo.
#
# Builds bench_itch_ingest (Release -O3, header bundle under include/ --
# see README.md's Contents section) and runs it with its built-in pinned
# core affinity defaults: producer on hardware_concurrency()-2, consumer on
# hardware_concurrency()-1 (bench_itch_ingest.cpp's run(), no flag needed --
# only --messages/--capacity are configurable, forwarded from this script's
# own arguments, e.g. ./run_itch_demo.sh --messages 2000000).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Animus Evaluation Kit -- ITCH 5.0 Ingest Demo ==="
echo

echo "--- Configuring: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ---"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

echo
echo "--- Building: cmake --build build -j --target bench_itch_ingest ---"
cmake --build build -j --target bench_itch_ingest

BIN="build/bench_itch_ingest"
if [[ ! -x "$BIN" && -x "build/bench_itch_ingest.exe" ]]; then
    BIN="build/bench_itch_ingest.exe"
fi
if [[ ! -x "$BIN" ]]; then
    echo "error: build succeeded but $BIN was not produced." >&2
    exit 1
fi

echo
echo "--- Running: $BIN $* (pinned core affinity defaults) ---"
echo
exec "$BIN" "$@"
