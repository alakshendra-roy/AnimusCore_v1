# Animus Core -- Low-Latency Benchmark Report

**Automatically generated** by `benchmarks/generate_benchmark_report.py` on 2026-08-29 17:02:31. Every number below comes from one real run of the compiled `AnimusCore_v1/animus_benchmark_suite.cpp` binary on this machine -- regenerate this file with the same command to reproduce or refresh it on any other machine; do not hand-edit the figures below.

## System Under Test

| | |
|---|---|
| Logical CPUs | 24 |
| Platform | Windows |
| Compiler | GCC 15.2 |
| Build flags | `-std=c++17 -O2 -pthread` |
| Generated | 2026-08-29 17:02:31 |

## 1. Tick-to-Trade End-to-End Latency

Single-threaded, sequential push -> poll -> `ExecutionClient::submit()` round trip through `animus::MarketDataFeed`, 500,000 ticks, timed tick-by-tick with `std::chrono::steady_clock`. `LoopbackBrokerGateway` provides an instant, deterministic in-process fill, so this isolates the pipeline's own overhead (ring push, ring poll, order construction, execution-client dispatch, telemetry record) from any real broker/exchange latency.

| Percentile | Latency (ns) | Latency (us) |
|---|---:|---:|
| Min | 0.0 | 0.000 |
| p50 | 100.0 | 0.100 |
| Mean | 94.2 | 0.094 |
| p99 | 100.0 | 0.100 |
| p99.9 | 200.0 | 0.200 |
| Max | 118,500.0 | 118.500 |

**Throughput (sequential, single-threaded):** 8,416,814 ticks/sec

p50 and p99.9 both land under 1 us on this run (0.100 us / 0.200 us) -- genuinely sub-microsecond, not a rounding artifact of a coarser unit.

**Measurement note:** repeated values quantized to whole hundreds of nanoseconds in the table above reflect this machine's `steady_clock` resolution (Windows: backed by `QueryPerformanceCounter`), not true single-digit-nanosecond precision -- true per-call latency may be finer than this clock can distinguish. A handful of outlier samples in the hundreds-of-microseconds range (visible in Max, not in p99.9) are consistent with an occasional OS scheduling interruption across 500,000 iterations on a general-purpose, non-real-time OS, not a defect in the measured pipeline.

