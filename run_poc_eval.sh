#!/usr/bin/env bash
# AnimusCore POC evaluation harness -- Linux/POSIX entry point.
#
# Compiles poc_eval/poc_eval_harness.cpp (a self-contained benchmark against
# AnimusCore_v1/animus.hpp, the core single-header engine -- no other
# repository headers, no third-party dependency) with native optimization
# flags, then runs a 10-second POC soak test and prints the scorecard.
#
# Usage:
#   ./run_poc_eval.sh [duration_seconds] [mpmc_producer_threads] [pin_base_core]
#
# All arguments are optional; defaults are 10 seconds, max(2, logical_cores/2)
# producer threads, and no core pinning. Pass pin_base_core (e.g. `2`) to pin
# every producer/consumer thread to a distinct logical core via
# animus::sys::pin_current_thread_to_core_exclusive (see poc_eval_harness.cpp's
# header comment and docs/technical_eval/COMPATIBILITY_TUNING_GUIDE.md Sec.2)
# for a pinned-vs-unpinned comparison. Requires clang++ or g++ (C++17) on
# PATH; set $CXX to force a specific compiler.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SCRIPT_DIR}/poc_eval/poc_eval_harness.cpp"
OUT="${SCRIPT_DIR}/poc_eval/poc_eval_harness"
DURATION="${1:-10}"
PRODUCERS="${2:-}"
PIN_BASE_CORE="${3:-}"

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

if [[ -z "${PRODUCERS}" && -n "${PIN_BASE_CORE}" ]]; then
    # pin_base_core is positional arg 3 -- producer count (arg 2) must be
    # supplied for it to line up, so fill in the harness's own default here.
    NPROC="$(command -v nproc >/dev/null 2>&1 && nproc || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    if [[ "${NPROC}" -gt 4 ]]; then PRODUCERS=$(( NPROC / 2 )); else PRODUCERS=2; fi
fi

if [[ -n "${PIN_BASE_CORE}" ]]; then
    "${OUT}" "${DURATION}" "${PRODUCERS}" "${PIN_BASE_CORE}"
elif [[ -n "${PRODUCERS}" ]]; then
    "${OUT}" "${DURATION}" "${PRODUCERS}"
else
    "${OUT}" "${DURATION}"
fi
