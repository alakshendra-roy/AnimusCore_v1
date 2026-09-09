#!/usr/bin/env bash
# Animus Core -- Evaluation Environment Verification Script
#
# Confirms a Linux host is fit to reproduce the ITCH 5.0 ingestion benchmark
# (bench_itch_ingest, adapters/itch50/) before anyone trusts numbers off it:
# toolchain support for C++17, a CMake new enough for this repo's
# CMakeLists.txt (cmake_minimum_required is 3.15 there; this script asks for
# 3.16 as the evaluation floor), CPU invariant-TSC and cache-line-size
# properties the benchmark's timing and padding assumptions depend on, and
# whether the run is core-isolated. It performs no network access -- every
# check reads local toolchain output or /proc.
#
# Usage:
#   ./scripts/verify_env.sh [--build-dir DIR] [--skip-build]
#
#   --build-dir DIR   Out-of-source CMake build directory (default: build_eval)
#   --skip-build      Run environment/CPU checks only; skip the CMake
#                       configure + bench_itch_ingest build step
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${REPO_ROOT}/build_eval"
SKIP_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --help|-h)
            sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown argument: $1 (--help for usage)" >&2; exit 2 ;;
    esac
done

# --- ANSI colors (disabled automatically when stdout isn't a terminal) ------
if [[ -t 1 ]]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YLW=$'\033[33m'
    C_CYN=$'\033[36m'; C_BLD=$'\033[1m'; C_RST=$'\033[0m'
else
    C_RED=""; C_GRN=""; C_YLW=""; C_CYN=""; C_BLD=""; C_RST=""
fi

FAILS=0
WARNINGS=0

banner() {
    echo
    echo "${C_BLD}${C_CYN}================================================================================${C_RST}"
    echo "${C_BLD}${C_CYN} $1${C_RST}"
    echo "${C_BLD}${C_CYN}================================================================================${C_RST}"
}

pass() { echo "  ${C_GRN}[ OK ]${C_RST} $1"; }
warn() { echo "  ${C_YLW}[WARN]${C_RST} $1"; WARNINGS=$((WARNINGS + 1)); }
fail() { echo "  ${C_RED}[FAIL]${C_RST} $1"; FAILS=$((FAILS + 1)); }

version_ge() {
    # version_ge A B -> true if A >= B, comparing dotted version strings
    [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n1)" == "$2" ]]
}

# --- Step 1: toolchain / C++17 support ---------------------------------------
banner "Step 1/4 -- Compiler toolchain (C++17 support)"

COMPILER_OK=0

if command -v g++ >/dev/null 2>&1; then
    GCC_VER="$(g++ -dumpfullversion -dumpversion 2>/dev/null || g++ -dumpversion)"
    if version_ge "${GCC_VER}" "9.0.0"; then
        pass "g++ ${GCC_VER} found (>= 9, C++17 supported)"
        COMPILER_OK=1
    else
        fail "g++ ${GCC_VER} found, but < 9 -- C++17 support is incomplete/unreliable"
    fi
else
    warn "g++ not found"
fi

if command -v clang++ >/dev/null 2>&1; then
    CLANG_VER="$(clang++ --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
    CLANG_VER="${CLANG_VER:-0.0.0}"
    if version_ge "${CLANG_VER}" "10.0.0"; then
        pass "clang++ ${CLANG_VER} found (>= 10, C++17 supported)"
        COMPILER_OK=1
    else
        fail "clang++ ${CLANG_VER} found, but < 10 -- C++17 support is incomplete/unreliable"
    fi
else
    warn "clang++ not found"
fi

if [[ ${COMPILER_OK} -eq 0 ]]; then
    fail "no compiler with adequate C++17 support found (need g++ >= 9 or clang++ >= 10)"
fi

# --- Step 2: CMake version ---------------------------------------------------
banner "Step 2/4 -- CMake version (>= 3.16)"

if command -v cmake >/dev/null 2>&1; then
    CMAKE_VER="$(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')"
    if version_ge "${CMAKE_VER}" "3.16.0"; then
        pass "cmake ${CMAKE_VER} found"
    else
        fail "cmake ${CMAKE_VER} found, but < 3.16"
    fi
else
    fail "cmake not found"
fi

# --- Step 3: CPU features (invariant TSC, cache line size) ------------------
banner "Step 3/4 -- CPU timing/cache properties"

