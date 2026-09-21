# Animus Core -- Standalone Evaluation Sandbox

A self-contained, dependency-free reproduction package for three specific,
independently checkable claims:

1. **64-byte cache-line alignment (`alignas(64)`)** and the throughput
   this buys by eliminating false sharing -- measured with a real A/B
   test, not just a `static_assert`.
2. **Lock-free MPMC ring buffer throughput** under real 8-producer /
   8-consumer contention, reported against a 16.5M+ pushes/sec target --
   a target `benchmark_harness.cpp` itself states plainly as a target,
   not a promise, and prints "MET" or "below target on this hardware"
   depending on what your run actually does. Note this is a producer-only
   push rate (`benchmark_harness`'s producer and consumer phases run
   sequentially, not simultaneously) measured over a short ~100-200ms
   burst -- it is not the throughput under real simultaneous
   producer+consumer load. `soak_harness.cpp` in this same directory runs
   both sides at once over a genuinely sustained window instead:
   `./soak_harness <duration_s> <interval_s> <producers> <consumers>
   [max_events]` (e.g. `./soak_harness 40 10 8 8` for a fixed time
   window, or `./soak_harness 60 2 8 8 10000000` for a fixed
   10,000,000-event soak -- both report the same p50/p90/p99/p99.9
   latency percentiles in ns and cycles after a 1.5s warm-up). On the
   reference machine this document was validated on, the sustained
   simultaneous number came in at ~7.28M pushes/sec, materially lower
   than the short-burst range above. Cite the sustained number, not the
   burst target, for anything resembling real deployed load.
3. **Invariant-TSC hardware cycle counting** with zero system calls in
   the hot path.
4. **Zero-copy Python consumption** of the same ring buffer via a
   nanobind bridge, at tens of nanoseconds per event with zero heap
   allocation in the drain loop.

Nothing in this directory depends on the parent repository -- every file
you need is here.

## Reproduce the core metrics (one command)

```
./run_benchmark.sh
```

