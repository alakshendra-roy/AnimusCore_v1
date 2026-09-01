# Animus Engine — Technical Evaluation Kit

A self-contained, dependency-free build of the Animus Engine's core
transport primitive — a lock-free single-producer/single-consumer ring
buffer with cache-line-isolated head/tail cursors — plus a native
round-trip latency benchmark, for independent technical review ahead of a
full pilot evaluation.

Nothing in this directory depends on the rest of the Animus source tree.
It is provided for benchmarking and code review under the terms of your
evaluation agreement; see [`LEGAL_EULA.md`](../LEGAL_EULA.md) at the
repository root.

```
eval-kit/
├── include/
│   └── animus.hpp            Header-only RingBuffer<T>
├── benchmark/
│   └── benchmark_native.cpp  Serialized-RDTSC round-trip latency harness
└── README.md
```

## Building

No build system required — a single translation unit, standard C++17,
x86/x86_64 only (the benchmark uses `__rdtsc()` / `_mm_lfence()`
intrinsics; the header itself is portable, but the benchmark harness is
not).

### GCC / Clang (Linux, macOS, MinGW)

```sh
g++ -O3 -march=native -pthread -std=c++17 \
    eval-kit/benchmark/benchmark_native.cpp \
    -o benchmark_native

# or
clang++ -O3 -march=native -pthread -std=c++17 \
    eval-kit/benchmark/benchmark_native.cpp \
    -o benchmark_native
```

- `-O3` — required. The ring buffer's push()/pop() are small enough that
  an unoptimized build spends most of its time on stack traffic and
  function-call overhead the real code path never pays, producing latency
  numbers with no bearing on actual performance.
- `-march=native` — compiles for the exact instruction set of the build
  machine, needed for the intrinsics above to inline cleanly and for
  `_mm_lfence` codegen to match your CPU's actual serialization behavior.
  Do not use a `-march=native` binary on a different machine than the one
  it was built on.
- `-pthread` — the benchmark spawns a consumer thread with
  `std::thread`; without this flag some libstdc++ configurations link a
  no-op threading stub.

### MSVC

```
cl /O2 /std:c++17 /EHsc eval-kit\benchmark\benchmark_native.cpp /Fe:benchmark_native.exe
```

MSVC has no `-march=native` equivalent; `/O2` plus your normal `/arch:`
setting is sufficient — `__rdtsc()` and `_mm_lfence()` are always
available via `<intrin.h>` on MSVC regardless of `/arch:`.

## Running

```sh
./benchmark_native [producer_core] [consumer_core]
```

Both arguments are optional and default to cores `0` and `1`. Pinning is
best-effort (see "Core isolation" below for why you should not rely on
the benchmark's own pinning alone) and silently does nothing on platforms
other than Windows and Linux — the run still completes, just with more
scheduler-induced jitter in the tail percentiles.

Example:

```
Animus Engine -- Native Ring Buffer Round-Trip Benchmark
==========================================================
Warm-up round trips:   100000
Measured round trips:  1000000
Ring capacity:         4096 slots
Requested affinity:    producer=core 2, consumer=core 3

TSC calibration:       2.5941 cycles/ns (~2.594 GHz)

Round-trip latency (producer push -> consumer echo -> producer pop):
  ---------------------------------------------
  min            312 cycles          120.3 ns
  p50            428 cycles          165.0 ns
  p90            561 cycles          216.3 ns
  p99            892 cycles          343.9 ns
  p99.9         2140 cycles          825.1 ns
  max           9871 cycles         3805.6 ns
  ---------------------------------------------
```

Figures above are illustrative formatting only, not a representative
result — actual numbers depend entirely on your hardware, isolation
setup, and background load. Run it on your own target machine.

The harness measures a **full round trip** (producer → consumer →
producer, two ring crossings) using a serialized `_mm_lfence()` +
`__rdtsc()` pair at each end, so scheduler jitter and out-of-order
reordering around the timed region are excluded from the sample as far
as the ISA allows. TSC frequency is measured at startup against
`std::chrono::steady_clock`, not assumed from the CPU's advertised base
clock, so the nanosecond column stays accurate under turbo boost.

## Core isolation notes

The benchmark's own `SetThreadAffinityMask` / `pthread_setaffinity_np`
calls pin the two threads to specific logical CPUs, but pinning alone
does not stop the OS scheduler from placing *other* work on those same
cores mid-run — that shows up as tail-latency spikes (p99/p99.9) that
have nothing to do with the ring buffer itself. For a clean run:

1. **Isolate the target cores from the general scheduler** (Linux):
   add `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3` to the kernel boot
   command line (`/etc/default/grub`, then `update-grub` and reboot).
   `isolcpus` keeps the general-purpose scheduler off those cores;
   `nohz_full` stops the periodic timer tick from interrupting a busy-spin
   loop on them; `rcu_nocbs` moves RCU callback processing off them.
   Re-run with matching core numbers, e.g. `./benchmark_native 2 3`.

2. **Pick two cores on the same NUMA node**, and ideally two *physical*
   cores rather than two hyperthread siblings of the same physical core
   (check `lscpu -e` — siblings share L1/L2 and will report artificially
   low round-trip latency that does not hold once real work is competing
   for the same execution units). Cross-NUMA-node pairs will show the
   opposite problem: inflated latency from cross-socket cache-coherency
   traffic that is not representative of same-socket deployments.

3. **Disable frequency scaling / turbo boost for the run** if you want
   cycle counts (not just the derived ns column) to be stable
   run-to-run: `cpupower frequency-set --governor performance`, and set
   `/sys/devices/system/cpu/intel_pstate/no_turbo` to `1` (Intel) or the
   equivalent P-state control for your CPU vendor. The benchmark's own
   TSC calibration corrects the *ns* figures for whatever clock the CPU
   actually ran at, but the raw *cycle* counts printed alongside them are
   only comparable across runs at a fixed clock.

4. **Quiesce background load** on the isolated cores specifically —
   `taskset -c 2,3 chrt -f 1 ./benchmark_native 2 3` additionally runs the
   process itself under `SCHED_FIFO` real-time priority, so a runaway
   background process cannot preempt either thread mid-measurement.
   Requires `CAP_SYS_NICE` or root.

5. **Run more than once.** A single 1,000,000-sample run is enough to see
   the shape of the distribution, but tail percentiles (p99.9 especially)
   are sensitive to whatever else touched those cores during that
   specific run — take the median of 3–5 runs before treating a p99.9
   figure as representative.
