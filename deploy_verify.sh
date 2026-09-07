#!/usr/bin/env bash
# Animus Core -- Turnkey Client Verification Script
#
# One command for an institutional client to clone this repository and
# reproduce every benchmark claim on their own hardware: builds
# animus_bench (the C++20 zero-allocation SPSC ingestion harness,
# benchmarks/animus_harness.cpp) in Release mode, runs a sustained pass and
# a burst pass, builds and exercises the Python SDK (sdk/python), verifies
# the zero-drop/zero-corruption/zero-hot-path-allocation guarantee both
# passes must report, and refreshes benchmarks/reports/ANIMUS_BENCHMARK_REPORT.html
# with this machine's own specs and this run's own numbers -- never a
# hand-typed or hardcoded figure (same discipline as
# benchmarks/generate_benchmark_report.py and
# scripts/generate_institutional_report.py, which this script drives).
#
# Supported environments: Ubuntu/Debian (apt), WSL2 running Ubuntu/Debian,
# and RHEL-family (dnf/yum: RHEL, CentOS Stream, Fedora, Rocky, Alma).
# macOS and native Windows are out of scope for this script -- see
# scripts/run_benchmarks.ps1 for a native-Windows benchmark path, and
# sdk/python/README.md for a manual Python SDK build on any platform.
#
# Usage:
#   ./deploy_verify.sh [--rate N] [--duration SECONDS] [--skip-install]
#                       [--build-dir DIR] [--venv-dir DIR]
#
#   --rate N          Sustained-pass target ingest rate, msgs/sec (default: 10000000)
#   --duration SEC    Duration of each benchmark pass, seconds (default: 8)
#   --skip-install    Assume dependencies are already present; never invoke
#                      the system package manager (use on a locked-down or
#                      already-provisioned machine)
#   --build-dir DIR   CMake build directory for animus_bench (default: build/deploy-verify)
#   --venv-dir DIR    Python virtualenv directory for the SDK build (default: .deploy_verify_venv)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}"

RATE=10000000
DURATION=8
SKIP_INSTALL=0
BUILD_DIR="${REPO_ROOT}/build/deploy-verify"
VENV_DIR="${REPO_ROOT}/.deploy_verify_venv"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rate) RATE="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --skip-install) SKIP_INSTALL=1; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --venv-dir) VENV_DIR="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown argument: $1 (--help for usage)" >&2; exit 2 ;;
    esac
done

banner() {
    echo
    echo "================================================================================"
    echo " $1"
    echo "================================================================================"
}

# --- Step 1: OS environment detection ---------------------------------------
banner "Step 1/6 -- Detecting OS environment"

OS_ID="unknown"
OS_FAMILY="unknown"   # debian | rhel | unknown
IS_WSL=0

if [[ -f /proc/version ]] && grep -qiE "microsoft|wsl" /proc/version 2>/dev/null; then
    IS_WSL=1
fi

if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    OS_ID="${ID:-unknown}"
    OS_ID_LIKE="${ID_LIKE:-}"
    if [[ "${OS_ID}" =~ ^(ubuntu|debian)$ ]] || [[ "${OS_ID_LIKE}" == *debian* ]]; then
        OS_FAMILY="debian"
    elif [[ "${OS_ID}" =~ ^(rhel|centos|fedora|rocky|almalinux)$ ]] || [[ "${OS_ID_LIKE}" == *rhel* ]] || [[ "${OS_ID_LIKE}" == *fedora* ]]; then
        OS_FAMILY="rhel"
    fi
fi

echo "  Distribution : ${OS_ID} (${PRETTY_NAME:-unknown})"
echo "  Family       : ${OS_FAMILY}"
echo "  WSL2         : $([[ ${IS_WSL} -eq 1 ]] && echo yes || echo no)"
echo "  Kernel       : $(uname -r)"
echo "  Architecture : $(uname -m)"

if [[ "${OS_FAMILY}" == "unknown" && "${SKIP_INSTALL}" -eq 0 ]]; then
    echo
    echo "warning: could not identify this distribution as Debian/Ubuntu or RHEL-family." >&2
    echo "         Skipping automatic dependency installation -- install cmake, ninja," >&2
    echo "         a C++20 compiler (gcc>=11 or clang>=13), and python3-dev yourself," >&2
    echo "         then re-run with --skip-install." >&2
    SKIP_INSTALL=1
fi

# --- Step 2: dependency verification / installation --------------------------
banner "Step 2/6 -- Verifying build dependencies"

need_cmd() { command -v "$1" >/dev/null 2>&1; }

MISSING=()
need_cmd cmake  || MISSING+=("cmake")
need_cmd g++    || need_cmd clang++ || MISSING+=("a C++ compiler (g++ or clang++)")
need_cmd python3 || MISSING+=("python3")

