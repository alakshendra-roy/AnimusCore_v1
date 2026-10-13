# AnimusCore Sandbox Quickstart (5 minutes)

One file, `sandbox_quickstart.cpp`: a lock-free SPSC ring, a pinned producer and consumer, invariant-TSC
timestamps, and a hot-path allocation audit. Standard library plus glibc/POSIX only. It compiles under GCC 11+
and Clang 13+.

## Requirements

- Linux x86-64 with glibc. The file needs `pthread_setaffinity_np`, `RDTSCP` and an invariant TSC. The
  allocation audit relies on glibc, so on other libcs it reports `UNAVAILABLE` and the run fails.
- A compiler: `g++` 11+ or `clang++` 13+.
- At least 2 CPUs, 3 recommended (a third for housekeeping). The two hot threads should be on two different
  physical cores.

## 1. Build

```sh
g++ -O3 -march=native -std=c++17 -pthread sandbox_quickstart.cpp -o sandbox_quickstart
# or
clang++ -O3 -march=native -std=c++17 -pthread sandbox_quickstart.cpp -o sandbox_quickstart
```

Do not add `-fsanitize=*`. Sanitizers provide their own `malloc` and can defeat the audit's interposer, which
the self-test will then flag.
Optional: `-DANIMUS_SPIN_PAUSE=1` adds `PAUSE` to the spin loops. Leave it off on dedicated cores. It is
only worth turning on if the consumer's sibling hyperthread is busy.

## 2. Run

```sh
./sandbox_quickstart                       # auto-picks two distinct physical cores, 2M messages, 1 us pacing
./sandbox_quickstart --producer-cpu 2 --consumer-cpu 4 --housekeeping-cpu 0 \
                     --messages 5000000 --interval-ns 500
./sandbox_quickstart --duration-s 60 --report-every-s 10     # time-bounded, per-interval percentiles
```

Output, in order:

| Section | Meaning |
|---|---|
| `placement` | Chosen CPUs. `DISTINCT` means different physical cores (read from sysfs). `SAME (SMT siblings!)` means the number is a same-core handoff, not cross-core. |
| `tsc` | Invariant TSC and RDTSCP flags, current clocksource, calibrated frequency, and the timer floor (cost of one stamp pair). |
| `Handoff latency` | min / p50 / p90 / p99 / p99.9 / max, in cycles and ns. |
| `Integrity` | Sequence gaps, payload corruption, TSC regressions, and producer stalls on a full ring. All must be 0, except stalls, which should be 0 at sane pacing. |
| `Hot-path heap` | `malloc`-family and `free` calls made by the producer and consumer threads during the run window. Must be 0. |
| `VERDICT` / `RESULT` | `PASS` or `FAIL` (exit code 0 or 1), plus one machine-readable line. |

**What the latency number is.** It is the one-way time from the producer's TSC stamp, taken just before the
release-store that publishes the slot, to the consumer's `RDTSCP` after it observes and reads that slot. It is
not a round trip. The timer floor is included, not subtracted.

**Pacing matters.** At `--interval-ns 0` the producer floods the ring, and the result is queueing delay. Use a
pacing interval that matches your real message rate.

## 3. Swap in your own payload

All three integration points are at the top of `sandbox_quickstart.cpp`, under "INTEGRATION POINT".

```cpp
struct MyTick { /* your fields; trivially copyable */ };
using Payload = MyTick;

inline void fill_payload(Payload& p, std::uint64_t seq) noexcept { /* producer: write one message */ }
inline bool consume_payload(const Payload& p, std::uint64_t seq) noexcept { /* consumer: decode/act; false = corrupt */ }
```

Rules:

- `Payload` must be trivially copyable. No `std::string`, `std::vector` or pointers into producer-owned memory.
  A `static_assert` enforces this.
- Each slot is `{seq, tsc, Payload}`, a 16-byte header plus your payload, rounded up to a multiple of 64 bytes.
  A payload of 48 bytes or less fits one cacheline. The banner prints the resulting `cacheline(s)/msg`.
- Your code in `fill_payload` and `consume_payload` is not measured. The producer stamps after `fill_payload`,
  and the consumer stamps before `consume_payload`.
- Return `false` from `consume_payload` if you can detect corruption. That fails the run.
- If your messages vary in size, use the largest as `Payload` plus a length field. The ring has fixed-size slots.

### Using the ring in your own threads

Copy section 3 (`Slot`, `SpscRing`) into your tree. Use the zero-copy API, which writes straight into ring memory:

```cpp
static SpscRing<Slot<MyTick>, 1u << 14> ring;          // static storage: no heap, no aligned-new needed

// producer thread
if (auto* s = ring.claim()) { s->seq = n; build(s->payload); s->tsc = tsc_stamp(); ring.publish(); }

// consumer thread
if (const auto* s = ring.front()) { handle(s->payload); ring.pop(); }
```

`claim`/`publish` must come from exactly one thread, and `front`/`pop` from exactly one other thread.
`try_push`/`try_pop` are copying convenience wrappers. The capacity must be a power of two. Each slot is
64 bytes, so 16384 slots is 1 MiB.

## 4. Get a number you can defend

1. **Different physical cores.** The harness warns when your CPUs are SMT siblings. On many Intel parts, logical
   CPUs 0 and 1 are siblings of one core. A sibling-pair result is a same-core figure and is much lower than a
   true cross-core one.
2. **Not in a VM.** The banner prints `hypervisor=YES (virtualised)` and a warning when it detects one. vCPU
   pinning and TSC behaviour are then best-effort.
3. **Governor and isolation.** For publishable figures, run `setup_baremetal_soak.sh` as root first. It fixes the
   governor, THP and IRQ steering, and prints the `isolcpus` / `nohz_full` boot line when it is missing.
4. **Soak it.** `run_isolated_soak.sh` compiles this file, runs it for 30 minutes on the isolated cores,
   logs per-interval p50 / p90 / p99 / p99.9, and gates on zero hot-path allocations and zero integrity errors.
5. **Read the tail separately from the median.** Percentiles are histogram bucket upper bounds, exact below 128
   cycles and within 1.6% above. A p99.9 or max far above p99 on a non-isolated machine is scheduler or
   platform noise, not the ring.

## Limits of this harness

- It measures a one-way, same-socket handoff on the host you run it on. It does not measure network or
  kernel-bypass NIC paths, NUMA-crossing handoffs, or your decode and strategy code.
- The cross-core latency figure is only valid if the TSC is synchronized across the two cores. `tsc_regressions`
  counts handoffs where the consumer's stamp is earlier than the producer's, and the run fails if there are any.
  `setup_baremetal_soak.sh` also runs a dedicated cross-core TSC probe.
- The allocation audit counts calls from the two hot threads inside the measured window. Setup, printing and
  thread creation are excluded on purpose. It does not see allocations made through `mmap` or direct syscalls.
