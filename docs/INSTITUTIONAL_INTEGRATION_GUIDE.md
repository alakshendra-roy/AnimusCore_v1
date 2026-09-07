# Animus Core -- Institutional Integration Guide

**Audience:** trading desks, quant infrastructure teams, and fintech
engineering teams evaluating or integrating Animus Core's C++20
zero-allocation ingestion engine and its Python SDK.

**Scope:** this document covers the architectural guarantees the engine
makes, how to reproduce its benchmark claims on your own hardware, how to
tune a production Linux host for sub-microsecond p99 determinism, and
minimal working snippets for both the native C++ API and the zero-copy
Python SDK. It does not cover order routing, exchange connectivity, or
risk controls -- Animus Core is an ingestion/telemetry substrate, not a
trading system.

---

## 1. Architecture

### 1.1 The SPSC ring buffer

The core primitive is `animus::eval::SpscRingBuffer<T>`
(`animus-eval-kit/include/spsc_ring_buffer.hpp`), a fixed-capacity,
single-producer/single-consumer ring. The narrower SPSC contract (as
opposed to a general MPMC queue) is what buys the latency: `push()`/`pop()`
need only

- a **relaxed** load of the caller's own last-published cursor,
- an **acquire** load of the other side's cursor, and
- a **release** store to publish,

with no compare-and-swap retry loop and no lock. This is not enforced at
runtime -- a debug-only thread-identity check would cost real cycles on
the exact path this class exists to make fast. Calling `push()` from two
threads concurrently (or `pop()` from two threads concurrently) is
undefined behavior by design, not an oversight.

### 1.2 64-byte cache-line isolation

Two independent things are cache-line-aligned, for two independent reasons:

1. **The ring's cursors.** `head_` (producer-owned) and `tail_`
   (consumer-owned) are each pinned to their own `alignas(64)` line inside
   `SpscRingBuffer<T>`. Without that, the producer publishing a new `head_`
   value would invalidate the same cache line the consumer is spin-polling
   `tail_` from (false sharing), forcing a cross-core cache-coherency round
   trip on *every* operation instead of only when a cursor genuinely
   changes ownership.

2. **The wire frame itself.** `TelemetryFrame`
   (`animus-eval-kit/include/telemetry_frame.hpp`) is `alignas(64)` and
   its field layout pads `sizeof(TelemetryFrame)` to exactly 64 bytes, so
   one frame never straddles two cache lines and an array of frames never
   causes false sharing between *adjacent elements* -- a different
   failure mode than (1), but the same underlying mechanism.

`benchmarks/BENCHMARK_REPORT.md`'s false-sharing A/B test quantifies the
effect directly: two `std::atomic<uint64_t>` counters on the same cache
line vs. each on its own line, incremented 20,000,000 times each by a
dedicated thread -- the padded layout wins by a measured multi-x factor on
the machine that report was generated on. Regenerate it on your own
hardware (`python benchmarks/generate_benchmark_report.py`) rather than
trusting that number on a different CPU.

### 1.3 Memory ordering semantics

| Operation | Ordering | Why |
|---|---|---|
| Producer's own `head` read (before push) | `relaxed` | Only the producer ever writes `head`; no other thread's writes need to be observed. |
| Producer's read of consumer's `tail` (full check) | `acquire` | Must observe every prior consumer `pop()` release-store, so "is the ring full" is never a stale answer. |
| Producer's publish of the new `head` | `release` | Makes the just-written slot visible to the consumer's next acquire-load of `head`. |
| Consumer's own `tail` read (before pop) | `relaxed` | Only the consumer ever writes `tail`. |
| Consumer's read of producer's `head` (empty check) | `acquire` | Must observe every prior producer `push()` release-store. |
| Consumer's publish of the new `tail` | `release` | Makes the freed slot visible to the producer's next acquire-load of `tail`. |

This is the standard "SPSC ring via acquire/release cursor exchange"
pattern, not a custom protocol -- it is exactly what
`animus::eval::SpscRingBuffer<T>::push()`/`pop()` implement, and the same
pattern `animus::sys::ipc::ShmRing<T>` (`include/animus/shm_ipc.hpp`)
reuses for the cross-process shared-memory variant.

### 1.4 Thread affinity