if [[ -r /proc/cpuinfo ]]; then
    CPU_FLAGS="$(grep -m1 '^flags' /proc/cpuinfo || true)"

    if echo "${CPU_FLAGS}" | grep -qw "constant_tsc"; then
        pass "constant_tsc present (TSC ticks at a fixed rate regardless of P-state)"
    else
        fail "constant_tsc NOT present -- rdtsc-based timing will be unreliable"
    fi

    if echo "${CPU_FLAGS}" | grep -qw "nonstop_tsc"; then
        pass "nonstop_tsc present (TSC keeps ticking through C-states)"
    else
        fail "nonstop_tsc NOT present -- rdtsc-based timing will be unreliable across idle states"
    fi

    CACHE_LINE=""
    if [[ -r /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size ]]; then
        CACHE_LINE="$(cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size)"
    else
        CACHE_LINE="$(grep -m1 '^cache_alignment' /proc/cpuinfo | grep -oE '[0-9]+' || true)"
    fi

    if [[ "${CACHE_LINE}" == "64" ]]; then
        pass "cache line size is 64 bytes (matches alignas(64) padding assumptions)"
    elif [[ -n "${CACHE_LINE}" ]]; then
        fail "cache line size is ${CACHE_LINE} bytes, not 64 -- alignas(64) padding assumptions do not hold"
    else
        warn "could not determine cache line size from /proc/cpuinfo or sysfs"
    fi
else
    fail "/proc/cpuinfo not readable -- cannot verify invariant TSC or cache line size (Linux only)"
fi

# --- Step 4: air-gapped configure + build of bench_itch_ingest --------------
banner "Step 4/4 -- Build validation (bench_itch_ingest, Release, -O3 -march=native)"

if [[ ${SKIP_BUILD} -eq 1 ]]; then
    warn "--skip-build passed -- skipping CMake configure/build step"
elif [[ ${FAILS} -gt 0 ]]; then
    warn "skipping build: earlier checks failed"
else
    echo "  Build dir: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"

    if cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-O3 -march=native" \
        > "${BUILD_DIR}.configure.log" 2>&1; then
        pass "CMake configure succeeded (log: ${BUILD_DIR}.configure.log)"
    else
        fail "CMake configure failed -- see ${BUILD_DIR}.configure.log"
    fi

    if [[ ${FAILS} -eq 0 ]]; then
        if cmake --build "${BUILD_DIR}" --target bench_itch_ingest --config Release \
            > "${BUILD_DIR}.build.log" 2>&1; then
            pass "bench_itch_ingest built successfully (log: ${BUILD_DIR}.build.log)"

            BENCH_BIN="${BUILD_DIR}/bin/bench_itch_ingest"
            [[ -x "${BENCH_BIN}" ]] || BENCH_BIN="${BUILD_DIR}/bin/Release/bench_itch_ingest"
            if [[ -x "${BENCH_BIN}" ]]; then
                pass "binary present: ${BENCH_BIN}"
            else
                fail "build reported success but bench_itch_ingest binary was not found under ${BUILD_DIR}/bin"
            fi
        else
            fail "bench_itch_ingest build failed -- see ${BUILD_DIR}.build.log"
        fi
    fi
fi

# --- Step 5: core isolation sanity check -------------------------------------
banner "Core isolation sanity check"

ISOLATION_TOOL=""
if command -v taskset >/dev/null 2>&1; then
    ISOLATION_TOOL="taskset"
elif command -v numactl >/dev/null 2>&1; then
    ISOLATION_TOOL="numactl"
fi

if [[ -n "${ISOLATION_TOOL}" ]]; then
    pass "${ISOLATION_TOOL} available for CPU pinning"
else
    warn "neither taskset nor numactl found -- benchmark will run without core isolation"
fi

if [[ -z "${ISOLATION_TOOL}" ]]; then
    echo
    echo "  ${C_YLW}WARNING: running bench_itch_ingest without core isolation (taskset/numactl)${C_RST}"
    echo "  ${C_YLW}         can introduce scheduler noise and inflate tail-latency figures.${C_RST}"
fi

# --- Summary ------------------------------------------------------------------
banner "Summary"

echo "  Failures : ${FAILS}"
echo "  Warnings : ${WARNINGS}"
echo

if [[ ${FAILS} -gt 0 ]]; then
    echo "${C_RED}${C_BLD}VERIFICATION FAILED${C_RST} -- ${FAILS} check(s) did not pass."
    exit 1
else
    echo "${C_GRN}${C_BLD}VERIFICATION PASSED${C_RST}$([[ ${WARNINGS} -gt 0 ]] && echo " with ${WARNINGS} warning(s)")."
    exit 0
fi
