# Animus Core — Technical Architecture & Benchmark White Paper

**Classification:** Institutional Technical Specification
**Audience:** HFT platform engineering teams, quantitative researchers, and independent systems evaluators
**Scope:** Architecture, measured performance, and an independently reproducible benchmark protocol. This document contains no corporate, formation, or legal content — see `legal/AnimusCore_Corporate_Dossier.md` for that material.
**Test Hardware:** Intel Core i7-14650HX — 16 cores / 24 logical threads (8 Performance-cores with Hyper-Threading + 8 Efficiency-cores). The P/E split is confirmed by a per-core isolation probe (each of the 24 logical cores timed individually with a pinned busy-loop), not read from a vendor spec sheet alone — see `AnimusCore_v1/BENCHMARKS.md` for the probe methodology and the raw per-core numbers.

---

---

## 1. Executive Architecture Overview

Animus Core is a low-latency telemetry and execution engine written in modern C++17, exposed to Python through a zero-copy nanobind bridge rather than a conventional serialize-and-copy interop layer. The design goal is narrow and specific: remove every avoidable cost between "an event exists in memory" and "a Python-side consumer can see it" — no heap allocation on the hot path, no serialization step, no lock contention on the transport itself.

Three architectural decisions carry most of that weight, and each is independently verifiable rather than asserted:

- **Cache-line-aligned payloads** (`alignas(64)`) so that concurrent producers and consumers never invalidate each other's cache lines through unrelated data sharing the same line.
- **A lock-free MPMC ring buffer** using per-slot sequence-number coordination (the Vyukov design), so producers and consumers each make progress via a single CAS on their own cursor, not a shared lock.
- **Invariant-TSC hardware cycle timing** on the hot path instead of a system clock call, eliminating syscall/vDSO overhead from latency measurements themselves.

The sections below document each of these with the actual measurement methodology, not just the headline number — and Section 7 gives you the exact commands to reproduce every number here on your own hardware.

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

These are real, machine-measured figures from repeated runs on the 24-thread development workstation described above — not a projected or theoretical figure. The `%.2fx` speedup is computed and printed at runtime by the benchmark itself (not a hardcoded constant); a representative single run measured **1.32x**, with repeated runs ranging **1.3x–1.35x**. Run it yourself; the exact numbers will shift with core count and CPU generation, and that variance is expected, not a defect in the test.

---

---

## 3. Lock-Free MPMC Ring Buffer — Ingestion Throughput

The ring buffer is a bounded, lock-free, multi-producer/multi-consumer queue: every slot carries its own sequence number, so a producer or consumer coordinates with the specific slot it is about to touch rather than with one shared full/empty flag. That is what allows independent producers (and independent consumers) to make progress concurrently via a single compare-and-swap each, without blocking on unrelated slots.

**Benchmark methodology:** 8 producer threads, each pinned to a distinct logical core, concurrently enqueue into a ring pre-sized so it can never fill during the timed phase (eliminating backpressure stalls as a confound). Throughput is `total events / wall-clock elapsed time` for that phase — a ~100-200ms burst, not a time-sustained measurement, despite "sustained" having previously labeled the row below. A separate, untimed correctness pass then drains the ring with 8 concurrent consumer threads and verifies an exact checksum over every event's payload — proving no event was lost, duplicated, or corrupted under contention, independent of the timing result.

**Target and observed results (producer-only push rate, short burst):**

| Metric | Value |
|---|---|
| Producer / consumer threads | 8 / 8 (sequential phases — see below for simultaneous) |
| Target push rate | 16.5M+ pushes/sec |
| Observed range (this benchmark, single dev workstation) | 13.5M – 21.4M pushes/sec (bimodal) |
| Correctness check | PASS — exact push/drain checksum match, every run |

The observed range spans both sides of the 16.5M target on the same machine, run to run. Investigated directly rather than assumed: system CPU load sampled immediately before each run showed no correlation with the outcome (the two lowest-load samples were still below target), ruling out ordinary background-process contention. The more likely explanation is the short ~100-200ms measurement window itself — short enough to sometimes catch the CPU package still in a transient turbo-boost state (PL2) and sometimes not, with no way to predict which in advance. The correctness check is unconditional and has never failed across any run performed during this benchmark's validation.

