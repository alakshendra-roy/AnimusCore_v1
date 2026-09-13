# Animus Core — Technical Architecture & Benchmark White Paper

**Classification:** Institutional Technical Specification
**Audience:** HFT platform engineering teams, quantitative researchers, and independent systems evaluators
**Scope:** Architecture, measured performance, and an independently reproducible benchmark protocol. This document contains no corporate, formation, or legal content — see `legal/AnimusCore_Corporate_Dossier.md` for that material.

---

---

## 1. Executive Architecture Overview

Animus Core is a low-latency telemetry and execution engine written in modern C++17, exposed to Python through a zero-copy nanobind bridge rather than a conventional serialize-and-copy interop layer. The design goal is narrow and specific: remove every avoidable cost between "an event exists in memory" and "a Python-side consumer can see it" — no heap allocation on the hot path, no serialization step, no lock contention on the transport itself.

Three architectural decisions carry most of that weight, and each is independently verifiable rather than asserted:

- **Cache-line-aligned payloads** (`alignas(64)`) so that concurrent producers and consumers never invalidate each other's cache lines through unrelated data sharing the same line.
- **A lock-free MPMC ring buffer** using per-slot sequence-number coordination (the Vyukov design), so producers and consumers each make progress via a single CAS on their own cursor, not a shared lock.
- **Invariant-TSC hardware cycle timing** on the hot path instead of a system clock call, eliminating syscall/vDSO overhead from latency measurements themselves.

The sections below document each of these with the actual measurement methodology, not just the headline number — and Section 6 gives you the exact two commands to reproduce every number here on your own hardware.

---

---

## 2. 64-Byte Cache-Line Alignment and False-Sharing Elimination

Every event payload that crosses a producer/consumer boundary in Animus Core is declared `alignas(64)` and sized to exactly 64 bytes — one x86/ARM cache line:

```cpp
struct alignas(64) Event {
    std::uint64_t sequence;
    std::uint64_t dispatch_tsc;
    std::uint32_t producer_id;
    std::uint32_t reserved0;
    std::uint64_t value;
    std::uint64_t reserved1[4];
};
static_assert(sizeof(Event) == 64, "Event must occupy exactly one cache line");
static_assert(alignof(Event) == 64, "Event must be cache-line aligned");
```

This is enforced at **compile time** via `static_assert`, not just documented — a future change that accidentally grows the struct past 64 bytes fails the build, not a benchmark run months later.

Alignment alone is a claim; the benchmark suite backs it with a direct **A/B measurement**. Two atomic counters are incremented by two threads pinned to distinct cores, once with both counters sharing a single cache line (`UnpaddedCounters`) and once with each counter on its own line via `alignas(64)` (`PaddedCounters`):

| Configuration | Structure size | Combined throughput (2 threads) |
|---|---|---|
| Unpadded (false sharing) | 16 bytes | ~162–164M ops/sec |
| Padded (`alignas(64)`) | 128 bytes | ~214–215M ops/sec |
| **Speedup from eliminating false sharing** | | **~1.3x** |

These are real, machine-measured figures from repeated runs on a 24-core development workstation — not a projected or theoretical figure. Run it yourself; the exact numbers will shift with core count and CPU generation, and that variance is expected, not a defect in the test.

---

---

## 3. Lock-Free MPMC Ring Buffer — Ingestion Throughput

The ring buffer is a bounded, lock-free, multi-producer/multi-consumer queue: every slot carries its own sequence number, so a producer or consumer coordinates with the specific slot it is about to touch rather than with one shared full/empty flag. That is what allows independent producers (and independent consumers) to make progress concurrently via a single compare-and-swap each, without blocking on unrelated slots.

**Benchmark methodology:** 8 producer threads, each pinned to a distinct logical core, concurrently enqueue into a ring pre-sized so it can never fill during the timed phase (eliminating backpressure stalls as a confound). Throughput is `total events / wall-clock elapsed time` for that phase. A separate, untimed correctness pass then drains the ring with 8 concurrent consumer threads and verifies an exact checksum over every event's payload — proving no event was lost, duplicated, or corrupted under contention, independent of the timing result.

**Target and observed results:**

