# bench_mpmc -- Standalone MPMC Ring Buffer Evaluation

A single-file, dependency-free reproduction harness for evaluating three
specific, independently checkable claims about the lock-free MPMC ring
buffer design used throughout Animus Core:

1. **64-byte cache-line alignment (`alignas(64)`)** and the throughput it
   buys by eliminating false sharing -- measured with a real A/B test on
   plain atomic counters, not just a `static_assert`.
2. **Zero dynamic (heap) allocation anywhere in the timed hot path** --
   enforced, not just claimed. The harness overrides `operator new` /
   `operator delete` process-wide and aborts with a clear `FAIL` if either
   fires while the producer/consumer loops are running.
3. **Lock-free MPMC throughput** under real multi-producer/multi-consumer
   contention, gated on a hard-fail correctness check (every pushed event
   drained exactly once, with an exact checksum match). Throughput itself
   is reported as measured, not gated -- actual numbers depend on your
   CPU, core count, and current machine load, which is exactly why this
   harness exists: run it on your own hardware rather than trusting a
   number in a slide deck.

Nothing in this directory depends on the parent repository, and nothing
depends on any third-party library. `bench_mpmc.cpp` is the entire
evaluation kit -- one file, standard C++17, no headers to copy alongside
it.

## Build

Any standard C++17 compiler works:

```
g++     -std=c++17 -O3 -pthread bench_mpmc.cpp -o bench_mpmc
clang++ -std=c++17 -O3 -pthread bench_mpmc.cpp -o bench_mpmc
```

On Windows with MSVC:

```
cl /std:c++17 /O2 /EHsc bench_mpmc.cpp
```

No Makefile, no CMake, no build system required -- that's deliberate, so
this can be dropped into any evaluation environment and built in one
command with whatever compiler is already installed there.

## Run

```
./bench_mpmc
```

With no arguments, the harness uses `min(8, logical_cores / 2)` threads
per side (producers, then consumers), matching the parent repository's
own default benchmark convention. Override with an explicit thread count:

```
./bench_mpmc 4     # 4 producers, 4 consumers
```

## Reading the output

The harness prints two sections:

- **`[1/2]`** -- the false-sharing A/B test: two atomic counters sharing
  one cache line (`UnpaddedCounters`) versus two counters each on their
  own cache line (`PaddedCounters`, via `alignas(64)`), each incremented
  20M times from two contending threads. The speedup printed is what
  `alignas(64)` buys on your specific CPU.
- **`[2/2]`** -- the MPMC ring buffer run: pushes a fixed batch of events
  through the queue via N producer threads, then drains it via N consumer
  threads, reporting push/drain throughput, a correctness verdict, and
  the hot-path heap allocation count.

The last line is a single machine-parseable JSON record prefixed
`BENCH_MPMC_RESULT` (thread count, event count, elapsed times,
throughput, correctness, and hot-path allocation count) if you want to
pipe results into your own tooling rather than parse the human-readable
output above it.

The process exits non-zero if either the correctness check or the
zero-heap-allocation check fails -- both are hard gates, unlike the
throughput numbers, which are simply reported.

## What this harness intentionally does not claim

- No fixed throughput target is asserted as pass/fail. Reference numbers
  from any specific machine are not a promise for yours -- CPU
  generation, core count, NUMA topology, and current system load all
  move the number materially.
- Thread pinning (`pin_to_core`) is best-effort on Windows and Linux and
  silently no-ops if denied by the OS or environment (e.g. inside some
  containers or VMs). An unpinned run is still a valid measurement, just
  with more scheduler-migration jitter in the tail latency -- this
  matters more for per-event latency measurements than for the aggregate
  throughput numbers this harness reports.
- This harness measures the ring buffer in isolation. It does not
  exercise licensing, transport, clustering, or any of the other
  components in the full Animus Core engine -- it exists specifically to
  make the three claims above independently reproducible in a few
  minutes, on hardware you control.