**Simultaneous producer+consumer, time-sustained (the number that reflects real deployed load):**

A ring buffer with no consumer draining it is not the deployed shape — `EngineImpl`'s telemetry path always has one. Running 8 producers and 8 consumers *simultaneously* (not sequential phases) for a genuinely sustained 40-second window, sampled every 10 seconds, gives a materially different and much more stable result:

| Metric | Value |
|---|---|
| Sustained throughput | **~7.28M pushes/sec** (7.26M–7.29M across four 10s samples — under 1% spread) |
| p50 / p90 latency | 834 ns / 1,896 ns |
| Correctness | Exact — produced == consumed (290,993,953 / 290,993,953) |

This sits *below* even the low end of the short-burst producer-only range above, not between its numbers — confirming the burst figures are measuring a transient best-case condition, not a noisy read on the same underlying rate. **For any client-facing citation of throughput under real (producer+consumer) load, use ~7.28M pushes/sec, not the 13.5M-21.4M burst range.** This is a single test session (one run, four samples); treat it as a strong first sustained data point pending a second machine and a longer window, not yet a fully cross-validated final number.

---

---

## 4. Multi-Hour Soak & Reliability Verification

The 40-second sustained figure above establishes throughput; it does not establish that the engine holds correctness and its zero-allocation guarantee over the multi-hour, tens-of-billions-of-events duration a live production desk actually runs for. A separate, longer-duration soak run exists specifically to answer that question — full interactive chronology in `SOAK_TEST_AUDIT_REPORT.html`, raw interval data in `soak_test_report_2026-09-14.log`.

**Methodology:** 8 producer threads + 8 consumer threads running simultaneously, compiled with `-fsanitize=address -fsanitize=undefined` (AddressSanitizer + UndefinedBehaviorSanitizer) rather than the clean `-O3` build used for the throughput figures above — this run is a memory-safety and correctness verification, not a peak-throughput citation, and its numbers should not be compared directly against the ~7.28M/sec clean-build figure without accounting for sanitizer overhead.

| Metric | Value |
|---|---|
| Duration | 2h 38m 49s (9,529.1s) continuous, uninterrupted |
| Total volume | 51,623,591,903 events produced = 51,623,591,903 events consumed (exact, zero deficit at completion) |
| Hot-path heap allocations | 0, every one of the 10 interval samples |
| Sanitizer result | Ran to completion and printed `SOAK_FINAL` without an ASan/UBSan abort — a sanitizer build terminates immediately on a detected memory-safety or undefined-behavior violation, so a clean run to completion is the sanitizer's own attestation that none occurred |
| Steady-state throughput floor | 5.16M–5.33M pushes/sec, final ~100 minutes (10 interval samples), under sanitizer shadow-memory instrumentation |

**The 0–60 minute decline, explained rather than hidden:** throughput opens at ~5.97M/sec and declines to ~5.16M/sec over the first hour before flattening. Two effects overlap here: ASan/UBSan shadow-memory instrumentation is a constant throughput tax present for the entire run (this is why the whole soak sits at 5.16M–5.9M/sec rather than the ~7.28M/sec clean-build figure), and separately, the CPU package is still settling out of its initial turbo (PL2) state into its sustained (PL1) power limit during this window — consistent with the shape of the curve, though not independently confirmed with a core-temperature sensor during this specific run, and stated as inferred rather than as a directly measured fact. Correctness and heap-allocation counters are flat across this exact window, which is what distinguishes a thermal/instrumentation effect from a code-level regression.

**Why this rules out a ring-buffer leak:** the produced/consumed deficit (events sitting in-flight in the ring at the moment of each sample) *shrinks* as a percentage over the steady-state window — 0.0059% → 0.0022% — rather than growing. A real backlog leak would show the opposite trend. Combined with flat throughput and flat correctness across the same window, this is what rules out both a ring-buffer leak and consumer drain starvation as concerns, not merely the zero-deficit figure at final completion in isolation.

