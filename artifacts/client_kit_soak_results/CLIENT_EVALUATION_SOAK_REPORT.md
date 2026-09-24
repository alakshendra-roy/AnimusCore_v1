# Client Evaluation Soak Report

Generated: 2026-09-24 05:55 UTC
Harness: `poc_eval/poc_eval_harness.cpp` (built via `run_poc_eval.bat`, MSVC cl.exe /O2 /std:c++17)
Method: unattended repeated soak -- 153 back-to-back runs, 20 s each, cores pinned starting at logical core 4 via `animus::sys::pin_current_thread_to_core_exclusive`, 3 s cooldown between runs. Every measurement below comes from samples actually captured on this machine during this soak window -- nothing simulated or backfilled.

## Summary card

```
+-----------------------------------------------------------------+
|        ANIMUS CORE -- CLIENT POC EVALUATION SOAK RESULT         |
+-----------------------------------------------------------------+
|  CPU        : Intel(R) Core(TM) i7-14650HX                      |
|  Cores      : 24                                                |
|  Runs       : 153                                               |
|  Wall time  : 62.1 min                                          |
|  Pin base   : core 4                                            |
+-----------------------------------------------------------------+
|  SPSC INGEST LATENCY (median across all runs)                   |
|    P50   : 21.08 ns                                             |
|    P90   : 24.39 ns                                             |
|    P99   : 27.7 ns                                              |
|    Max   : 30030989.93 ns (worst sample)                        |
+-----------------------------------------------------------------+
|  TICK-TO-TELEMETRY LATENCY                                      |
|    P50   : 30.59 ns                                             |
+-----------------------------------------------------------------+
|  MPMC THROUGHPUT (pushes/sec)                                   |
|    Median: 7471133                                              |
|    Range : 5139660 - 8563594                                    |
+-----------------------------------------------------------------+
|  Heap allocations : 0 total across all runs                     |
|  Sequence intact  : 153 / 153 runs                              |
|  Overall PASS     : 0% (0 / 153 runs)                           |
+-----------------------------------------------------------------+
```

## Aggregate metrics (median across all 153 runs)

| Metric | Value |
|---|---|
| CPU | Intel(R) Core(TM) i7-14650HX |
| Logical cores | 24 |
| TSC frequency | 2.4192 GHz |
| Producer threads (MPMC) | 8 |
| SPSC push P50 | 21.08 ns |
| SPSC push P90 | 24.39 ns |
| SPSC push P99 | 27.7 ns |
| SPSC push Max (worst observed) | 30030989.93 ns |
| Tick-to-telemetry P50 | 30.59 ns |
| MPMC pushes/sec (median) | 7471133 |
| MPMC pushes/sec (range) | 5139660 - 8563594 |
| Heap allocations, total across all runs | 0 (PASS -- zero heap allocations across every run) |
| Event sequence integrity | intact on all 153 runs (PASS) |
| Overall PASS rate | 0% (0 / 153) |

## Notes

- Full per-run data: [`soak_summary.csv`](soak_summary.csv).
- Raw stdout/stderr for every individual run: [`raw_logs/`](raw_logs/).
- Machine-readable aggregate: [`soak_aggregate.json`](soak_aggregate.json).
- This soak used core pinning (base core 4) but not kernel-level isolation (isolcpus/nohz_full/rcu_nocbs) or NUMA-pinned allocation -- see `docs/technical_eval/COMPATIBILITY_TUNING_GUIDE.md` for what a fully isolated host adds on top of this.
- `run_poc_eval.bat` had a pre-existing quoting bug in its MSVC `/Fo:` argument (trailing backslash inside a quoted path being parsed as an escaped quote by cl.exe, corrupting the object-file output path) that made the client-facing entry point fail to build on this machine. Fixed as part of this verification run -- see the diff to `run_poc_eval.bat`.
- `poc_eval_harness.cpp` was extended to additionally report P90 and Max latency (previously only P50/P99/P99.9), reusing the same already-collected sample arrays -- no change to measurement technique, allocation tracking, or PASS/FAIL criteria.
- The 30030989.93 ns SPSC Max above is a single outlier from one run (`raw_logs/run_0123.log`) out of 153 -- every other run's max stayed in the 30 us-1 ms range, and P99.9 held steady at ~39-46 ns across the whole soak. Consistent with the harness's own note that this host isn't running kernel-level core isolation (isolcpus/nohz_full/rcu_nocbs), so a rare OS-scheduler preemption can still land on a pinned thread; it is not a recurring or worsening pattern over the 62-minute window.
- Every run's `[Phase A -- SPSC Ingest]` target line reported FAIL (P50 ~21 ns measured vs. the harness's built-in <15 ns target) -- consistently, not as a soak-induced regression. The <15 ns target was calibrated on different reference hardware; this machine's baseline sits a few ns above it on every single run, including the very first.
