#!/usr/bin/env bash
# Animus Engine -- deterministic one-command benchmark reproduction harness
# (Linux/macOS).
#
# Builds benchmarks/harness_benchmark.cpp in Release configuration via the
# root CMakeLists.txt's `harness_benchmark` target (the same binary
# BENCHMARKS.md's Phase 29 verification exercises -- a real, two-process-
# capable animus::sys::ipc::ShmRing<ExecutionEvent> producer, see
# include/animus/shm_ipc.hpp), runs the standard throughput/latency pass,
# and samples that ring's telemetry header while the producer is still
# writing to it -- once via scripts/animus_stat.py's live-dashboard row
# (--once) and once via its OpenMetrics/Prometheus exposition (--prometheus)
# -- before tearing the segment down. See ARCHITECTURE.md sections 1, 2,
# and 4 for what these numbers mean and why the telemetry sampler never
# perturbs the producer it's reading.
#
# The producer is launched in the background and its shared-memory segment
# is polled for (bounded retries, not a fixed sleep) rather than sampled
# after it exits: on POSIX the segment happens to outlive the process
# (shm_unlink() is never called implicitly), but relying on that would
# make this script's Windows counterpart (run_benchmarks.ps1) behave
# differently by construction -- named file mappings there are destroyed
# the instant the creating process's last handle closes (ARCHITECTURE.md
# section 1.1). Sampling concurrently, while the producer is still
# attached, is the one approach that is correct on both platforms and is
# also the honest demonstration of this system's actual out-of-band
# telemetry contract (section 4.2): a live monitor reading a ring a real
# producer is still writing to, not a post-mortem log read.
#
# Usage: ./scripts/run_benchmarks.sh [--events N] [--capacity SLOTS] [--build-dir DIR]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

EVENTS=10000000
CAPACITY=1048576
BUILD_DIR="${REPO_ROOT}/build/bench-release"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --events) EVENTS="$2"; shift 2 ;;
        --capacity) CAPACITY="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --help)
            echo "usage: $0 [--events N] [--capacity SLOTS] [--build-dir DIR]"
            echo "  --events      synthetic events to push (default: 10000000)"
            echo "  --capacity    ring capacity in slots (default: 1048576)"
            echo "  --build-dir   CMake build directory to use (default: build/bench-release)"
            exit 0 ;;
        *) echo "unknown argument: $1 (--help for usage)" >&2; exit 2 ;;
    esac
done

RING_NAME="animus_bench_$$"

echo "=============================================================="
echo " Animus Engine -- Benchmark Reproduction Harness"
echo "=============================================================="
echo "Platform:    $(uname -s) $(uname -m)"
echo "Repo root:   ${REPO_ROOT}"
echo "Build dir:   ${BUILD_DIR}"
echo "Events:      ${EVENTS}"
echo "Capacity:    ${CAPACITY} slots"
echo "Ring name:   ${RING_NAME}"
echo

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found on PATH -- required to build harness_benchmark" >&2
    exit 1
fi

# --- 1. Verify/trigger the release build ---------------------------------
# Always re-invoke configure+build: both are incremental (Ninja/Make/MSBuild
# each skip recompiling anything unchanged), so this is the "verify or
# trigger" step the underlying build system already implements correctly --
# no hand-rolled staleness check needed on top of it.
GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

echo "--- Configuring (CMake, Release) ---"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${GENERATOR_ARGS[@]}"

echo
echo "--- Building harness_benchmark (Release) ---"
cmake --build "${BUILD_DIR}" --target harness_benchmark --config Release

BIN="${BUILD_DIR}/harness_benchmark"
[[ -x "${BIN}" ]] || BIN="${BUILD_DIR}/Release/harness_benchmark"
if [[ ! -x "${BIN}" ]]; then
    echo "error: harness_benchmark binary not found after build (looked in" \
         "${BUILD_DIR} and ${BUILD_DIR}/Release)" >&2
    exit 1
fi
echo "-> ${BIN}"
echo

# --- 2. Launch the standard throughput/latency pass (background) --------
echo "=============================================================="
echo " Throughput / Latency Pass (decoupled overwrite, ${EVENTS} events)"
echo "=============================================================="
JSON_OUT="${BUILD_DIR}/harness_benchmark_result.json"
LOG_FILE="${BUILD_DIR}/harness_benchmark_stdout.log"

"${BIN}" --name "${RING_NAME}" --events "${EVENTS}" --capacity "${CAPACITY}" \
    --mode overwrite --json "${JSON_OUT}" > "${LOG_FILE}" 2>&1 &
PRODUCER_PID=$!

# --- 3. Poll for the live segment, sample telemetry, and unlink it -------
# Delegates to run_benchmark_telemetry.py (a single, long-lived Python
# process) rather than polling from the shell: see that script's own header
# comment for why holding one open handle for the whole sampling window is
# what actually makes this race-free on Windows, not just the poll interval.
PY=""
if command -v python3 >/dev/null 2>&1; then PY=python3
elif command -v python >/dev/null 2>&1; then PY=python
fi

if [[ -n "${PY}" ]]; then
    echo
    echo "=============================================================="
    echo " Telemetry Snapshot (scripts/animus_stat.py, ring='${RING_NAME}')"
    echo "=============================================================="
    "${PY}" "${SCRIPT_DIR}/run_benchmark_telemetry.py" "${RING_NAME}" "${SCRIPT_DIR}/animus_stat.py" || true
    echo
else
    echo "warning: no python3/python on PATH -- skipping telemetry snapshot" \
         "(scripts/animus_stat.py)" >&2
fi

# --- 4. Wait for the producer to finish, then show its own report --------
wait "${PRODUCER_PID}"
echo "=============================================================="
echo " Producer Output"
echo "=============================================================="
cat "${LOG_FILE}"
echo
echo "=============================================================="
echo " Done. Full JSON report: ${JSON_OUT}"
echo "=============================================================="
