# Animus Core — Compatibility & Systems Tuning Guide

**Classification:** Institutional Technical Reference
**Audience:** Systems engineers, SREs, and co-location architects responsible for building, deploying, and tuning the host an Animus Core engine instance runs on.
**Companion documents:** [`SECURITY_INFOSEC_MEMO.md`](SECURITY_INFOSEC_MEMO.md) (network/memory/thread-safety posture) · [`PILOT_EVAL_CHECKLIST.md`](PILOT_EVAL_CHECKLIST.md) (the fuller host-qualification checklist this guide's §2 summarizes; use that document's checklist for a formal sign-off, this one as a standing reference) · [`ARCHITECTURE.md`](ARCHITECTURE.md) (ingestion pipeline deep-dive) · [`../../BENCHMARK_DATASHEET.md`](../../BENCHMARK_DATASHEET.md) (measured numbers this tuning is validated against).

---

## 1. Compiler & Language Standard Support

The core engine and its C-ABI bindings are **strict C++17** — see `CMakeLists.txt:13`/`14` and `bindings/CMakeLists.txt:39`/`40` (`CMAKE_CXX_STANDARD 17`, `CMAKE_CXX_STANDARD_REQUIRED ON`). This is what `AnimusCore_v1/animus.hpp`, `include/animus/*.hpp`, and `AnimusNative.dll`/`.so` are built and validated against, and what `run_poc_eval.sh`/`run_poc_eval.bat` (repository root) compile the POC evaluation harness with.

| Toolchain | Minimum version | Notes |
|---|---|---|
| GCC | 9+ | `std::hardware_destructive_interference_size` (used for cache-line-derived padding, `animus.hpp:351`) is gated on the `__cpp_lib_hardware_interference_size` feature-test macro at compile time — on a libstdc++ that doesn't define it, the code falls back to a conservative 64-byte constant (`animus.hpp:361`) rather than failing to build. GCC 9 is the practical floor for reliable C++17 `<filesystem>`/structured-binding/`if constexpr` support this codebase otherwise assumes. |
| Clang | 10+ | Same fallback behavior; Clang's libc++ has had the feature-test macro inconsistently available across versions, which is exactly why the code checks for it rather than assuming C++17 implies it. |
| MSVC | 2019+ (v16.8 / `_MSC_VER` ≥ 1928, first version with usable C++17 conformance mode `/std:c++17` + `/permissive-`) | `run_poc_eval.bat` targets `cl.exe`/`clang-cl`; see that script for the exact invocation. |

