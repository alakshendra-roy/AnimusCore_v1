#!/usr/bin/env bash
# animus_sandbox/run_benchmark.sh
#
# Single-command entry point for this directory: checks CPU topology,
# builds benchmark_harness + soak_harness with -O3 -march=native -pthread,
# runs them (pinned via taskset when available), and prints a summary
# table built directly from each program's own stdout -- nothing in this
# script's summary is a number it invented; every figure comes from the
# harness that measured it, on whatever machine this script is run on.
#
# Usage:
#   ./run_benchmark.sh                  # C++ only (no external deps), ~5-10s
#   ./run_benchmark.sh --with-python     # also builds/runs the optional
#                                        # nanobind bridge (needs Python
#                                        # 3.8+ with dev headers + `pip
#                                        # install nanobind` already done)
#   ./run_benchmark.sh --events 5000000 # override the soak event count
#                                        # (default 10,000,000)
#
# Exit code is non-zero if either harness's own correctness check fails --
# see benchmark_harness.cpp / soak_harness.cpp for what each check covers.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"

WITH_PYTHON=0
EVENTS=10000000
while [ $# -gt 0 ]; do
    case "$1" in
        --with-python) WITH_PYTHON=1 ;;
        --events) EVENTS="$2"; shift ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
    shift
done

CXX="${CXX:-g++}"
CXXFLAGS="-O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic -pthread"
RULE="---------------------------------------------------------------------"

section() { echo; echo "$RULE"; echo " $1"; echo "$RULE"; }

# --- json_field <json_line> <field_name> -----------------------------------
# Zero-dependency extraction from this directory's own flat, single-line
# JSON logs (no jq, no python needed just to read a number back out).
json_field() {
    printf '%s' "$1" | grep -o "\"$2\":[^,}]*" | head -n1 | cut -d: -f2 | tr -d '"'
}

section "1. CPU topology & isolation checks"

LOGICAL_CPUS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
echo "logical CPUs: $LOGICAL_CPUS"

if [ -r /proc/cmdline ]; then
    ISOLCPUS="$(grep -o 'isolcpus=[^ ]*' /proc/cmdline || true)"
    if [ -n "$ISOLCPUS" ]; then
        echo "kernel boot param: $ISOLCPUS (producer/consumer cores reserved outside the scheduler)"
    else
        echo "kernel boot param: no isolcpus= set -- this run shares cores with the rest of the OS scheduler."
        echo "  For tail-latency numbers that hold under production load, add isolcpus=/nohz_full= for the"
        echo "  cores you pin below and re-run. Not required to get a number, just to trust its tail."
    fi
else
    echo "kernel boot param: /proc/cmdline not readable on this OS -- skipping isolcpus check."
fi

GOV_FILES=(/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor)
if [ -e "${GOV_FILES[0]}" ]; then
    GOVERNORS="$(cat "${GOV_FILES[@]}" 2>/dev/null | sort -u | tr '\n' ',' | sed 's/,$//')"
    echo "cpufreq governor(s) in use: $GOVERNORS"
    case "$GOVERNORS" in
        performance) ;;
        *) echo "  Not pinned to 'performance' on every core -- frequency scaling can inflate tail latency." \
                " sudo cmd: for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance | sudo tee \$g; done" ;;
    esac
else
    echo "cpufreq governor: no cpufreq sysfs on this machine (common in a VM/container) -- skipping."
fi

TASKSET_BIN="$(command -v taskset || true)"
PIN_CORES=""
if [ -n "$TASKSET_BIN" ] && [ "$LOGICAL_CPUS" != "unknown" ] && [ "$LOGICAL_CPUS" -ge 4 ] 2>/dev/null; then
    PIN_CORES="0-3"
    echo "taskset found: pinning this run to logical CPUs $PIN_CORES"
else
    echo "taskset not available (or fewer than 4 logical CPUs) -- running unpinned."
    echo "  Every number below is still valid, just noisier in the tail from scheduler migration."
fi
RUN_PREFIX=""
[ -n "$PIN_CORES" ] && RUN_PREFIX="$TASKSET_BIN -c $PIN_CORES"

section "2. Build (${CXX} ${CXXFLAGS})"

BUILD_T0=$(date +%s)
"$CXX" $CXXFLAGS benchmark_harness.cpp -o benchmark_harness || { echo "benchmark_harness build FAILED"; exit 1; }
"$CXX" $CXXFLAGS soak_harness.cpp -o soak_harness || { echo "soak_harness build FAILED"; exit 1; }
BUILD_T1=$(date +%s)
echo "built benchmark_harness + soak_harness in $((BUILD_T1 - BUILD_T0))s"

section "3. benchmark_harness -- false sharing / MPMC throughput / TSC cost"

BENCH_LOG="$(mktemp)"
$RUN_PREFIX ./benchmark_harness | tee "$BENCH_LOG"
BENCH_STATUS=$?

