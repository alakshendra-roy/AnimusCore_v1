# Animus Evaluation Kit -- Quickstart

Turnkey, self-contained benchmark package for institutional evaluation of
Animus's cross-process shared-memory execution telemetry engine. No build
step, no dependency resolution, no root/admin privileges required --
extract the tarball and you are producing and consuming ticks over real
POSIX shared memory in under three minutes.

If you'd rather run one command than follow the steps below:

```bash
./run_demo.sh
```

does the entire Quickstart automatically and prints a pass/fail verdict.

---

## Architecture Overview

- **Transport**: a single-producer/single-consumer, lock-free ring buffer
  living entirely inside a named POSIX shared-memory segment under
  `/dev/shm`. Two independent OS processes -- the bundled C++ producer
  binary and your Python consumer -- exchange fixed-layout records with
  no serialization step and no kernel round trip once both sides have
  mapped the segment.
- **Cache-line isolation**: the ring's head cursor (producer-owned) and
  tail cursor (consumer-owned) each live on their own `alignas(64)`
  cache line, so the producer publishing a new head never invalidates
  the cache line the consumer is polling the tail from, and vice versa
  -- the false-sharing elimination that makes a lock-free SPSC ring
  actually fast across two different cores (or two different sockets).
- **Non-blocking overwrite mode**: the producer's default mode never
  waits on a slow or absent consumer. When the ring is full, it reclaims
  the oldest unconsumed slot instead of blocking, and increments a
  deterministic drop counter -- the producer's throughput is never
  gated by consumer speed. A bounded-backpressure mode is also available
  (`--mode backpressure`) for measuring true zero-loss, end-to-end
  throughput against a consumer that is actually keeping up.
- **Zero-copy Python consumer**: the bundled `_animus_shm_native`
  extension (nanobind) binds the same ring directly -- no ctypes
  marshalling, no per-record Python object construction. Its spin-wait
  for new records runs with Python's GIL released, so a lagging producer
  never blocks anything else your interpreter is doing; the GIL is
  reacquired only to hand back a zero-copy view of whatever batch
  arrived.

## Pre-requisites

| Requirement | Notes |
|---|---|
| Linux, x86_64 | This kit's `bin/harness_benchmark` is a Linux ELF binary. |
| Kernel 5.4+ | Any mainstream distro from the last several years. Older kernels likely work too; not tested against them. |
| Python 3.10+ | Check `python3 --version`. The exact interpreter this kit's wheels were built against is recorded in `MANIFEST.txt` -- see Troubleshooting if `pip install` rejects a wheel. |
| glibc | Standard on virtually every non-musl Linux distro (Ubuntu, Debian, RHEL/CentOS/Rocky, Amazon Linux 2+, ...). Alpine/musl is not a supported target. |
| `/dev/shm` writable | Standard on any normal Linux install; some hardened containers restrict or omit it -- see Troubleshooting. |

No root privileges, no compiler, no `cmake`, no internet access required
on the evaluation machine itself.

## 3-Minute Quickstart

**1. Set up an isolated virtual environment and install the bundled wheels.**

```bash
python3 -m venv venv
source venv/bin/activate
pip install wheels/animus_engine_sdk-*.whl
pip install wheels/animus_native_stream-*.whl
```

**2. Start the C++ producer.**

```bash
./bin/harness_benchmark --events 10000000 --mode overwrite
```

This injects 10,000,000 synthetic execution events into a new shared-memory
ring and reports its own enqueue-latency percentiles and throughput.
Overwrite mode is self-contained -- it completes and exits on its own,
whether or not a consumer is attached, leaving the segment (and whatever
fits in the ring's capacity) behind for the next step. To watch the
producer being kept honest by a live, draining consumer instead, run it
with `--mode backpressure` in one terminal and start step 3 in a second
terminal before it finishes.

**3. Start the Python consumer.**

```bash
python3 scripts/verify_stream.py
```

Attaches to the segment the producer created, drains it, and prints live
throughput followed by a summary table.

**4. Interpreting the output.**

- **Throughput** (ticks/sec): sustained consumption rate from the
  Python side. Compare against the producer's own reported throughput to
  see how much headroom the transport has versus your consumption loop.
- **Latency percentiles** (p50 / p90 / p99 / p99.9): the producer reports
  its own *enqueue* latency (RDTSC-timestamped, calibrated against wall
  clock at startup); `verify_stream.py` separately reports *consumer-side
  inter-arrival* latency (this process's own `CLOCK_MONOTONIC`-based
  clock). These are deliberately not merged into one number -- they
  measure different things, on different clocks, in different processes,
  and conflating them would silently misstate what's actually being
  measured. Tail figures (p99.9, max) reflect OS scheduling noise on
  whatever core each process landed on as much as the transport itself;
  pin both processes (see Troubleshooting) for a tighter tail.
- **Dropped packet counter**: under `--mode overwrite`, `dropped_count`
  is the number of records the producer reclaimed before any consumer
  read them -- this is expected, not an error. `verify_stream.py`
  independently counts sequence gaps in what it actually received and
  cross-checks that figure against the producer's own counter
  (`Gaps == dropped_count?` in its summary table); a mismatch there,
  not a nonzero drop count by itself, would indicate a real problem.

## Troubleshooting & Edge Cases

**`ShmRing::create(...) failed` / permission denied on `/dev/shm`.**
Some hardened containers (certain Docker/Kubernetes security profiles,
some CI runners) mount `/dev/shm` read-only, too small, or not at all.
Check with `df -h /dev/shm`; if it's missing or tiny, this kit needs a
host or container configured with a normal, writable `/dev/shm` (Docker:
`--shm-size=64m` or larger; Kubernetes: an `emptyDir` medium `Memory`
volume mounted at `/dev/shm`).

**A previous run's segment is still present ("segment with this name
already exists").** The producer does not delete its segment on exit by
default (so a consumer started slightly late can still attach to it).
Clean it up directly -- POSIX shared-memory segments are ordinary files
under `/dev/shm`:

```bash
rm -f /dev/shm/animus_harness_shm   # or whatever --name you passed
```

**`pip install wheels/animus_native_stream-*.whl` fails with a
"no matching distribution" / platform tag error.** This wheel ships a
compiled extension, not pure Python -- its filename encodes the exact
Python version and platform it was built for (see `MANIFEST.txt`). Use
the `python3` that matches, or request a kit rebuilt against yours.
`wheels/animus_engine_sdk-*.whl` (the pure-Python SDK) is unaffected and
installs anywhere Python 3.8+ runs.

**CPU core affinity pinning.** For the tightest, least noisy latency
tail, pin the producer and consumer to separate, isolated cores (ideally
on the same NUMA node, on separate physical cores -- not two hyperthread
siblings of the same core):

```bash
taskset -c 2 ./bin/harness_benchmark --events 10000000 --mode backpressure &
taskset -c 3 python3 scripts/verify_stream.py
```

`harness_benchmark` also accepts `--core N` to pin itself internally
without `taskset`; pin the consumer externally either way, since
`verify_stream.py` has no such flag of its own.

## License Notice

Access to this evaluation kit and the Software it contains is granted
solely under Animus's institutional, non-production evaluation license --
see [https://animusinfra.com/terms](https://animusinfra.com/terms) for
the full scope, including the license's zero-liability terms for any
trading or execution outcome and its no-sale-of-telemetry-data
commitment. Production use requires a separate, executed commercial
agreement.
