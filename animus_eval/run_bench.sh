#!/usr/bin/env bash
# Animus Evaluation Kit -- turnkey build + run.
#
# Configures, builds, and runs replay_bench in one command, with the whole
# process additionally confined to CPU cores 2 and 3 via taskset as an
# outer safety net -- replay_bench already pins its own producer/consumer
# threads internally via pthread_setaffinity_np (see src/engine.cpp), so
# taskset here isn't load-bearing for correctness, but it stops the OS
# scheduler from placing any *other* thread in this process on those same
# two cores during the run, which is one less source of noise on a
# multi-tenant box. For real isolation beyond that, see README.md's
# isolcpus/nohz_full and CPU governor notes.
#
# Usage:
#   ./run_bench.sh              # producer=core 2, consumer=core 3 (defaults)
#   ./run_bench.sh 4 5          # override: producer=core 4, consumer=core 5
#
# Exits non-zero if the build fails or replay_bench reports a non-zero
# (i.e. non-zero-loss) result -- see src/replay_bench.cpp's own exit code.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PRODUCER_CORE="${1:-2}"
CONSUMER_CORE="${2:-3}"

if ! command -v taskset >/dev/null 2>&1; then
    echo "warning: taskset not found (util-linux not installed?) -- running without the outer CPU mask." >&2
    echo "         replay_bench's own internal thread pinning (pthread_setaffinity_np) still applies." >&2
    TASKSET_PREFIX=()
else
    # Confine the whole process to exactly the two cores replay_bench will
    # internally pin its threads to -- see header comment above.
    TASKSET_PREFIX=(taskset -c "${PRODUCER_CORE},${CONSUMER_CORE}")
fi

echo "== Configuring (CMake, Release) =="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

echo "== Building =="
cmake --build build -j

echo "== Running (producer=core ${PRODUCER_CORE}, consumer=core ${CONSUMER_CORE}) =="
exec "${TASKSET_PREFIX[@]}" ./build/replay_bench "${PRODUCER_CORE}" "${CONSUMER_CORE}"
