# Animus Evaluation Kit -- SPSC Ring Buffer Benchmark

A standalone, air-gapped benchmark harness for `animus::eval::SpscRingBuffer<T>` --
the zero-copy, cache-aligned single-producer/single-consumer ring buffer that
underlies Animus's market-data and execution transport. Zero external
runtime dependencies: C++20 standard library plus the platform's native
pthreads.

```
animus-eval-kit/
├── CMakeLists.txt                    # C++20, -O3/-march=native, LTO/IPO, pthread
├── README.md                         # this file
├── include/
│   └── spsc_ring_buffer.hpp          # self-contained SPSC ring buffer
└── benchmarks/
    └── bench_ring_buffer.cpp         # latency + saturation-throughput harness
```

No package manager, no vendored third-party code, no network access
required at configure or build time.

## What this measures

1. **One-way handoff latency** -- per-event time from `push()` on the
   producer to `pop()` on the consumer, on two different physical cores,
   reported as p50 / p90 / p99 / p99.9 / max over 2,000,000 samples (after a
   200,000-event warm-up).
2. **Saturation throughput** -- sustained events/sec over a fixed window
   with the producer pushing as fast as possible; `push()` retries under
   backpressure rather than dropping, so the run also reports events
   pushed vs. consumed to confirm zero drops.

Numbers are only meaningful measured on quiet, tuned bare metal. A shared
CI runner, a laptop on battery, or a VM without CPU pinning support will
produce results dominated by scheduler noise, not the ring buffer.

## Linux kernel prerequisites

- A recent 64-bit Linux kernel (5.x+) with an **invariant TSC**
  (`cat /proc/cpuinfo | grep constant_tsc` should show `constant_tsc`
  on every core). The harness's `__rdtsc()` calibration assumes this; on
  hardware without it, reported nanosecond figures are meaningless even
  though the raw cycle counts are still internally consistent.
- At least 4 logical cores, since the harness pins to core 2 (producer) and
  core 3 (consumer) by default. Pass two different core IDs as `argv[1]`
  `argv[2]` to target a different pair (e.g. an isolated pair on a larger
  box).
- Root or `CAP_SYS_NICE` is *not* required for `pthread_setaffinity_np`
  affinity pinning used here, but is required if you extend the harness to
  request `SCHED_FIFO` real-time scheduling.

## CPU performance governor

Frequency scaling is the single largest source of run-to-run latency
variance on an otherwise idle benchmark box. Pin every core to its highest
fixed P-state before running:

```sh
# Inspect current governor per core
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Force "performance" on every core (requires root)
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
    echo performance | sudo tee "$cpu/cpufreq/scaling_governor" > /dev/null
done

# Or, if cpupower is installed:
sudo cpupower frequency-set -g performance
```

Also disable turbo/boost while benchmarking, since opportunistic boost
clocks are themselves a source of run-to-run TSC-vs-wall-clock variance on
some microarchitectures even with an invariant TSC:

```sh
# Intel
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# AMD (cpufreq-based)
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
```

## `isolcpus` tuning

