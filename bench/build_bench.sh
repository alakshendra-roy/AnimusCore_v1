#!/usr/bin/env bash
# Standalone build script for the AnimusCore hot-path benchmark harness
# (hotpath_bench.cpp). Zero external dependencies: compiles against the
# single-header animus.hpp core (AnimusCore_v1/animus.hpp) plus the C++17
# standard library and pthreads only.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC="${SCRIPT_DIR}/hotpath_bench.cpp"
OUT="${SCRIPT_DIR}/hotpath_bench"

CXX="${CXX:-}"
if [[ -z "${CXX}" ]]; then
    if command -v g++ >/dev/null 2>&1; then
        CXX=g++
    elif command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    else
        echo "error: neither g++ nor clang++ found on PATH (set \$CXX explicitly)" >&2
        exit 1
    fi
fi

echo "Using compiler: ${CXX} ($(${CXX} --version | head -1))"

"${CXX}" -O3 -std=c++17 -march=native -Wall -Wextra -pthread \
    -I"${REPO_ROOT}/AnimusCore_v1" \
    "${SRC}" -o "${OUT}"

echo "-> ${OUT}"
echo
echo "Run it:"
echo "  ${OUT}"