section "4. soak_harness -- ${EVENTS} event soak, tick-to-telemetry percentiles"

echo "producers=8 consumers=8, duration cap 60s (safety net), stops at ~${EVENTS} events"
SOAK_LOG="$(mktemp)"
$RUN_PREFIX ./soak_harness 60 2 8 8 "$EVENTS" | tee "$SOAK_LOG"
SOAK_STATUS=$?
SOAK_FINAL_LINE="$(grep '^SOAK_FINAL' "$SOAK_LOG" | sed 's/^SOAK_FINAL //')"

section "5. Summary"

if [ -n "$SOAK_FINAL_LINE" ]; then
    P50_NS="$(json_field "$SOAK_FINAL_LINE" final_window_p50_ns)"
    P50_CYC="$(json_field "$SOAK_FINAL_LINE" final_window_p50_cyc)"
    P90_NS="$(json_field "$SOAK_FINAL_LINE" final_window_p90_ns)"
    P90_CYC="$(json_field "$SOAK_FINAL_LINE" final_window_p90_cyc)"
    P99_NS="$(json_field "$SOAK_FINAL_LINE" final_window_p99_ns)"
    P99_CYC="$(json_field "$SOAK_FINAL_LINE" final_window_p99_cyc)"
    P999_NS="$(json_field "$SOAK_FINAL_LINE" final_window_p999_ns)"
    P999_CYC="$(json_field "$SOAK_FINAL_LINE" final_window_p999_cyc)"
    P999_EXACT="$(json_field "$SOAK_FINAL_LINE" final_window_p999_exact)"
    HEAP_ALLOCS="$(json_field "$SOAK_FINAL_LINE" heap_allocs_during_hot_path)"
    CORRECT="$(json_field "$SOAK_FINAL_LINE" correctness_ok)"
    PRODUCED="$(json_field "$SOAK_FINAL_LINE" total_produced)"
    ELAPSED="$(json_field "$SOAK_FINAL_LINE" total_elapsed_s)"

    printf "%-14s %14s %16s\n" "Percentile" "Latency (ns)" "Latency (cycles)"
    printf "%-14s %14s %16s\n" "p50"  "$P50_NS"  "$P50_CYC"
    printf "%-14s %14s %16s\n" "p90"  "$P90_NS"  "$P90_CYC"
    printf "%-14s %14s %16s\n" "p99"  "$P99_NS"  "$P99_CYC"
    printf "%-14s %14s %16s%s\n" "p99.9" "$P999_NS" "$P999_CYC" \
        "$([ "$P999_EXACT" = "false" ] && echo '  (histogram bucket floor, not exact -- see soak_harness.cpp)')"
    echo
    echo "events: $PRODUCED produced in ${ELAPSED}s"
    echo "zero heap allocations on the hot path: $([ "$HEAP_ALLOCS" = "0" ] && echo PASS || echo "FAIL ($HEAP_ALLOCS allocs)")"
    echo "produced == consumed (correctness): $([ "$CORRECT" = "true" ] && echo PASS || echo FAIL)"
else
    echo "could not find a SOAK_FINAL line in soak_harness's output -- see $SOAK_LOG"
fi

echo
echo "These numbers are specific to THIS machine, THIS moment, and (unless you passed a pinned"
echo "core range via taskset above) THIS machine's current background load -- re-run before citing"
echo "them anywhere. Raw logs: $BENCH_LOG (benchmark_harness), $SOAK_LOG (soak_harness)."

if [ "$WITH_PYTHON" -eq 1 ]; then
    section "6. Optional: nanobind zero-copy Python bridge"
    if ! command -v cmake >/dev/null 2>&1; then
        echo "cmake not found -- skipping. Install CMake to build animus_sandbox_bridge."
    elif ! python3 -c "import nanobind" >/dev/null 2>&1; then
        echo "nanobind not importable from python3 -- skipping. Install with: pip install nanobind"
    else
        cmake -B build -DCMAKE_BUILD_TYPE=Release >/tmp/animus_sandbox_cmake.log 2>&1 \
            && cmake --build build --config Release >>/tmp/animus_sandbox_cmake.log 2>&1 \
            && cp build/animus_sandbox_bridge*.so . 2>/dev/null
        if [ -f animus_sandbox_bridge*.so ] || ls animus_sandbox_bridge*.so >/dev/null 2>&1; then
            $RUN_PREFIX python3 python_bridge_test.py
        else
            echo "nanobind module build failed -- see /tmp/animus_sandbox_cmake.log"
        fi
    fi
else
    echo
    echo "Skipped the optional nanobind Python bridge (needs Python 3.8+ dev headers + 'pip install"
    echo "nanobind', not zero-dependency like the two C++ harnesses above). Run with --with-python to"
    echo "include it, or see README.md to run python_bridge_test.py by hand."
fi

[ "$BENCH_STATUS" -eq 0 ] && [ "$SOAK_STATUS" -eq 0 ]
exit $?