Cores 2 and 3 (the harness's defaults) should be excluded from the kernel's
general-purpose scheduler so no other process, interrupt handler, or kernel
thread preempts the producer or consumer mid-measurement. Add to the kernel
command line (`/etc/default/grub`'s `GRUB_CMDLINE_LINUX_DEFAULT`, then
`sudo update-grub` and reboot):

```
isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
```

- `isolcpus=2,3` removes cores 2/3 from the general SMP scheduling
  balancer -- only threads explicitly affinitized there (via
  `pthread_setaffinity_np`, which is exactly what the harness does) will
  ever run on them.
- `nohz_full=2,3` stops the scheduler tick from firing on those cores when
  only one runnable task is present, removing a periodic-interrupt source
  of tail-latency jitter.
- `rcu_nocbs=2,3` offloads RCU callback processing off the isolated cores
  onto a housekeeping core.

Also route hardware interrupts away from the isolated cores (`irqbalance`
should already avoid isolated CPUs on recent versions; verify with
`cat /proc/interrupts` that IRQ counts on cores 2/3 stay flat during a run).

## Transparent Huge Pages (THP): `madvise`

Leave THP enabled but set to `madvise` rather than `always`, so the
`std::vector<T>` backing the ring only gets huge pages when explicitly
requested (this harness does not call `madvise(MADV_HUGEPAGE)` itself --
the ring is far smaller than a single 2 MB huge page at any realistic
capacity), and so no unrelated process on the box can trigger a background
`khugepaged` compaction pass that steals CPU from the isolated cores
mid-benchmark:

```sh
cat /sys/kernel/mm/transparent_hugepage/enabled
echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
```

`always` is the setting to avoid here specifically because `khugepaged`'s
periodic compaction scans are a scheduler-invisible source of jitter that
`isolcpus` alone does not prevent (it is kernel-thread work, not a
userspace task the isolation cordons off by itself unless also pinned away
via `rcu_nocbs`/IRQ affinity as above).

## Build

Two steps:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/bench_ring_buffer`. Run it directly (defaults to
producer=core 2, consumer=core 3):

```sh
./build/bench_ring_buffer
# or target a different core pair:
./build/bench_ring_buffer 4 5
```

## Acquire-release barrier semantics

`SpscRingBuffer<T>::push()`/`pop()` use exactly three atomic operations per
call, no compare-exchange:

```cpp
// push() -- producer thread only
const uint64_t head = head_.load(std::memory_order_relaxed); // my own cursor
const uint64_t tail = tail_.load(std::memory_order_acquire);  // their cursor
...
slots_[head & mask_] = value;                                 // plain write
head_.store(head + 1, std::memory_order_release);             // publish
```

- The producer's `relaxed` load of `head_` is safe because `head_` is
  *only ever written by the producer itself* -- there is no other writer to
  synchronize with, so no ordering guarantee is needed on that read.
- The producer's `acquire` load of `tail_` pairs with the consumer's
  `release` store to the same variable. That pairing is what makes it safe
  for the producer to reuse slot `head & mask_` once `tail_` has advanced
  past it: the acquire load guarantees the producer sees every write the
  consumer performed (specifically, finishing its read out of that slot)
  *before* the consumer's release store that published the new `tail_`.
  Without acquire/release here, the producer could overwrite a slot the
  consumer is still mid-read on, on a CPU that reorders the consumer's slot
  read after its `tail_` store becomes visible.
- The producer's `release` store to `head_` pairs with the consumer's
  `acquire` load of `head_` in `pop()`, guaranteeing that when the consumer
  observes the new `head_` value, it also observes the just-written slot
  data -- the write to `slots_[head & mask_]` happens-before the
  `head_.store(..., release)`, and that happens-before the consumer's
  `acquire` load returns the same value.

This is the minimum synchronization the SPSC contract requires: one
release-acquire pair per direction, no locks, no CAS retry loop. A general
multi-producer queue cannot use this scheme (two producers racing on the
same `head_` need compare-exchange to serialize their claims), which is
exactly the throughput/latency cost this class avoids by assuming SPSC.

## 64-byte cache-line isolation

```cpp
alignas(kCacheLineSize) std::atomic<uint64_t> head_{ 0 }; // producer-owned
alignas(kCacheLineSize) std::atomic<uint64_t> tail_{ 0 }; // consumer-owned
```

`kCacheLineSize` is 64 bytes -- the line size on essentially all x86_64 and
most 32/64-bit ARM cores (widen it to 128 if specifically targeting Apple
Silicon or a Neoverse-class ARM server part).

Without the `alignas`, `head_` and `tail_` would very likely land in the
same 64-byte cache line (they are declared adjacently and are each only 8
bytes). Every producer write to `head_` would then invalidate the cached
copy of the *same line* the consumer is spin-polling `tail_` from on
another core, forcing a cache-coherency transaction (MESI/MOESI
Invalidate + RFO) on essentially every single `push()`, even though the
consumer never touches `head_`'s bytes -- this is classic false sharing.
Padding each cursor out to its own line means the producer's writes only
ever invalidate a line the consumer isn't reading, and vice versa; the only
genuine cross-core traffic left is the *intentional* handoff (the consumer
actually needs to observe `head_`, the producer actually needs to observe
`tail_`), not an accidental one caused by memory layout.

This is also why the benchmark harness pins producer and consumer to
*different physical cores* rather than just different logical threads: the
cost being measured -- and the cost this class is optimized against -- only
exists in a genuine cross-core cache-coherency handoff. Two threads
scheduled onto the same core (or onto sibling SMT threads sharing an L1/L2)
would understate real deployment latency.