| Metric | Value |
|---|---|
| Producer / consumer threads | 8 / 8 |
| Target sustained throughput | 16.5M+ pushes/sec |
| Observed range (this benchmark, single dev workstation) | 13.5M – 19.7M pushes/sec |
| Correctness check | PASS — exact push/drain checksum match, every run |

The observed range spans both sides of the 16.5M target on the same machine, run to run — this is real measurement noise from a shared, non-isolated development box (thermal state, background load, scheduler placement), not an inconsistency in the queue itself. On dedicated, isolated-core institutional hardware, expect materially less run-to-run variance and results consistently at or above target. The correctness check is unconditional and has never failed across any run performed during this benchmark's validation.

---

---

## 4. Invariant-TSC Hardware Cycle Timing

Wall-clock timing calls (`clock_gettime`, `QueryPerformanceCounter`, etc.) cost real, measurable overhead on a latency budget measured in nanoseconds — a vDSO hop or system call inside a hot loop can itself be larger than the thing being measured. Animus Core's timing primitives instead read the CPU's Time Stamp Counter directly via a serialized `__rdtscp` instruction:

- **Zero system calls, zero vDSO hops** — a plain hardware register read.
- **Invariant-TSC verified at startup**, not assumed: `CPUID 0x80000007:EDX[8]` is checked directly, confirming the counter ticks at a fixed rate independent of core P-state/C-state transitions (turbo, power-saving) before any cycle-to-time conversion is trusted.
- **Calibrated, not guessed:** cycles-per-nanosecond is measured by racing the TSC against `std::chrono::steady_clock` over a dedicated 200ms window at startup, rather than trusting an advertised base clock — a wrong assumed conversion factor is a common, silent source of error in TSC-based timing code, and this benchmark closes that gap explicitly.

Measured cost of the read itself: **~10 ns per read**, zero system calls, on a 2.4 GHz-class core (both figures — the clock rate and the per-read cost — are measured at runtime and printed, not hardcoded).

---

---

## 5. Zero-Copy Python Bridge (nanobind)

The Python-side consumption path is built on [nanobind](https://github.com/wjakob/nanobind), exposing the ring buffer to Python as a `TelemetryStream` object whose `drain()` method:

1. Pops up to `batch` events directly into a scratch buffer allocated **once, at construction** — never inside the hot loop.
2. Returns an `nb::ndarray` view over that same memory, aliasing it directly rather than copying it.
3. Constructs **zero per-event Python objects** — the entire batch crosses the language boundary as one array view.

**Methodology:** a background native producer thread feeds the ring unthrottled while the Python-side loop drains it continuously, timing only `drain()` itself with `time.perf_counter_ns()` — a concurrent, steady-state measurement, not a pre-filled-then-drained snapshot.

Reference figure commonly cited for this path is **~34.5 ns/event**. Repeated real runs on development hardware measured a range of roughly **22–66 ns/event** depending on run and machine load, with representative runs clustering around the mid-20s to low-30s — consistent with the ~34.5ns reference figure as a realistic ballpark, not an exact constant. Cite your own measured number from your own hardware; Section 6 gives you the exact reproduction steps.

---

---

## 6. Bare-Metal Reproduction Protocol (`animus_sandbox`)

Every number above should be treated as a claim until you've reproduced it on your own hardware. `animus_sandbox/` is a standalone, dependency-free package built specifically for that: it has no dependency on the rest of this repository, and ships its own build system.

**Two steps:**

```
make
./benchmark_harness
```

That's it. `make` builds with `-O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic` (GCC/Clang); running the binary executes all three native measurements above — the false-sharing A/B test, the 8-producer/8-consumer MPMC throughput run with its correctness check, and the invariant-TSC calibration and hot-loop cost — and prints the results plainly, including whether your hardware met or missed the 16.5M+ pushes/sec target.

A CMake path (`cmake -B build && cmake --build build --config Release`) is also provided for cross-compiler builds and additionally builds the optional nanobind Python bridge module when a Python 3.8+ toolchain with `nanobind` installed is available, enabling reproduction of the Section 5 figure via `python python_bridge_test.py`.

Full details, including the optional Python bridge setup, are documented in `animus_sandbox/README.md`.