**A separate, broader-floor component:** the optional zero-copy Python SDK bridge under `sdk/python/csrc/` targets **C++20** (`sdk/python/CMakeLists.txt:15`, `CMAKE_CXX_STANDARD 20`) — a narrower toolchain floor (GCC 11+ / Clang 14+ / MSVC 2022+ for full C++20 support) than the core engine above. This is a distinct build target from `bindings/animus_py.cpp` (the core engine's own nanobind bridge, C++17, §4 below) — do not assume the C++20 floor applies to the core engine, or vice versa.

Compiler flags used for performance validation: `-O3 -march=native -std=c++17` (Clang/GCC) or `/O2 /std:c++17` (MSVC/clang-cl) — see `run_poc_eval.sh`/`run_poc_eval.bat` and `bench/build_bench.sh`/`.bat` for the exact, reproducible invocations. `-march=native` pins the build to the CPU it was compiled on; cross-machine binary distribution should instead target a specific `-march=` microarchitecture level (e.g. `-march=x86-64-v3`) rather than `native`.

## 2. OS & Kernel Tuning

These reduce scheduler-induced jitter on the cores a producer/consumer pair occupies. **None of this is required for correctness** — the ring buffers are correct on an untuned host, as documented in [`SECURITY_INFOSEC_MEMO.md`](SECURITY_INFOSEC_MEMO.md) §3–§4 — but the aggressive tail-latency figures in this project's benchmark documents assume it. See [`PILOT_EVAL_CHECKLIST.md`](PILOT_EVAL_CHECKLIST.md) §2 for the full step-by-step checklist with verification commands; this section is the condensed reference.

- **CPU isolation (`isolcpus`)** — remove the target core(s) from the general SMP scheduling domain so the kernel never lands an unrelated task on them: `isolcpus=<core_list>` on the kernel command line.
- **Tickless operation (`nohz_full`)** — stop the periodic scheduler timer tick on isolated cores so a spinning producer/consumer thread isn't interrupted on a fixed cadence: `nohz_full=<core_list>`.
- **RCU callback offload (`rcu_nocbs`)** — move RCU callback processing off the isolated cores: `rcu_nocbs=<core_list>`. Combine all three on one `GRUB_CMDLINE_LINUX` line and reboot.
- **Invariant TSC** — required for the TSC-cycle-based latency measurements this project's benchmarks (and `poc_eval/poc_eval_harness.cpp`) use to be meaningful across cores/frequency states. Check `grep -m1 -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo` (Linux) or CPUID leaf `0x80000007:EDX[8]` directly (`run_poc_eval` prints this check itself — see the "Invariant TSC" line in its scorecard). Without it, cycle counts taken on different cores or across a frequency-scaling event are not directly comparable.
- **Hugepages** — reduce TLB pressure for large ring-buffer segments (`ShmRing<T>`/`SpmcRing<T>` backing memory, `include/animus/shm_ipc.hpp`). On Linux, either transparent hugepages (`madvise`) or explicit `hugetlbfs` reservations (`/proc/sys/vm/nr_hugepages`) sized to the configured ring capacity × record stride.
- **Thread/core pinning (application-level, not just kernel isolation)** — `include/animus/thread_affinity.hpp` provides `animus::sys::pin_current_thread_to_core(core_id)` (Windows: `SetThreadAffinityMask`; POSIX: `sched_setaffinity`) and `pin_current_thread_to_core_exclusive(core_id)`, which additionally attempts to keep the OS from scheduling anything else onto that core. Pin the producer and consumer to *separate* physical cores on the same NUMA node (not hyperthread siblings of each other) to avoid both cross-core cache-coherency stalls and SMT contention on the same physical execution unit.
- **NUMA placement** — for a multi-socket host, allocate the ring's backing memory and pin producer/consumer threads on the same NUMA node; a ring spanning nodes turns every cache-line transfer into a cross-socket QPI/Infinity-Fabric hop.
- **Frequency governor** — set `performance` (not `powersave`/`ondemand`) on the isolated cores so C-state/P-state transitions don't introduce latency spikes mid-measurement; this also keeps the invariant-TSC assumption meaningful in practice even on CPUs where it holds architecturally.

**Environment disclosure applies here too:** a run of `run_poc_eval.sh`/`run_poc_eval.bat` on a general-purpose, non-isolated development host will show materially higher P99/P99.9 tail latency than the same code on a tuned host per the above — the harness prints this caveat directly in its own scorecard. Treat an untuned run as a correctness/regression check, not a production-representative latency number.

### 2.1 Measured Impact of Pinning Alone (Application-Level Only, No Kernel Isolation)

`poc_eval_harness.cpp` (repo root, `run_poc_eval.sh`/`.bat`) takes an optional 3rd CLI argument, `pin_base_core`, that pins every producer/consumer thread in both phases to a distinct logical core via `animus::sys::pin_current_thread_to_core_exclusive` — deliberately the pin **+ elevated-priority** variant, not plain affinity pinning, per that function's own documented rationale (a pinned-but-not-prioritized thread has nowhere to migrate to when preempted, which measurably worsened P99.99 in this project's own earlier benchmarking — see `thread_affinity.hpp`'s comment above `pin_current_thread_to_core_exclusive`). The table below is one actual before/after run on the same host, back to back, isolating that one variable:

| Metric | Unpinned | Pinned (`pin_base_core=4`) | Change |
|---|---|---|---|
| P50 SPSC ingest latency | 55.39 ns | 19.43 ns | 2.9x lower |
| P99 SPSC ingest latency | 109.13 ns | 25.22 ns | 4.3x lower |
| P50 tick-to-telemetry | 155.84 ns | 34.31 ns | 4.5x lower |
| P99 tick-to-telemetry | 398,189 ns | 56.22 ns | ~7,100x lower |
| P99.9 tick-to-telemetry | 2,488,476 ns | 59,682 ns | ~42x lower (one outlier still visible) |
| MPMC pushes/sec (4 producers) | 7.82M | 9.66M | +24% |
| Heap allocations (both phases) | 0 | 0 | unchanged |
| Target: P50 ingest < 15 ns | FAIL | FAIL (19.43 ns — within ~30%) | — |
| Target: P50 tick-to-telemetry < 100 ns | FAIL | **PASS** | — |

**Read this narrowly.** This is `SetThreadAffinityMask`/`SetThreadIdealProcessor` + `THREAD_PRIORITY_TIME_CRITICAL`/`HIGH_PRIORITY_CLASS` on an **unmodified Windows development laptop** (Intel Core i7-14650HX, hybrid P-core/E-core, 24 logical cores) — no `isolcpus`/`nohz_full`/`rcu_nocbs` (Linux-only kernel boot parameters this host doesn't have), no `REALTIME_PRIORITY_CLASS` (requires a privilege this session didn't hold, so it fell back to `HIGH_PRIORITY_CLASS` per `set_thread_high_priority()`'s documented fallback), and no NUMA/hugepage tuning. It demonstrates that application-level pinning *by itself* already removes most scheduler-induced tail latency (the P99 tick-to-telemetry improvement in particular), while the remaining P99.9 outlier and the still-missed sub-15ns ingest target are consistent with exactly the residual OS interference §2's kernel-level items (`isolcpus`, `nohz_full`, real-time scheduling class) exist to remove. A Linux host with the full §2 checklist applied — and, on Linux, `sched_setscheduler(SCHED_FIFO)` actually succeeding rather than falling back to `nice(-20)` — would be expected to close more of the remaining gap, but that is an expectation to verify on that host, not a number asserted here.

Reproduce this comparison yourself:

```bash
./run_poc_eval.sh 10 4          # unpinned baseline
./run_poc_eval.sh 10 4 4        # pinned, base core 4 (Linux/macOS)
run_poc_eval.bat 10 4 4         # pinned, base core 4 (Windows)
```

## 3. Low-Overhead Python Bridge Integration (nanobind)

The core engine's Python interop (`bindings/animus_py.cpp`, C++17, built via `bindings/CMakeLists.txt`) is a native **nanobind** extension binding `animus::SpscRingBuffer<TelemetryPayload>` directly — chosen over pybind11 specifically for nanobind's smaller per-call dispatch overhead and binary footprint (a build-time property to verify yourself for your own workload, per that file's own comment, not to take as an unverified claim).

Design properties relevant to a systems integrator:

- **Zero-copy across the Python/C++ boundary.** `drain()` copies ring events into a scratch buffer the binding object owns once (the same producer→consumer memcpy-equivalent cost the ring's `pop()` always has), then returns a Python buffer-protocol view *over that same scratch memory* — no per-event Python object is constructed, and no second copy hands the batch to Python. `animus_sdk`'s `to_numpy()` path turns that view into a structured NumPy array with the same zero-copy property.
- **View lifetime is explicit, not GC-managed.** The buffer-protocol view returned by `drain()`/`poll()` aliases scratch memory that is only valid until the *next* `drain()`/`poll()` call on the same object — copy out (`np.array(view, copy=True)`) before calling again if you need to retain it.
- **GIL discipline.** Producer thread lifecycle management (spawn/join) and the ring's spin-wait-for-data loop run under `nb::call_guard<nb::gil_scoped_release>`, so a native producer thread is never blocked behind Python holding the GIL (e.g. mid-garbage-collection); the GIL is reacquired only for the narrow window of constructing the returned Python object, which is itself a Python-API call and cannot be avoided.
- **Build floor:** `bindings/CMakeLists.txt` targets C++17 (matching the core engine, §1) for this bridge specifically — distinct from the separate C++20-floor SDK kit under `sdk/python/csrc/` (§1), which binds a different `SpscRingBuffer` (from `animus-eval-kit/include/spsc_ring_buffer.hpp`) for evaluation/benchmark purposes rather than the shipped engine's own telemetry ring.
- **Python version / NumPy floor:** the packaged SDK's own `pyproject.toml` states its general compatibility floor; [`PILOT_EVAL_CHECKLIST.md`](PILOT_EVAL_CHECKLIST.md) §1 states the narrower range (CPython 3.10–3.14, NumPy ≥ 1.26) that this project's own pilot-acceptance thresholds were validated against — use the narrower range for a production sizing decision.

## 4. Reproducing This Guide's Claims

```bash
# Confirm the core engine's actual C++ standard floor (not assumed):
grep -n "CMAKE_CXX_STANDARD" CMakeLists.txt bindings/CMakeLists.txt sdk/python/CMakeLists.txt

# Confirm invariant TSC on your own host (Linux):
grep -m1 -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo
cat /sys/devices/system/clocksource/clocksource0/current_clocksource   # expect "tsc"

# Run the POC harness and read its own printed CPU/TSC diagnostics + scorecard:
./run_poc_eval.sh 10        # Linux, unpinned
run_poc_eval.bat 10         # Windows, unpinned

# Compare against a pinned run (see §2.1) on the same host:
./run_poc_eval.sh 10 4 4    # Linux, pinned to base core 4
run_poc_eval.bat 10 4 4     # Windows, pinned to base core 4
```

*This guide describes build/runtime configuration as implemented in the code cited above. For which specific latency figures a given order form contractually guarantees under a tuned Reference Topology versus an untuned host, see [`../../COMPLIANCE_AND_RISK_MITIGATION.md`](../../COMPLIANCE_AND_RISK_MITIGATION.md) §1.*
