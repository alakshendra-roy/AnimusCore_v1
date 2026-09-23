#!/usr/bin/env bash
# AnimusCore POC evaluation harness -- Linux/POSIX entry point.
#
# Compiles poc_eval/poc_eval_harness.cpp (a self-contained benchmark against
# AnimusCore_v1/animus.hpp, the core single-header engine -- no other
# repository headers, no third-party dependency) with native optimization
# flags, then runs a 10-second POC soak test and prints the scorecard.
#
# Usage:
#   ./run_poc_eval.sh [duration_seconds] [mpmc_producer_threads]
#
# Both arguments are optional; defaults are 10 seconds and
# max(2, logical_cores/2) producer threads. Requires clang++ or g++ (C++17)
# on PATH; set $CXX to force a specific compiler.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/poc_eval/poc_eval_harness.cpp"
OUT="${SCRIPT_DIR}/poc_eval/poc_eval_harness"
DURATION="${1:-10}"
PRODUCERS="${2:-}"

CXX="${CXX:-}"
if [[ -z "${CXX}" ]]; then
    if command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    elif command -v g++ >/dev/null 2>&1; then
        CXX=g++
    else
        echo "error: neither clang++ nor g++ found on PATH (set \$CXX explicitly)" >&2
        exit 1
    fi
fi

echo "Using compiler: ${CXX} ($(${CXX} --version | head -1))"
echo "Building poc_eval_harness (-O3 -march=native -std=c++17)..."

"${CXX}" -O3 -march=native -std=c++17 -Wall -Wextra -pthread \
    "${SRC}" -o "${OUT}"

echo "-> ${OUT}"
echo
echo "Running ${DURATION}s POC soak test..."
echo

if [[ -n "${PRODUCERS}" ]]; then
    "${OUT}" "${DURATION}" "${PRODUCERS}"
else
    "${OUT}" "${DURATION}"
fi
