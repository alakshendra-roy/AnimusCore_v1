#!/usr/bin/env bash
# Animus Evaluation Kit -- one-command turnkey demo.
#
# Runs exactly the 3-Minute Quickstart from README.md, end to end, with no
# manual steps: creates an isolated venv, installs the two bundled wheels,
# runs the C++ producer to completion in decoupled-overwrite mode (self-
# contained -- see README's Architecture Overview), then runs the nanobind
# Python consumer against whatever survived in the ring, and reports a
# single PASS/FAIL verdict. Safe to re-run; each run uses a fresh,
# PID-suffixed segment name and cleans up after itself.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

EVENTS="${ANIMUS_EVAL_EVENTS:-10000000}"
SEGMENT_NAME="animus_eval_demo_$$"
VENV_DIR="$SCRIPT_DIR/venv"
PRODUCER_JSON="$SCRIPT_DIR/producer_report.json"

echo "=== Animus Evaluation Kit -- Turnkey Demo ==="
echo

# --- Pre-flight checks -------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 not found on PATH. See README.md's Pre-requisites section." >&2
    exit 1
fi
PYVER="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
echo "python3: $(command -v python3) (version $PYVER)"

if [[ ! -d /dev/shm ]]; then
    echo "error: /dev/shm not found -- this kit requires POSIX shared memory." >&2
    echo "See README.md's Troubleshooting section." >&2
    exit 1
fi

if [[ ! -x bin/harness_benchmark ]]; then
    echo "error: bin/harness_benchmark not found or not executable." >&2
    echo "Run this script from inside the extracted eval kit directory." >&2
    exit 1
fi

# --- Cleanup: fires on normal exit, Ctrl+C, or an error under set -e ---
cleanup() {
    local status=$?
    rm -f "/dev/shm/${SEGMENT_NAME}"
    exit "$status"
}
trap cleanup EXIT INT TERM

# --- Isolated venv + bundled wheels -------------------------------------
if [[ ! -d "$VENV_DIR" ]]; then
    echo "Creating virtual environment at ./venv ..."
    python3 -m venv "$VENV_DIR"
fi
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"

echo "Installing bundled wheels ..."
pip install --quiet --upgrade pip
pip install --quiet wheels/animus_engine_sdk-*.whl
if ! pip install --quiet wheels/animus_native_stream-*.whl; then
    echo
    echo "error: failed to install the compiled nanobind extension wheel." >&2
    echo "This usually means python3 ($PYVER) doesn't match the Python this kit" >&2
    echo "was built against -- check MANIFEST.txt for the exact build version," >&2
    echo "and see README.md's Troubleshooting section." >&2
    exit 1
fi
chmod +x bin/harness_benchmark

# --- Step 1: C++ producer (decoupled overwrite mode, self-contained) ---
echo
echo "--- Starting producer: ./bin/harness_benchmark --name $SEGMENT_NAME --events $EVENTS --mode overwrite ---"
./bin/harness_benchmark --name "$SEGMENT_NAME" --events "$EVENTS" --mode overwrite --json "$PRODUCER_JSON"

# --- Step 2: nanobind Python consumer -----------------------------------
# The producer has already finished (overwrite mode never blocks on a
# consumer -- see README), so whatever fits in the ring's capacity is
# still there to drain; the rest is accounted for by dropped_count, which
# verify_stream.py cross-checks against the sequence gaps it observes.
echo
echo "--- Starting consumer: python3 scripts/verify_stream.py --name $SEGMENT_NAME ---"
set +e
python3 scripts/verify_stream.py --name "$SEGMENT_NAME" --events "$EVENTS" --idle-timeout-s 3
CONSUMER_STATUS=$?
set -e

echo
if [[ $CONSUMER_STATUS -eq 0 ]]; then
    echo "=== DEMO PASSED -- data integrity OK; see the tables above for throughput/latency ==="
else
    echo "=== DEMO FAILED (verify_stream.py exit code $CONSUMER_STATUS) -- see output above ===" >&2
fi
exit "$CONSUMER_STATUS"
