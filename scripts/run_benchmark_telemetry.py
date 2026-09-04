#!/usr/bin/env python3
"""Telemetry-sampling helper for scripts/run_benchmarks.{sh,ps1}.

Shared by both the bash and PowerShell benchmark-reproduction launchers so
the actual sampling logic exists once, not as two hand-maintained inline
copies that could drift.

Problem this solves: a Windows named file mapping is destroyed the instant
its last OS handle closes (ARCHITECTURE.md section 1.1) -- unlike POSIX,
where a /dev/shm node outlives the process until explicitly shm_unlink()'d.
harness_benchmark.cpp's own producer (Milestone 4, benchmarks/harness_
benchmark.cpp) never calls unlink() by default, but on a fast run (a few
hundred ms at small event counts, well under a second even at the default
10,000,000) it can still exit -- and on Windows, tear the segment down --
before a *separately spawned* `animus_stat.py --once` process has even
finished interpreter startup. Polling for the segment from a short-lived
process doesn't fix this: the gap between "the poll process closed its own
handle" and "the next process opens one" is exactly where the race is lost.

The fix: this script opens (and DOES NOT close) its own handle on the
segment for the entire duration of sampling. That handle alone is enough
to keep the underlying OS object alive on Windows regardless of whether
harness_benchmark's own process has already exited, and is harmless on
POSIX (which does not need it, but is not bothered by it either). Once
sampling is done, this script closes its handle and unlinks the segment
itself -- POSIX: a real shm_unlink(); Windows: a documented no-op, since
closing this script's own handle is what actually releases the object
there, immediately once no other handle (there never was one besides the
already-exited producer's) remains.

Usage: run_benchmark_telemetry.py <ring_name> <path/to/animus_stat.py>
Exit code: 0 if the segment was found and sampled; 1 on timeout.
"""
import subprocess
import sys
import time
from multiprocessing import shared_memory

POLL_ATTEMPTS = 150
POLL_INTERVAL_SECONDS = 0.02


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: run_benchmark_telemetry.py <ring_name> <path/to/animus_stat.py>", file=sys.stderr)
        return 2
    ring_name, animus_stat_path = sys.argv[1], sys.argv[2]

    segment = None
    for _ in range(POLL_ATTEMPTS):
        try:
            segment = shared_memory.SharedMemory(name=ring_name, create=False)
            break
        except FileNotFoundError:
            time.sleep(POLL_INTERVAL_SECONDS)

    if segment is None:
        print(
            f"warning: timed out waiting for shared-memory segment '{ring_name}' to appear -- "
            "skipping live telemetry sample (producer may have finished before it could be "
            "sampled on an unusually slow/loaded machine)",
            file=sys.stderr,
        )
        return 1

    try:
        # flush=True on every print here: this process's own stdout is
        # block-buffered when piped/redirected (not a TTY), but each
        # subprocess.run() below writes directly to the same inherited
        # stream and flushes independently -- without an explicit flush,
        # these header lines can be reordered after the subprocess output
        # they're meant to introduce.
        print("--- Live snapshot (--once) ---", flush=True)
        subprocess.run([sys.executable, animus_stat_path, "--name", ring_name, "--once"], check=False)
        print(flush=True)
        print("--- Prometheus / OpenMetrics (--prometheus) ---", flush=True)
        subprocess.run([sys.executable, animus_stat_path, "--name", ring_name, "--prometheus"], check=False)
        return 0
    finally:
        # Release our own hold first -- on POSIX this alone does not destroy
        # the segment (that's what unlink() below is for); on Windows it may
        # already be the last handle in the system, in which case the OS
        # destroys the object right here, making the unlink() call below the
        # documented no-op ARCHITECTURE.md section 1.1 describes.
        segment.close()
        try:
            segment.unlink()
            print(f"Segment '{ring_name}' unlinked.", flush=True)
        except FileNotFoundError:
            print(f"Segment '{ring_name}' already gone.", flush=True)


if __name__ == "__main__":
    sys.exit(main())
