# AnimusCore — Lock-Free Telemetry Ring Buffer for Real-Time C++ Systems

[![Build](https://github.com/alakshendra-roy/AnimusCore_v1/actions/workflows/build.yml/badge.svg)](https://github.com/alakshendra-roy/AnimusCore_v1/actions/workflows/build.yml)
![C++: 17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![GCC: 13+](https://img.shields.io/badge/GCC-13%2B-blue.svg)
![Clang: 17+](https://img.shields.io/badge/Clang-17%2B-blue.svg)
![Architecture: x86_64](https://img.shields.io/badge/arch-x86__64-lightgrey.svg)
![License: Evaluation](https://img.shields.io/badge/license-evaluation--only-orange.svg)

A single-producer/single-consumer (SPSC) ring buffer for moving fixed-layout
telemetry structs between a real-time producer thread (a sensor reader, an
OBC ingest loop, a flight-control tick) and a consumer thread (a serializer,
a downlink buffer, a ground-segment bridge) — with a bounded, provable
allocation and latency profile. The core (`AnimusCore_v1/animus.hpp`) is a
zero-dependency, header-only C++17 template; there is no domain-specific
assumption baked into it about what `T` is. See
[`bench/custom_packet_example.cpp`](bench/custom_packet_example.cpp) below
for a 128-byte AOCS-style state-vector frame plugged directly into it.

> **Evaluation Notice.** This repository is made available under the
> **Community Grant** in [`LICENSE`](LICENSE): free, royalty-free use for
> evaluation, technical due diligence, and benchmarking on your own
> systems. It is provided for **internal benchmarking and non-flight
> testbed evaluation only** — it is not flight software, carries no DO-178C
> or similar airworthiness/flight-safety qualification, and is not
> represented as fit for use on board a flight vehicle, in a
> safety-critical control loop, or in any certified system. Production,
> revenue-generating, or flight/mission deployment requires a separate,
> executed Enterprise license — see [`COMMERCIAL.md`](COMMERCIAL.md) and
> [`LEGAL_EULA.md`](LEGAL_EULA.md). This software is provided "as is," with
> no warranty of fitness for any particular purpose — see `LICENSE` and
> `LEGAL_EULA.md` §6 in full before relying on it for anything beyond
> evaluation.

---

## Quickstart

Zero third-party dependencies: the ring buffer core is one header
(`AnimusCore_v1/animus.hpp`), and the example/benchmark binaries below link
against nothing but the C++17 standard library and pthreads.

```bash
git clone https://github.com/alakshendra-roy/AnimusCore_v1.git
cd AnimusCore_v1

cmake -S bench -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/hotpath_bench            # SPSC push/pop latency + zero-alloc assertion
./build/custom_packet_example    # 128B AOCS telemetry frame through the same ring
```

No CMake on hand, or cross-compiling for a target toolchain directly? Both
binaries are single translation units with one include path — this is the
entire build:

```bash
g++ -O3 -std=c++17 -march=native -Wall -Wextra -pthread \
    -IAnimusCore_v1 bench/hotpath_bench.cpp -o hotpath_bench
```

`-march=native` targets the build host's own instruction set — replace it
with an explicit `-march=` target when cross-compiling for different
hardware than the one running the build. On Windows, build inside WSL2 or
with clang/MinGW-w64 on PATH; MSVC's `cl.exe` is not supported by these two
example binaries (the header itself, `animus.hpp`, builds fine under MSVC —
see `AnimusCore_v1/AnimusCore_v1.vcxproj`).

---

## Core Architectural Guarantees

### 1. Cache-line isolation between producer and consumer

`SpscRingBuffer<T>`'s head and tail cursors are each pinned to their own
64-byte cache line (`alignas(kCachelineBytes)`, `AnimusCore_v1/animus.hpp`),
so the producer thread's writes to `head_` never force a cache-coherency
round trip on the cache line the consumer thread's `tail_` lives on, and
vice versa — the classic false-sharing failure mode between a flight-control
producer loop and a telemetry-serializer/downlink-buffer consumer loop
pinned to separate cores.

```
 Cache line 0 (64B)          Cache line 1 (64B)          Cache line N (64B)
+----------------------+    +----------------------+    +----------------------+
| atomic<size_t> head_ |    | atomic<size_t> tail_ |    | cells_[i]: T (record)|
| (producer-owned)     |    | (consumer-owned)     |    | copied whole by      |
+----------------------+    +----------------------+    | push()/pop()         |
                                                          +----------------------+
```

This repo's own cache-line-padding A/B measured a **4.55x** combined
throughput improvement from padding alone on an otherwise-identical
contention benchmark — see
[`BENCHMARK_DATASHEET.md` §3, "Cache-line padding — false-sharing A/B"](BENCHMARK_DATASHEET.md)
for the full methodology, source file, and reproduction command.

### 2. Invariant-TSC cycle timing, not wall-clock

`animus::read_cycle_counter()` reads the CPU's time-stamp counter
(`__rdtsc()` on MSVC, `__builtin_ia32_rdtsc()` on GCC/Clang) directly — a
single instruction, no syscall, no OS context switch — giving sub-clock-tick
resolution that a `steady_clock`/`QueryPerformanceCounter` read cannot match
on hardware where the OS timer quantizes to whole hundreds of nanoseconds.
`bench/hotpath_bench.cpp` and `bench/custom_packet_example.cpp` both wrap
their timed reads in `_mm_lfence()` pairs, which pins the read against
out-of-order execution so a bracketed interval (start-read, do work,
end-read) reflects the work actually being bracketed, not an instruction
that retired early or late relative to it — see the comment on
`read_cycle_counter()` in `animus.hpp` for why the *unserialized* call the
hot path itself uses deliberately skips that fence (a single unbracketed
timestamp has no ordering property to protect, and the fence is a real
pipeline-serializing stall you should not pay on every `push()`).

### 3. Zero heap allocation on the hot path — asserted, not assumed

`SpscRingBuffer<T>` allocates its backing store exactly once, at
construction (`std::vector<T> cells_` sized to the requested capacity,
rounded to a power of two). `push()`/`pop()` after that point perform a
fixed number of atomic loads/stores and one element copy — no `new`, no
container growth, no `std::function`, nothing that can allocate. Every
harness in this repo enforces this at runtime, not just by design: a global
`operator new`/`operator delete` override counts allocations during the
timed region and the harness reports `FAIL` (and exits non-zero) if that
counter is ever nonzero — see the `Heap allocation check` line in the
sample output below.

```cpp
bool push(const T& value) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= capacity_) {
        return false; // full -- never blocks, never allocates
    }
    cells_[head & mask_] = value;
    head_.store(head + 1, std::memory_order_release);
    return true;
}
```

### 4. Non-blocking, bounded backpressure — never stalls the caller

`push()` and `pop()` are both `noexcept` and return `bool`: a full ring
returns `false` to the producer immediately (no lock, no wait, no
condition-variable park) and an empty ring returns `false` to the consumer
the same way. A flight-critical producer thread calling `push()` never
blocks on the state of the consumer — a slow or stalled downlink-buffer
consumer degrades to dropped/backpressured frames at the caller's own
discretion (retry, drop, log-and-continue), never to the producer's own
call stalling on someone else's schedule. There is no unbounded internal
queue anywhere in this path to exhaust memory under sustained producer
overrun, either — capacity is fixed at construction.

---

## Reproducible Harness Output

`bench/hotpath_bench.cpp` runs three phases against `animus::SpscRingBuffer<uint64_t>`
on a live producer/consumer thread pair — median (p50) and burst-tail
(p99/p99.9) turnaround, then the zero-allocation assertion — and prints
exactly this (a real run, reproduced with the Quickstart commands above;
your own numbers will vary by CPU, OS scheduler, and whether the producer/
consumer threads are pinned — see
[`BENCHMARK_DATASHEET.md`](BENCHMARK_DATASHEET.md) for cross-environment
figures, including a WSL2/virtualized baseline and the methodology behind
each one):

```
=====================================================
 AnimusCore -- SPSC Ring Buffer Hot-Path Benchmark
=====================================================
Warmup iterations : 100000
Timed iterations  : 1000000
Ring capacity     : 4096

Calibrated TSC frequency: 2.4192 GHz (2.4192 cycles/ns)

push() latency (single-producer, 1000000 samples):
  P50                 110 cyc          45.47 ns
  P90                 207 cyc          85.57 ns
  P99                 231 cyc          95.49 ns
  P99.9               260 cyc         107.47 ns

pop() latency (single-consumer, 1000000 samples):
  P50                 107 cyc          44.23 ns
  P90                 197 cyc          81.43 ns
  P99                 222 cyc          91.77 ns
  P99.9               250 cyc         103.34 ns

Heap allocation check (timed push region, 1000000 calls): PASS -- zero heap allocations
```

`bench/custom_packet_example.cpp` runs the same non-blocking push/pop loop
against the 128-byte `AocsTelemetryPacket` struct below, and additionally
verifies frame sequence continuity end to end (no reordering, no silent
drop) across the whole run:

```
=====================================================
 AnimusCore -- Custom Telemetry Packet Example (AOCS)
=====================================================
sizeof(AocsTelemetryPacket) = 128 bytes
alignof(AocsTelemetryPacket) = 64 bytes
Ring capacity     : 4096 frames
Warmup frames     : 50000
Timed frames      : 500000

Frames verified (sequence + drain)   : 500000 / 500000
Sequence continuity                  : PASS -- exact, in order
Avg push() cost (timed region)       : 250.1 TSC cycles/frame
Heap allocations (timed push+pop loop, 500000 frames): PASS -- zero heap allocations
```

---

## Plugging a Custom Telemetry Struct In

[`bench/custom_packet_example.cpp`](bench/custom_packet_example.cpp) is a
standalone, self-contained file demonstrating a realistic AOCS/ADCS
state-vector frame — attitude quaternion, body-rate gyro, subsystem health
and status bitfields, a static extension-payload buffer — dropped directly
into `animus::SpscRingBuffer<T>` with no adapter layer:

```cpp
struct alignas(64) AocsTelemetryPacket {
    // --- Timing (16 bytes) ---
    uint64_t tsc_cycles;      // animus::read_cycle_counter() at acquisition time
    uint64_t epoch_utc_us;    // wall-clock capture time, microseconds since Unix epoch

    // --- Provenance (8 bytes) ---
    uint32_t sequence;        // monotonically increasing per-subsystem frame counter
    uint16_t subsystem_id;    // ADCS / EPS / TCS / COMMS / OBC / PROP
    uint16_t frame_version;   // wire-format version, for downstream decoders

    // --- Attitude quaternion, body-to-reference, scalar-first (16 bytes) ---
    float q_w, q_x, q_y, q_z;

    // --- Body-frame angular velocity, rad/s (12 bytes) ---
    float omega_x, omega_y, omega_z;

    // --- Health & status (8 bytes) ---
    uint32_t health_flags;    // bitwise OR of active health conditions
    uint32_t status_flags;    // bitwise OR of active status conditions

    // --- Static extension payload (68 bytes; no pointer, no heap) ---
    uint8_t payload[68];
};
// sizeof == 128, alignof == 64 -- both static_assert'd in the source file.

animus::SpscRingBuffer<AocsTelemetryPacket> ring(4096);

// Producer thread (a driver ISR/poll loop, or a flight-control tick):
ring.push(frame);   // noexcept, non-blocking, no allocation

// Consumer thread (serializer / downlink buffer / ground bridge):
AocsTelemetryPacket out;
if (ring.pop(out)) { /* forward `out` on */ }
```

Three points worth making explicit for a struct you bring yourself:

* **Field order controls padding.** Declare fields in descending natural
  alignment (largest-aligned members first) so the compiler inserts no
  internal gaps — `AocsTelemetryPacket` above does this deliberately;
  verify your own layout with `sizeof`/`offsetof`, not by inspection alone.
* **`alignas(64)` on the struct, not just the ring.** Padding the struct
  itself to a whole multiple of the cache line (here, exactly 2 lines) is
  what keeps every element of the ring's backing array cache-line-aligned,
  not just the first one.
* **`T` must be trivially copyable.** `push()`/`pop()` copy `T` by value —
  that copy is only a flat `memcpy`-equivalent (no hidden allocation, no
  destructor running mid-copy) if `T` has no owning pointers or non-trivial
  special member functions. `custom_packet_example.cpp` enforces this with
  `static_assert(std::is_trivially_copyable<AocsTelemetryPacket>::value, ...)`
  at compile time — copy that pattern for your own packet type rather than
  discovering the violation at runtime.

For CCSDS Space Packet or other variable-length/TLV formats: keep the fixed
128 (or whatever your budget is) as the ring's element type for the
telemetry *header and fixed fields*, and either size the static payload
buffer to your largest expected APID, or carry a pointer/handle to
out-of-band storage in the payload region if frames genuinely vary in size
beyond what a fixed buffer should hold — the ring itself has no opinion on
which you choose, as long as `T` stays trivially copyable and fixed-size.

---

## Further Reading

* [Sandbox Evaluation Harness (`animus-sandbox-eval-v1.0.tar.gz`)](https://github.com/alakshendra-roy/AnimusCore_v1/releases/download/sandbox-eval-v1.0/animus-sandbox-eval-v1.0.tar.gz) —
  standalone, zero-dependency tarball: extract and run `./run_benchmark.sh`
  for the same false-sharing/MPMC throughput and soak-test percentile
  numbers referenced below, with no checkout of this repo required. See the
  [release notes](https://github.com/alakshendra-roy/AnimusCore_v1/releases/tag/sandbox-eval-v1.0)
  for the checksum and full contents.
* [`ARCHITECTURE.md`](ARCHITECTURE.md) — full engine architecture, not
  limited to the SPSC ring covered above.
* [`BENCHMARK_DATASHEET.md`](BENCHMARK_DATASHEET.md) — every latency/
  throughput figure this repository publishes, with the exact reproduction
  command and environment for each one.
* [`LICENSE`](LICENSE) / [`LEGAL_EULA.md`](LEGAL_EULA.md) /
  [`COMMERCIAL.md`](COMMERCIAL.md) — evaluation grant, governing license
  terms, and commercial/production licensing tiers.
* [`docs/FINTECH_CHANGELOG.md`](docs/FINTECH_CHANGELOG.md) — this
  project's original, trading-desk-oriented README and full development
  history (market-data feed adapters, CEP rule engine, mTLS multi-tenancy,
  distributed clustering) — the same `animus.hpp` core covered above, in
  its original application domain.

---

Copyright Holder / Founder: Alakshendra Roy · Governing Jurisdiction: India
Commercial & Procurement: inquiries@animusinfra.com · Technical
Inquiries/Issues: https://github.com/alakshendra-roy/AnimusCore_v1/issues