---

---

## 5. Invariant-TSC Hardware Cycle Timing

Wall-clock timing calls (`clock_gettime`, `QueryPerformanceCounter`, etc.) cost real, measurable overhead on a latency budget measured in nanoseconds — a vDSO hop or system call inside a hot loop can itself be larger than the thing being measured. Animus Core's timing primitives instead read the CPU's Time Stamp Counter directly via a serialized `__rdtscp` instruction:

- **Zero system calls, zero vDSO hops** — a plain hardware register read.
- **Invariant-TSC verified at startup**, not assumed: `CPUID 0x80000007:EDX[8]` is checked directly, confirming the counter ticks at a fixed rate independent of core P-state/C-state transitions (turbo, power-saving) before any cycle-to-time conversion is trusted.
- **Calibrated, not guessed:** cycles-per-nanosecond is measured by racing the TSC against `std::chrono::steady_clock` over a dedicated 200ms window at startup, rather than trusting an advertised base clock — a wrong assumed conversion factor is a common, silent source of error in TSC-based timing code, and this benchmark closes that gap explicitly.

Measured cost of the read itself: **~10 ns per read**, zero system calls, on a 2.4 GHz-class core (both figures — the clock rate and the per-read cost — are measured at runtime and printed, not hardcoded).

---

---

## 6. Zero-Copy Python Bridge (nanobind)

The Python-side consumption path is built on [nanobind](https://github.com/wjakob/nanobind), exposing the ring buffer to Python as a `TelemetryStream` object whose `drain()` method:

1. Pops up to `batch` events directly into a scratch buffer allocated **once, at construction** — never inside the hot loop.
2. Returns an `nb::ndarray` view over that same memory, aliasing it directly rather than copying it.
3. Constructs **zero per-event Python objects** — the entire batch crosses the language boundary as one array view.

**Methodology:** a background native producer thread feeds the ring unthrottled while the Python-side loop drains it continuously, timing only `drain()` itself with `time.perf_counter_ns()` — a concurrent, steady-state measurement, not a pre-filled-then-drained snapshot.

Reference figure commonly cited for this path is **~34.5 ns/event**. Repeated real runs on development hardware measured a range of roughly **22–66 ns/event** depending on run and machine load, with representative runs clustering around the mid-20s to low-30s — consistent with the ~34.5ns reference figure as a realistic ballpark, not an exact constant. Cite your own measured number from your own hardware; Section 7 gives you the exact reproduction steps.

---

---

## 7. Bare-Metal Reproduction Protocol (`animus_sandbox`)

Every number above should be treated as a claim until you've reproduced it on your own hardware. `animus_sandbox/` is a standalone, dependency-free package built specifically for that: it has no dependency on the rest of this repository, and ships its own build system.

**Two steps:**

```
make
./benchmark_harness
```

That's it. `make` builds with `-O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic` (GCC/Clang); running the binary executes all three native measurements above — the false-sharing A/B test, the 8-producer/8-consumer MPMC throughput run with its correctness check, and the invariant-TSC calibration and hot-loop cost — and prints the results plainly, including whether your hardware met or missed the 16.5M+ pushes/sec target.

A CMake path (`cmake -B build && cmake --build build --config Release`) is also provided for cross-compiler builds and additionally builds the optional nanobind Python bridge module when a Python 3.8+ toolchain with `nanobind` installed is available, enabling reproduction of the Section 6 figure via `python python_bridge_test.py`.

Full details, including the optional Python bridge setup, are documented in `animus_sandbox/README.md`.

The Section 4 multi-hour soak figures are reproduced separately via `benchmarks/soak_test_engine.py` driving `animus_sandbox/soak_harness.cpp` built with `-fsanitize=address -fsanitize=undefined` — a multi-hour run, not a two-command quick check, so it is not part of this two-step protocol. See `SOAK_TEST_AUDIT_REPORT.html` for the full run configuration.