`animus_bench` pins its producer and consumer threads to specific logical
cores (`animus::sys::pin_current_thread_to_core`,
`include/animus/thread_affinity.hpp`) and raises their scheduling priority
(`animus::sys::set_thread_high_priority`) before the timed window opens.
This matters for tail latency specifically: an unpinned thread migrating
cores mid-run pays a cold-cache penalty and is subject to the general
scheduler's timeslice preemption, both of which show up as p99.9/p99.99
outliers, not as a lower median. Section 3 below covers isolating cores at
the kernel level so this pinning is effective rather than fighting the
scheduler.

---

## 2. Reproducing the Benchmark Claims

Everything in this repository's benchmark reports traces back to a real,
reproducible run on the machine that generated it -- never a hand-typed
figure. To reproduce on your own hardware:

```bash
git clone <this repository>
cd AnimusCore_v1
./deploy_verify.sh
```

This single command (Ubuntu/Debian, WSL2, or RHEL-family -- see
`deploy_verify.sh`'s own header comment for exact supported environments):

1. Detects your OS and installs `cmake`, a C++20 compiler, and
   `python3-dev` if missing.
2. Builds `animus_bench` in Release mode with `-O3` and `-march=native`.
3. Runs a sustained pass (default: 10,000,000 msgs/sec target) and a burst
   pass.
4. Builds the Python SDK (`sdk/python`) and runs its own throughput
   benchmark, pulling `TelemetryFrame` records directly into NumPy.
5. Asserts both `animus_bench` passes report **0 dropped frames, 0
   corrupted frames, 0 hot-path heap allocations** -- the script fails
   loudly (non-zero exit) if either guarantee doesn't hold on your
   machine, rather than silently reporting a partial result.
6. Refreshes `benchmarks/reports/ANIMUS_BENCHMARK_REPORT.html` with your
   own machine's CPU model, core topology, and cache sizes, plus this
   run's own numbers.

Run individual steps directly if you want to isolate one part:

```bash
# C++ harness only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target animus_bench
./build/bin/animus_bench --rate 10000000 --duration 10
./build/bin/animus_bench --rate 10000000 --duration 10 --burst

# Python SDK only
pip install ./sdk/python
python sdk/python/bench_python_throughput.py --duration 10
```

**Zero-allocation proof, precisely stated:** `animus_bench` replaces the
six standard replaceable `operator new`/`delete` overloads with
instrumented wrappers that count every call process-wide, snapshots that
counter immediately before the timed window opens (after thread launch,
ring allocation, and priority/affinity setup -- all legitimate setup-phase
allocation) and again immediately after both threads finish, and reports
the *difference* as the hot-path allocation count. This isolates the
measurement to the actual producer/consumer loop, not the whole process's
lifetime.

---

## 3. Production Kernel Tuning Guide (Linux)

The defaults below target sub-microsecond p99 determinism on a dedicated
or near-dedicated host. None of this is required to run `animus_bench` or
the SDK correctly -- it narrows the *tail* of the latency distribution by
removing sources of OS jitter, and every step is something you should
measure the effect of on your own workload before committing to in
production.

### 3.1 Core isolation (`isolcpus`, `nohz_full`, `rcu_nocbs`)

Add to the kernel command line (`/etc/default/grub`'s
`GRUB_CMDLINE_LINUX_DEFAULT`, then `update-grub`/`grub2-mkconfig` and
reboot). Adjust the CPU list to your own topology -- reserve the cores your
producer/consumer threads will be pinned to, leaving at least one core
free for the kernel/housekeeping:

```
isolcpus=nohz,domain,22-23 nohz_full=22-23 rcu_nocbs=22-23
```

- **`isolcpus`** removes the listed cores from the general SMP scheduler's
  load-balancing domain -- ordinary processes/threads are never scheduled
  onto them unless explicitly pinned there (matching `animus_bench`'s own
  `pin_current_thread_to_core` calls).
- **`nohz_full`** stops the periodic scheduling-clock tick on those cores
  when only one runnable task is present, removing a small but real
  recurring interrupt source from the isolated cores' latency budget.
- **`rcu_nocbs`** offloads RCU (read-copy-update) callback processing away
  from the isolated cores onto a housekeeping core, so the isolated cores
  never pay for kernel-internal RCU grace-period work.

Verify after reboot: `cat /sys/devices/system/cpu/isolated` should list
your isolated core range.

### 3.2 Real-time scheduling priority (`chrt -f 99`)

Run the producer/consumer process (or a specific thread, via
`sched_setscheduler` from within the process -- see
`animus::sys::set_thread_high_priority`'s Linux branch,
`include/animus/thread_affinity.hpp`) under the `SCHED_FIFO` real-time
policy at the highest static priority:

```bash
sudo chrt -f 99 ./build/bin/animus_bench --rate 10000000 --duration 30
```

`SCHED_FIFO` at priority 99 preempts every normal (`SCHED_OTHER`) task on
the same core and is never subject to the CFS scheduler's timeslice
fairness -- appropriate for a bounded, well-behaved hot loop, dangerous
for anything that could spin indefinitely on a shared resource (a
runaway `SCHED_FIFO` thread can starve the rest of the system, including
`sshd`, hard enough to require a physical reboot). Test on a machine you
can access out-of-band before deploying this in production.

### 3.3 Huge pages

Reserve 2 MiB huge pages for the ring buffer's backing allocation to
reduce TLB pressure on a large, hot-path-resident buffer (`animus_bench`'s
default ring is `1u << 20` frames &times; 64 bytes = 64 MiB):

```bash
echo 64 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
```

Adjust the count for your ring's actual size (`nr_hugepages &times; 2 MiB
>= ring backing store size`) and NUMA node (`node0` above; check
`numactl --hardware` for a multi-socket host and pin allocation to the
same node your pinned cores belong to). This repository's ring buffer
implementations use a plain `std::vector<T>`/OS-page-backed shared memory
today, not an explicit `mmap(..., MAP_HUGETLB, ...)` allocation -- huge
pages here take effect only if you additionally configure
transparent huge pages (`always` madvise mode) or extend the allocator,
which is a deployment-specific integration step, not something
`animus_bench` does automatically.

### 3.4 Verification checklist

After applying the above, re-run `deploy_verify.sh` (or `animus_bench`
directly) and compare the p99/p99.9 latency figures against an unturned
baseline on the same hardware -- the isolation and priority changes above
are specifically aimed at the *tail*, and a report that only compares
mean/p50 will not show their effect.

---

## 4. API Usage

### 4.1 Native C++ (producer/consumer in one process)

```cpp
#include "animus/thread_affinity.hpp"
#include "spsc_ring_buffer.hpp"
#include "telemetry_frame.hpp"

using animus::eval::SpscRingBuffer;
using animus::eval::TelemetryFrame;

SpscRingBuffer<TelemetryFrame> ring(1u << 20); // 1,048,576 frames

// Producer thread:
animus::sys::pin_current_thread_to_core(2);
animus::sys::set_thread_high_priority();
TelemetryFrame frame{};
frame.sequence_id = 1;
frame.timestamp_ns = /* ingress timestamp */ 0;
std::memcpy(frame.symbol, "AAPL", 4);
frame.price = 189.50;
frame.volume = 100;
while (!ring.push(frame)) {
    animus::cpu_relax(); // ring momentarily full -- retry, never drop silently
}

// Consumer thread (a different OS thread, pinned to a different core):
animus::sys::pin_current_thread_to_core(3);
TelemetryFrame out;
if (ring.pop(out)) {
    // out.price, out.symbol, out.timestamp_ns, ... are ready to use
}
```

### 4.2 Python SDK (zero-copy NumPy consumption)

```python
from animus_sdk import AnimusRingBuffer, AnimusConsumer

ring = AnimusRingBuffer(capacity=1 << 20)
consumer = AnimusConsumer(ring)

# Feed real market data from Python (fine for moderate rates; for the
# highest throughput, push from native code and consume from Python):
ring.push(sequence_id=1, symbol="AAPL", price=189.50, volume=100, flags=0)

# Or drive a background native producer for load testing:
ring.start_synthetic_load(target_frames_per_sec=0)  # 0 = unthrottled

# Zero-copy ingestion loop: to_numpy() aliases the same scratch memory
# drain() just filled -- no per-frame Python object, no heap allocation
# on this path beyond what pop() itself always costs.
frames = consumer.to_numpy(consumer.drain(max_count=8192))
print(frames.dtype.names)          # ('sequence_id', 'timestamp_ns', 'symbol', 'price', 'volume', 'flags')
print(frames["price"].mean())      # a real NumPy reduction over the live buffer

ring.stop_synthetic_load()
```

See `sdk/python/bench_python_throughput.py` for a full ingestion-loop
benchmark and `sdk/python/README.md` for installation instructions.

---

## 5. Support

Questions about integrating Animus Core into a specific execution
architecture, or about the pilot program, should go through the same
channel that provided you this repository. See `README.md` at the repo
root for the project overview and `ARCHITECTURE.md` for a lower-level
walkthrough of the wider engine (execution client, telemetry, secure
transport) beyond the ingestion ring covered here.
