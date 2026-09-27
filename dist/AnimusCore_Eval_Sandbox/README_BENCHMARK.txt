AnimusCore -- External Evaluation Sandbox
==========================================
SCOPE
  eval_harness.exe measures exactly one primitive: single-producer
  ShmRing<ExecutionEvent>::try_push() latency, zero consumer contention,
  no OS syscalls in the timed loop. Header-only C++17; 64-byte alignment
  enforced at compile time (static_assert on RingHeader/CacheAligned).

BUILD / RUN
  g++ -std=c++17 -O3 -march=native -DNDEBUG -I include \
      tests/eval_harness.cpp -o eval_harness.exe -lws2_32 -lbcrypt -liphlpapi
  strip --strip-all eval_harness.exe && ./eval_harness.exe
  (exit 0 + "PASS" = zero-alloc confirmed; MSVC: /std:c++17 /O2 /DNDEBUG)

MEASURED BASELINE (1,000,000 iters, unpinned dev core, local TSC ~2.42 GHz)
  Zero heap allocations:  PASS      Invariant TSC:  yes
  Raw cycles:  min 39 / P50 43 / P90 47 / P99 128
  Approx ns:   min ~16 / P50 ~18  (recalibrate TSC per host)
  Pin for a tighter tail: animus::sys::pin_current_thread_to_core_exclusive
  (thread_affinity.hpp) -- this build does not pin itself.

BENCHMARK INTEGRITY NOTE
  Not measured here, not claimed here:
    - ~100 ns end-to-end tick-to-trade turnaround
    - 16.5M+ pushes/sec multi-producer burst throughput
  Both are separate figures from the integration suite
  (benchmark_harness.cpp / telemetry_benchmark.cpp), different code paths
  under different concurrency. Do not cite this sandbox against them.