Checks CPU topology (kernel `isolcpus=`, cpufreq governor), builds
`benchmark_harness` and `soak_harness` with `-O3 -march=native -std=c++17
-Wall -Wextra -Wpedantic -pthread`, runs both (pinned via `taskset` when
it's available), and prints a summary table of the soak's p50/p90/p99/p99.9
in both nanoseconds and CPU cycles -- built directly from each program's
own stdout, not a separate claimed number. No external dependencies beyond
a C++17 compiler and (optionally) `taskset`; typically finishes in well
under a minute. Pass `--events N` to change the soak's event-count target
(default 10,000,000) or `--with-python` to also build and run the optional
nanobind bridge below.

Prefer to drive the two binaries yourself:

```
make
./benchmark_harness
./soak_harness 60 2 8 8 10000000   # duration_s interval_s producers consumers max_events
```

`make` builds `benchmark_harness` and `soak_harness` with `-O3
-march=native -std=c++17 -Wall -Wextra -Wpedantic` (GCC/Clang); running
`benchmark_harness` prints all three native measurements above, with a
hard-fail correctness check on the ring buffer (every pushed event must
be drained exactly once, with a checksum over its payload) and pass/fail
context against the throughput target -- so it tells you plainly if
either your build or your hardware disagrees with the claims.
`soak_harness`'s fifth argument (`max_events`) stops the run as soon as
that many events have been produced *after* a fixed 1.5s untracked
warm-up (letting the ring and OS thread scheduling reach steady state
first -- a sub-second cold-start burst measures ramp-up backlog, not
transport cost; see the comment above `std::this_thread::sleep_for` in
`soak_harness.cpp` if you're tuning this). It reports p50/p90/p99/p99.9
in both ns and cycles, a zero-heap-allocations check over the tracked
window, and a produced==consumed correctness check.

Actual throughput and cache-line-related numbers vary by CPU generation,
core count, and current machine load -- that's expected and is the whole
point of running this on your own hardware rather than trusting a
printed number in a slide deck.

### Alternative: CMake

If you'd rather use CMake (also builds the optional Python bridge below
when it can, and picks sane flags automatically per-compiler including on
MSVC, which has no `-march=native` equivalent):

```
cmake -B build && cmake --build build --config Release
```

The resulting binary is `build/benchmark_harness` (Linux/macOS) or
`build/Release/benchmark_harness.exe` (Windows/MSVC).

## Zero-copy Python bridge (optional)

This part requires a Python 3.8+ toolchain with dev headers and the
`nanobind` package -- most institutional Linux eval boxes have this
already; if not:

```
pip install nanobind
cmake -B build -DPython_EXECUTABLE=$(which python3)
cmake --build build --config Release
```

If nanobind is importable from the interpreter CMake finds, this also
builds `animus_sandbox_bridge` (a `.so`/`.pyd` Python extension module).
Copy it next to `python_bridge_test.py` (or run the script from the
build output directory), then:

```
python python_bridge_test.py
```

This starts a real background native producer thread feeding the ring
unthrottled while the script drains it in a tight loop, timing only
`drain()` itself with `time.perf_counter_ns()`. `drain()` pops events
directly into a scratch buffer allocated once at construction and reused
across every call -- no per-event Python object is constructed, and no
heap allocation happens anywhere in the timed loop. Run 1 of 5 also
asserts `view.flags["OWNDATA"] is False` on the first non-empty batch --
proof this is a real zero-copy view, not numpy silently copying on the
way out. Reference machines in this class have measured roughly 25-55
ns/event on this benchmark (most recently: median 33.2 ns/event across
runs 2-5 of a 5-run session, run 1 excluded as a cold-start outlier --
see `BENCHMARK_DATASHEET.md` for the full run); your own printed number
is what to cite for your hardware.

On Windows with a MinGW-built module, if `import animus_sandbox_bridge`
fails with `DLL load failed`: Python 3.8+ stopped searching `PATH` for a
native extension's own DLL dependencies (PEP 578). `python_bridge_test.py`
already calls `os.add_dll_directory()` for the common MSYS2 install paths;
if yours differs, add it the same way before the `import`.

## What's in this directory

| File | Purpose |
|---|---|
| `run_benchmark.sh` | Single-command entry point: checks CPU topology, builds both harnesses, runs them (pinned via `taskset` when available), prints a summary table. Start here. |
| `Makefile` | `make` / `make soak` -- direct build for `benchmark_harness` and `soak_harness` (GCC/Clang). |
| `CMakeLists.txt` | Cross-compiler build; also builds the optional nanobind module. |
| `ring_buffer.hpp` | Bounded, lock-free MPMC ring buffer (Vyukov's sequence-number design). |
| `sandbox_event.hpp` | The 64-byte, cache-line-aligned event payload shared by the C++ and Python paths. |
| `tsc_clock.hpp` | Invariant-TSC detection, serialized reads, wall-clock calibration -- zero system calls. |
| `benchmark_harness.cpp` | The native benchmark: false-sharing A/B test, MPMC throughput + correctness check, TSC hot-loop cost. Producer-only push rate, short burst. |
| `soak_harness.cpp` | Simultaneous producer+consumer, time- or event-count-bounded throughput and latency percentiles (p50/p90/p99/p99.9/p99.99, ns and cycles), after a fixed warm-up. Built by `make soak` or directly: `g++ -std=c++17 -O3 -pthread soak_harness.cpp -o soak_harness` (or add `-fsanitize=address,undefined -g` for a correctness/leak-checking run). |
| `nanobind_bridge.cpp` | Python extension module exposing the ring buffer as a zero-copy `TelemetryStream`. |
| `python_bridge_test.py` | Measures real drain() latency against the compiled bridge module; verifies `OWNDATA is False`. |

## Notes for evaluators

- All three native measurements in `benchmark_harness` include a
  **structural correctness check** (payload size/alignment
  `static_assert`s, and an exact checksum + count match on every event
  pushed through the ring) that aborts with a clear message if it ever
  fails -- the *performance* numbers are reported as measured, not gated,
  since real throughput depends on your specific CPU and current load.
- Thread pinning (`pin_to_core`) is best-effort on both Windows and
  Linux and silently no-ops if denied; an unpinned run is still a valid
  measurement, just with more scheduler-migration jitter in the tail.
- MSVC has no direct equivalent of `-march=native`; the CMake build uses
  `/O2` (MSVC's Release default) there instead and documents this rather
  than silently omitting a claimed flag.
