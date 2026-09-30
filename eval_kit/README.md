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

- **Transport**: a single-writer, lock-free broadcast ring
  (`BroadcastRing`) living entirely inside a named POSIX shared-memory
  segment under `/dev/shm`. Two independent OS processes -- the bundled
  C++ producer binary and your Python consumer -- exchange fixed-layout records with
  no serialization step and no kernel round trip once both sides have
  mapped the segment.
- **Cache-line isolation**: the producer's single write cursor lives on
  its own `alignas(64)` cache line, apart from the read-only descriptor.
  Consumers never write shared memory at all -- no tail cursor, no CAS,
  no reader count -- so a consumer polling can never invalidate the line
  the producer is publishing on, and any number of consumers can attach
  without the producer knowing or paying for them.
- **Non-blocking overwrite mode**: the producer's default mode never
  waits on a slow or absent consumer. When the ring is full, it simply
  overwrites the oldest slot instead of blocking -- the producer's
  throughput is never gated by consumer speed. Each consumer keeps its own
  read cursor in its own process memory; one that falls more than
  `capacity - 1` records behind is snapped forward, and the records it
  skipped are counted in *its own* drop counter. Only the newest
  `capacity - 1` records are ever readable.
- **Zero-copy Python consumer**: the bundled `_animus_shm_native`
  extension (nanobind) binds the same ring directly (`BroadcastRing`) --
  no ctypes marshalling, no per-record Python object construction.
  `poll()` is non-blocking and hands back a zero-copy view of whatever
  batch is available; an empty view means the consumer has caught up.
- **Backpressure mode** (`--mode backpressure`): a separate, lossless
  mode on the older `ShmRing` layout, for measuring zero-loss end-to-end
  throughput against a consumer that keeps up. It is read by the
  source repo's `benchmarks/consumer.py`, which this kit does not bundle;
  `verify_stream.py` reads overwrite-mode (`BroadcastRing`) segments only.

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
whether or not a consumer is attached, leaving the segment (and the newest
`capacity - 1` records) behind for the next step. To watch a consumer
drain the stream *while* the producer is still running, start step 3 in a
second terminal before it finishes -- use a larger `--events` count so
there is time to attach. (`--mode backpressure` is not an option here:
`verify_stream.py` cannot read that mode's segment -- see the Architecture
Overview.)

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
- **Dropped packet counter**: the producer cannot know what any consumer
  missed, so it reports a writer-side figure -- how many of the events are
  no longer readable once the run ends (everything beyond the newest
  `capacity - 1`). `verify_stream.py` reports its own, reader-side
  `Dropped records (reader)`: the records *this* attach missed, including
  any published before it attached. Both are expected, not errors.
  `verify_stream.py` independently counts sequence gaps in what it
  actually received and cross-checks that figure against its own counter
  (`Gaps == dropped_count?` in its summary table); a mismatch there,
  not a nonzero drop count by itself, would indicate a real problem.

## Troubleshooting & Edge Cases

**`error: creating the '<name>' segment failed` / permission denied on `/dev/shm`.**
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
taskset -c 2 ./bin/harness_benchmark --events 100000000 --mode overwrite &
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