if [[ ${#MISSING[@]} -gt 0 && "${SKIP_INSTALL}" -eq 0 ]]; then
    echo "  Missing: ${MISSING[*]}"
    SUDO=""
    if [[ "$(id -u)" -ne 0 ]]; then
        need_cmd sudo && SUDO="sudo" || {
            echo "error: not running as root and 'sudo' is not available -- cannot install packages." >&2
            echo "       Install manually, or re-run as root, or pass --skip-install." >&2
            exit 1
        }
    fi

    if [[ "${OS_FAMILY}" == "debian" ]]; then
        echo "  Installing via apt-get (Debian/Ubuntu${IS_WSL:+/WSL2})..."
        ${SUDO} apt-get update -y
        ${SUDO} apt-get install -y cmake ninja-build build-essential python3-dev python3-venv python3-pip
    elif [[ "${OS_FAMILY}" == "rhel" ]]; then
        echo "  Installing via dnf/yum (RHEL-family)..."
        PKG_MGR="dnf"; need_cmd dnf || PKG_MGR="yum"
        ${SUDO} "${PKG_MGR}" install -y cmake ninja-build gcc-c++ python3-devel python3-pip
    else
        echo "error: dependencies missing (${MISSING[*]}) and no known package manager to install them." >&2
        exit 1
    fi
else
    echo "  All required build dependencies already present (or --skip-install requested)."
fi

for c in cmake python3; do
    need_cmd "${c}" || { echo "error: '${c}' still not found after dependency step." >&2; exit 1; }
done
need_cmd g++ || need_cmd clang++ || { echo "error: no C++ compiler found after dependency step." >&2; exit 1; }

echo "  cmake        : $(cmake --version | head -1)"
echo "  compiler     : $(g++ --version 2>/dev/null | head -1 || clang++ --version | head -1)"
echo "  python3      : $(python3 --version)"

# --- Step 3: configure and build animus_bench (Release, native arch) --------
banner "Step 3/6 -- Building animus_bench (Release, -O3, native architecture)"

GENERATOR_ARGS=()
need_cmd ninja && GENERATOR_ARGS=(-G Ninja)

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-march=native" \
    "${GENERATOR_ARGS[@]}"

cmake --build "${BUILD_DIR}" --target animus_bench --config Release -- ${MAKEFLAGS:-}

BENCH_BIN="${BUILD_DIR}/bin/animus_bench"
[[ -x "${BENCH_BIN}" ]] || BENCH_BIN="${BUILD_DIR}/bin/Release/animus_bench"
if [[ ! -x "${BENCH_BIN}" ]]; then
    echo "error: animus_bench binary not found after build (looked in ${BUILD_DIR}/bin and ${BUILD_DIR}/bin/Release)" >&2
    exit 1
fi
echo "  -> ${BENCH_BIN}"

# --- Step 4/5/6: run sustained + burst passes, verify guarantees, refresh report --
# Delegated to scripts/generate_institutional_report.py: it runs animus_bench
# itself (sustained pass at --rate/--duration, then a --burst pass of the
# same duration), parses the real stdout of both runs, gathers this
# machine's own CPU/cache/topology specs, and overwrites
# benchmarks/reports/ANIMUS_BENCHMARK_REPORT.html with all of it. It exits
# non-zero (and this script aborts, via `set -e`) if either pass's own
# printed VERIFIED line -- 0 dropped frames, 0 corrupted frames, 0
# hot-path heap allocations -- is missing, so this step covers steps 4, 6,
# and 7 of the institutional verification checklist in one reproducible call.
banner "Step 4/6 -- Sustained + burst passes, integrity verification, report refresh"

python3 "${SCRIPT_DIR}/scripts/generate_institutional_report.py" \
    --binary "${BENCH_BIN}" --rate "${RATE}" --duration "${DURATION}"

# --- Step 5: build the Python SDK -------------------------------------------
banner "Step 5/6 -- Building the Python SDK (sdk/python)"

python3 -m venv "${VENV_DIR}"
# shellcheck disable=SC1091
source "${VENV_DIR}/bin/activate"
pip install --upgrade pip >/dev/null
pip install nanobind numpy scikit-build-core >/dev/null
pip install "${REPO_ROOT}/sdk/python"

# --- Step 6: Python SDK throughput verification -----------------------------
banner "Step 6/6 -- Python SDK throughput verification"

python3 "${REPO_ROOT}/sdk/python/bench_python_throughput.py" --duration "${DURATION}"

deactivate

banner "DONE -- all verification steps completed"
echo "  Benchmark report : benchmarks/reports/ANIMUS_BENCHMARK_REPORT.html"
echo "  animus_bench      : ${BENCH_BIN}"
echo "  Python SDK venv   : ${VENV_DIR}"
echo
echo "Re-run any single step directly, e.g.:"
echo "  ${BENCH_BIN} --rate ${RATE} --duration ${DURATION}"
echo "  ${BENCH_BIN} --rate ${RATE} --duration ${DURATION} --burst"
echo "  ${VENV_DIR}/bin/python sdk/python/bench_python_throughput.py --duration ${DURATION}"
