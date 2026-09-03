#!/usr/bin/env bash
# Animus Engine -- Hardened build script for harness_benchmark.cpp (Milestone 4).
#
# Two targets from the same source, same as this project's other Linux/GCC
# build path (see the root CMakeLists.txt's animus_native target, which
# this script deliberately does not replace or touch):
#
#   build/harness_benchmark        -O3 -march=native, for real measurements.
#   build/harness_benchmark_sanitized   -fsanitize=address,undefined, for
#                                   correctness verification under ASan/UBSan
#                                   -- run this one whenever the ring/header
#                                   logic changes, not for measuring latency
#                                   (sanitizer instrumentation makes the
#                                   numbers meaningless, not just slower).
#
# POSIX-only by design: harness_benchmark.cpp's ShmRing<T> falls back to
# CreateFileMapping on Windows (include/animus/shm_ipc.hpp), but Milestone
# 2's SIGSEGV/kill -9 handling and /dev/shm are POSIX-specific -- this
# script targets Linux/macOS with clang++ or g++, per CLAUDE.md's stated
# "MSVC / GCC" engine toolchain support. On Windows, build
# harness_benchmark.cpp directly with MSVC (cl /std:c17 /O2 /EHsc) instead;
# ASan is available there via /fsanitize=address but UBSan is not, so this
# script's combined sanitizer target has no direct MSVC equivalent.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
SRC="${SCRIPT_DIR}/harness_benchmark.cpp"

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

mkdir -p "${BUILD_DIR}"

COMMON_FLAGS=(-std=c++17 -Wall -Wextra -Wpedantic -I"${REPO_ROOT}/include")
LIBS=(-lpthread) # thread_affinity.hpp/shm_ipc.hpp's POSIX path -- must follow the source on the link line

echo
echo "=== Building hardened release target: ${BUILD_DIR}/harness_benchmark ==="
"${CXX}" -O3 -march=native "${COMMON_FLAGS[@]}" "${SRC}" -o "${BUILD_DIR}/harness_benchmark" "${LIBS[@]}"
echo "-> ${BUILD_DIR}/harness_benchmark"

echo
echo "=== Building ASan+UBSan target: ${BUILD_DIR}/harness_benchmark_sanitized ==="
"${CXX}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${COMMON_FLAGS[@]}" "${SRC}" -o "${BUILD_DIR}/harness_benchmark_sanitized" "${LIBS[@]}"
echo "-> ${BUILD_DIR}/harness_benchmark_sanitized"

cat <<'EOF'

Done. Suggested checks:

  Release run (real numbers):
    ./build/harness_benchmark --events 10000000

  Sanitizer run (correctness, not performance -- expect a slower, noisier
  run; a clean exit with no ASan/UBSan report is the pass criterion, not
  the latency numbers it happens to print):
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
      ./build/harness_benchmark_sanitized --events 100000 --unlink-when-done

  Ctrl+C teardown check (Milestone 2 -- confirms SIGINT detaches cleanly
  without a sanitizer-reported leak/use-after-free from the shared mapping):
    ./build/harness_benchmark_sanitized --events 100000000 &
    sleep 1 && kill -INT %1 && wait
EOF