**Methodology note:** an earlier two-thread version of this benchmark (a dedicated producer thread racing a separate consumer thread) was tried first and measured mean/p50 latency in the *milliseconds*, not nanoseconds -- an unpaced producer outruns the consumer and most ticks sit queued in a growing backlog before ever being processed, the same "producer-faster-than-consumer backlog" effect this repo already documented once before for its shared-memory IPC transport (`README.md`'s Phase 16 section). That was measuring queueing delay, not pipeline cost, so it was discarded in favor of the single-threaded sequential design actually used here -- a decision-loop metric, matching how a real single-threaded low-latency trading loop (read tick, decide, send order) actually operates, and the same methodology `execution_interop_demo.cpp` already uses for its own submit() latency numbers.

## 2. Lock-Free Ring Buffer Throughput Under 8-Thread Concurrency

`animus::LockFreeRingBuffer<TelemetryPayload>` (the Vyukov MPMC ring `EngineImpl`'s own telemetry ring uses) driven by 8 concurrent producer threads (real `std::thread`s, real OS scheduling across real cores), each pushing 200,000 records (1,600,000 total), all contending on the same compare-exchange retry loop. The ring is pre-sized to hold every push from every thread, so throughput reflects `push()` cost under real contention, not backpressure stalls from a concurrent consumer -- see the Limitations section below for what that does and doesn't cover.

| Metric | Value |
|---|---:|
| Producer threads | 8 |
| Total pushes | 1,600,000 |
| Wall-clock elapsed | 0.2217 s |
| **Aggregate throughput** | **7,216,498 pushes/sec** |
| Per-push mean latency | 996.1 ns |
| Per-push p50 latency | 600.0 ns |
| Per-push p99 latency | 5,200.0 ns |

Correctness was verified as part of this same run, not assumed: after all producer threads joined, every one of the 1,600,000 pushes was drained back out exactly once (the benchmark binary exits with an error rather than reporting a result if that count doesn't match) -- the throughput number above isn't hiding lost or corrupted pushes under 8-way contention.

## 3. CPU Cache Locality

Pointer-chase latency vs. working-set size (the standard "membench" technique): one 64-byte node per cache line, chained via a Sattolo-shuffled permutation (a single cycle covering every node, no shorter sub-cycles) so each jump is data-dependent on the previous one and effectively unpredictable to the hardware prefetcher. 3,000,000 chase steps timed per working-set size.

| Working Set | Avg Latency (ns/access) | Tier |
|---:|---:|---|
| 4 KB | 1.135 | Tier 1 (fastest) |
| 8 KB | 1.213 | Tier 1 (fastest) |
| 16 KB | 1.243 | Tier 1 (fastest) |
| 32 KB | 1.198 | Tier 1 (fastest) |
| 64 KB | 3.728 | Tier 2 |
| 128 KB | 3.767 | Tier 2 |
| 256 KB | 3.205 | Tier 2 |
| 512 KB | 3.638 | Tier 2 |
| 1 MB | 4.516 | Tier 2 |
| 2 MB | 8.696 | Tier 3 |
| 4 MB | 11.891 | Tier 3 |
| 8 MB | 23.769 | Tier 4 |
| 16 MB | 18.350 | Tier 4 |
| 32 MB | 77.646 | Tier 5 (slowest -- consistent with spilling into DRAM) |
| 64 MB | 96.094 | Tier 5 (slowest -- consistent with spilling into DRAM) |
| 128 MB | 100.132 | Tier 5 (slowest -- consistent with spilling into DRAM) |

**Tiers are inferred from jumps (>1.8x) between consecutive points in this sweep's own measured curve, not a claim about this CPU's actual L1/L2/L3 sizes** -- this benchmark never queries CPUID or any vendor spec sheet for that information, so it does not assert exact cache-tier boundaries. What the data does show: distinct latency "knees" as working-set size grows, consistent with successive on-die cache levels being exceeded, ending in a clearly higher, flatter latency plateau once the working set is large enough to spill into DRAM.

### False-Sharing A/B Test

Two `std::atomic<uint64_t>` counters, each incremented 20,000,000 times by its own dedicated thread, concurrently. **Unpadded**: both counters on the same cache line (the classic false-sharing setup -- every increment on either thread invalidates the other core's cached copy of the line). **Padded**: each counter on its own cache line via `alignas(64)` -- the exact layout `animus::LockFreeRingBuffer` (`enqueue_pos_`/`dequeue_pos_`) and `animus::SpscRingBuffer` (`head_`/`tail_`) already use in this codebase, so this result is a direct empirical justification of an existing design choice, not an abstract exercise.

| Layout | Combined ops/sec |
|---|---:|
| Unpadded (false sharing) | 123,747,214 |
| Padded (`alignas(64)`) | 562,887,161 |

**4.55x** throughput from cache-line padding alone, two threads, no other change.

## Methodology & Limitations

- **Single-machine measurement.** Every number above is from one run on the system described in "System Under Test" -- absolute figures will differ on different hardware; run this suite on your own target machine before relying on these numbers for capacity planning.
- **No OS-level core isolation.** None of these benchmarks pin threads or reserve cores exclusively (Linux `isolcpus`, Windows CPU Sets) -- background OS/process load can and does affect tail latency (see `AnimusCore_v1/QUICKSTART.md`'s CPU-pinning caveat for a documented case of this cutting the other way, worsening p99.99 even after pinning to a good core).
- **Ring buffer throughput measures producer-side contention only** -- the ring is pre-sized to hold the entire run, so no concurrent consumer drains it during the timed window. A workload with simultaneous concurrent producers *and* consumers will see different numbers (likely lower throughput, since a draining consumer adds its own cache-coherency traffic on the same ring).
- **Tick-to-trade latency is a decision-loop metric, not a concurrent-pipeline metric** -- see the methodology note in section 1 for why a two-thread producer/consumer design was tried and discarded.
- **`LoopbackBrokerGateway` fills instantly** -- tick-to-trade latency here measures this pipeline's own overhead only, never a real broker's or exchange's response time.
- **Clock resolution.** Latency figures below roughly 100 ns are at or near this machine's `steady_clock` resolution; treat single-digit-nanosecond distinctions as not meaningful.

## Reproducing This Report

```bash
python benchmarks/generate_benchmark_report.py
```

Compiles `AnimusCore_v1/animus_benchmark_suite.cpp` (g++/clang++, `-std=c++17 -O2 -pthread`) into `benchmarks/_build/` if the binary is missing or the source has changed since it was last built, runs it, and overwrites this file.
