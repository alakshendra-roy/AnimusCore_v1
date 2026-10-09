# AnimusCore ring-buffer verification suite

Zero-allocation audit, invariant-TSC latency profiling and a zero-copy NumPy bridge for AnimusCore's lock-free
telemetry rings (bounded SPSC and MPMC, 64-byte slots, and the header-based `SpscQueue`). C++17, one `make` target per claim, x86-64 Linux.

| Target | What it establishes | Needs |
|---|---|---|
| `make check` | No thread in the process calls any heap entry point while 10,000,000 frames cross each ring. | x86-64 Linux, glibc, C++17 compiler |
| `make bench` | A pinned, serialized, calibrated TSC latency distribution (p50 / p90 / p99 / p99.9, ticks and ns). The profiler is itself checked against a known 1 µs spin. | x86-64 Linux, invariant TSC |
| `make pybridge` | A NumPy array that aliases the ring's slot memory: no copy, read-only, lifetime-safe, intact under a concurrent native producer. | CMake, nanobind (to build), NumPy (to run) |

This repository is written to be read and attacked by someone who does not trust it. Every claim has a check, and every
check has a negative control that shows it can fail ([attack it](#attack-it)). It reports what it measures on *your*
machine and contains no benchmark figures: latency is a property of a CPU, a core, a compiler and a host, never of a
library.

## Quick start

```
make check                                 # build, the 29-entry-point negative controls, then 10,000,000 frames per ring
make bench                                 # build, pin to the current CPU, print the report (CPU=3 chooses the core)
python3 -m pip install nanobind numpy      # once; a virtualenv is the safe place for it
make pybridge                              # build the module with CMake, run the 12-test validation suite
```

Each target exits non-zero if any check fails. Build products go under `./build`, and no target writes outside this
directory. The compiler is `clang++` if installed, otherwise `g++`; override with `CXX=`. Flags are
`-std=c++17 -O3 -march=native -Wall -Wextra -Werror -pedantic`. `ITERS=1000000` shortens the audit.

To use the bridge from your own code rather than from the tests: `PYTHONPATH=build/bridge python your_script.py`.

## Allocation interposition (`make check`)

**Claim.** While a thread has opted in, any heap call it makes aborts the process at the call site, with the entry point,
the byte count and a stack trace. While the measured window is open, the process as a whole makes no heap calls at all.

### The switch

```cpp
thread_local bool g_enforce_zero_alloc;    // external linkage, constant-initialized, trivially destructible
{ ZeroAllocScope hot; /* ... hot loop ... */ }   // sets it for the scope; scopes nest; AllocAllowedScope carves a hole
```

The flag is thread-local, so setup and warmup on any thread allocate freely and only threads that opt in are policed.
It is constant-initialized and trivially destructible, so reading it from inside `malloc` needs no lazy TLS setup (which
would itself allocate), and `tls_model("initial-exec")` keeps that true under `-fPIC`.

### Interposition, not wrapping

The malloc family and every C++17 `operator new` / `operator delete` form are *defined in the test executable*. The
dynamic linker therefore binds every caller in the process to them, including the calls libc and libstdc++ make
internally (`strdup`, `fopen`, `pthread_create`, `std::string` growth). Each wrapper reports to the guard and then
forwards to glibc's own `__libc_malloc`, `__libc_calloc`, `__libc_realloc`, `__libc_memalign`, `__libc_valloc`,
`__libc_pvalloc` and `__libc_free`, so allocation itself behaves as before; it is only observed.
`posix_memalign` and `aligned_alloc` validate alignment as the originals do, and `posix_memalign` leaves `errno` alone.

Link-time wrapping (`ld --wrap=malloc`) is the usual alternative. It redirects only references in the objects being
linked and cannot see calls made from inside `libc.so` or `libstdc++.so`, which is why it is not used. glibc removed
`__malloc_hook` in 2.34, so interposition is the supported route.

### The 29 entry points

| Group | Entry points | Count |
|---|---|---|
| C allocators | `malloc` `calloc` `realloc` `posix_memalign` `aligned_alloc` `memalign` `valloc` `pvalloc` | 8 |
| C deallocator | `free` | 1 |
| `operator new` | `new` `new[]` · `(nothrow)` ×2 · `(align_val_t)` ×2 · `(align_val_t, nothrow)` ×2 | 8 |
| `operator delete` | `delete` `delete[]` · `(size)` ×2 · `(align_val_t)` ×2 · `(size, align_val_t)` ×2 · `(nothrow)` ×2 · `(align_val_t, nothrow)` ×2 | 12 |

Releasing a non-null pointer is a heap event too, so a `std::vector` destructor in the hot path is caught as well as its
growth. `free(nullptr)` and `delete nullptr` are not events.

### Two layers on the window

1. **Per thread, at the call site.** An opted-in thread that reaches any of the 29 entry points reports it, and the
   process aborts with `ZERO-ALLOC VIOLATION: heap call '<entry>' (<n> bytes) on thread <tid>` and a backtrace. Frames
   print as `module(+offset)`; resolve one with `addr2line -f -C -e build/test_no_alloc <offset>`. `assert()` is not
   used: release builds define `NDEBUG`, which would compile the guard out.
2. **Process-wide, across the window.** Every entry point also increments one atomic counter, enforcing or not. The
   counter must not move between the instant the last worker is parked and the instant the last worker has left its hot
   loop. That catches a thread that never opted in (a logging helper, a runtime thread), and the last 32 entry-point
   names are kept so a failure says *which* call it was.

The window protocol is: workers park, `main` snapshots the counter and releases them, they run the loop inside a
`ZeroAllocScope`, they wait at an exit gate, `main` snapshots again and then lets them go. The exit gate is not
decoration: without it, `std::thread`'s state is deleted on the worker after its last hot call, and that `operator
delete(size)` lands inside the window.

What runs inside the window, with every frame verified on arrival:

- `animus::SpscRingBuffer<TelemetryPayload>`, 1 producer, 1 consumer, 10,000,000 frames: strict order and per-field
  integrity.
- `animus::sys::ipc::SpscQueue<TelemetryPayload>` (the queue the bridge below aliases), 1 producer, 1 consumer,
  10,000,000 frames, over a cache-line-aligned block the caller owns: strict order and per-field integrity.
- `animus::LockFreeRingBuffer<TelemetryPayload>` (the bounded MPMC ring), 2 producers, 2 consumers, 10,000,000 frames:
  per-field integrity, exact per-producer counts and index sums, and per-producer order as seen by each consumer.
- `Profiler::run()` from `bench_tsc.hpp` for 1,000,000 samples, so the profiler's own timed loop is proven
  allocation-free and cannot perturb what it measures.

### Why a clean run means something

A guard that never fires also reports "clean", so before the hot path is trusted the suite proves it can see:

- **Every entry point, in-process.** Each of the 29 is called with enforcement on and must be recorded exactly once,
  under its own name; with enforcement off it must record nothing. The check is by exact name, which is what exposed
  a probe the compiler had rewritten: `realloc(nullptr, n)` is turned into `malloc(n)` at compile time, so a literal
  null never reaches the `realloc` entry point. Probes route their pointers through `asm volatile` escapes and a
  volatile null, because a new/delete pair whose storage is never observed may legally be removed.
- **Every entry point, for real.** Each of the 29 is called in a forked child with the fatal guard and must die by
  `SIGABRT` with a diagnostic naming that call; one more child aborts from an opted-in worker thread.
- **Reach.** `strdup`, `fopen`/`fclose`, `std::string` growth, `std::vector` and `std::thread` creation, whose
  allocations happen inside libc and libstdc++, are caught.
- **Per-thread semantics.** An enforcing thread does not trip a free one and vice versa, scopes nest, and
  `AllocAllowedScope` permits heap use inside an enforced region.
- **Pass-through.** The wrappers keep the originals' behaviour: 16 to 4096-byte alignment for the aligned families,
  page alignment for `valloc` and `pvalloc`, `EINVAL` and an untouched `*memptr` on bad alignments, `calloc`
  zero-fill and overflow to `NULL`, `realloc` contents, `errno` after a successful `posix_memalign`.

### What it does not establish

- It is a dynamic check of the paths exercised in the run, not a proof over all inputs. A static audit of the compiled
  hot-path symbols is the complementary check.
- The MPMC integrity check (counts, index sums, order) catches any duplicated record that is paired with a lost one,
  unless several errors cancel exactly. It is not a proof of exactly-once delivery, and it is not what the audit is for.
- It sees heap API calls, not direct `mmap` / `brk` system calls or a private arena. The rings' storage (a
  `std::vector` inside each ring) is allocated before the window, by design.
- It audits a program that uses glibc's allocator. A binary linked against another allocator needs the forwarding
  targets changed; the 29-entry-point structure stays the same.
- Linux with glibc, dynamically linked. Static glibc, musl and macOS are rejected at compile time. Do not combine it
  with ASan, TSan or MSan, which interpose the allocator themselves (UBSan is fine).

## Hardware invariant TSC timing (`make bench`)

`bench_ring_tsc` runs six measurements on one pinned thread: an empty callable (the timer's own floor), a 1000 ns spin
by `CLOCK_MONOTONIC_RAW` (the self-check), `SpscRingBuffer` push+pop, `LockFreeRingBuffer` push+pop, one
`clock_gettime(CLOCK_MONOTONIC_RAW)` call for scale, and `SpscQueue` enqueue+dequeue (capacity 1,024, one 64-byte
record). Options: `--cpu N`, `--samples N` (default 1,000,000),
`--warmup N` (default 100,000).

### What one sample is

```
t0 = lfence ; rdtsc  ; lfence        timed code        t1 = rdtscp ; lfence        sample = t1 - t0
```

Each read is a single `asm volatile` statement with a memory clobber, so the compiler cannot place anything between
the fences. The leading `lfence` keeps earlier work out of the interval; the trailing one stops the timed code from
starting before the counter is read. `rdtscp` waits for all earlier instructions to execute, and the `lfence` after it
stops later instructions from starting early. `cpuid` is the older recipe; it costs hundreds of cycles and traps to the
host under a hypervisor. The `lfence`-serializes-`rdtsc` guarantee is architectural on Intel; AMD parts have it by
default or through an OS-set MSR depending on the generation, so confirm it on the part you deploy on.

The end read is `rdtscp` because it also returns `IA32_TSC_AUX`, which Linux sets to `node << 12 | cpu`. Comparing it
between consecutive samples counts CPU changes for free.

`rdtscp` does not wait for earlier *stores* to become globally visible. The interval measures execution, not the time
until a write is observed by another core, so a ring push whose stores are still in the store buffer at `t1` has been
timed up to that point and no further.

### Calibration against `CLOCK_MONOTONIC_RAW`

Ticks are converted to nanoseconds with a ratio measured at run time. The profiler spins on the pinned core for five 50 ms
windows, which doubles as warmup for the core (`run()` then also invokes the callable `--warmup` times untimed); each window
end reads the clock, then the TSC, then the clock again, and uses the midpoint of the two clock reads as the time of the
TSC read, so the cost of the read cancels. The ratio is the median over the windows
and the spread between windows is reported in ppm. `CLOCK_MONOTONIC_RAW` is read through the vDSO and is never
NTP-slewed, unlike `CLOCK_MONOTONIC` and `std::chrono::steady_clock`.

Ticks are TSC *reference* cycles: an invariant TSC ticks at a fixed rate whatever the core's P-state or C-state, so under
turbo a tick is longer than a core clock cycle. The nanosecond columns are the comparable ones. The profiler refuses to
run unless CPUID reports an invariant TSC (`80000007H:EDX[8]`) and RDTSCP (`80000001H:EDX[27]`).

Limits of the self-check, stated plainly: when the kernel clocksource is `tsc` (the report prints it), `CLOCK_MONOTONIC_RAW`
is itself derived from the TSC using the kernel's own calibration. The 1000 ns spin therefore validates the conversion
arithmetic, the serialization and the timer-cost subtraction; it is a consistency check, not an independent time
reference, and it cannot detect an error in the kernel's TSC frequency.

### Pinning

The constructing thread is pinned with `pthread_setaffinity_np` to one CPU (the one it is running on, or `--cpu N`),
verified with `sched_getcpu()` before and after the run, and its previous mask is restored on destruction. If the thread
cannot be pinned the profiler refuses to measure rather than measure a drifting thread. CPU changes between samples are
reported; a pinned run must show none.

### Percentiles and the timer floor

Percentiles are nearest-rank on the sorted samples: the smallest sample such that at least p% of all samples are less
than or equal to it. p99.9 is resolved only from 1000 samples up (below that it *is* the maximum), and the report says
so. The cost of an empty begin/end pair is measured at setup (median of 100,000 pairs) and subtracted in the "net"
columns, floored at 0; a net value near 0 means "below the resolution of this method", not "free". Raw columns are
printed alongside.

### Reading a result honestly

- Rows 3, 4 and 6 are *uncontended single-thread* costs of a push immediately followed by a pop of the same record. They say
  nothing about cross-core latency or throughput under contention.
- On a hybrid CPU (P and E cores) the answer depends on which kind of core you pin to; choose with `--cpu`.
- In a VM the guest-visible topology and TSC flags need not match the host, and a hypervisor may mask the invariant-TSC
  bit. To measure anyway, set `require_invariant_tsc = false` in `src/bench_ring_tsc.cpp`; ticks are then not guaranteed
  constant-rate.
- To measure cross-core latency, pin each thread to a different *physical* core, not an SMT sibling
  (`cat /sys/devices/system/cpu/cpuN/topology/thread_siblings_list`): siblings share an L1 and give hyperthread-local
  numbers.
- For tight tails, use the performance governor and an isolated core (`isolcpus`, `nohz_full`).

### Checks that set the exit status

Pinned and stayed pinned (by `sched_getcpu()` and `IA32_TSC_AUX`); calibration spread under 5000 ppm; the 1000 ns spin
reads between 990 and 1300 ns; ring operations returned the record that was pushed, every time; the percentile arithmetic
matches hand-worked answers for n = 1, 2, 10 and 1000; p99.9 flips from unresolved to resolved between 999 and 1000
samples; a profiler that cannot pin refuses to run; `ScopedPin` restores the original affinity mask; and, as a negative
control, the CPU-change detector reports a change for (nearly) every forced migration.

## Zero-copy nanobind Python bridge (`make pybridge`)

`TelemetryBuffer(capacity)` owns one cache-line-aligned block laid out as the engine's `SpscQueue<TelemetryPayload>`: a
header whose `head` and `tail` indices each own a cache line, then `capacity` 64-byte slots. The block is zeroed and
pre-faulted at construction, so Python never reads stale heap bytes and first-touch page faults stay out of the producer's
path. `slots()` returns the slot array as a NumPy array that *aliases* the C++ memory.

The engine's `SpscRingBuffer` keeps its storage private, so it cannot be aliased. `SpscQueue` operates on memory the caller
owns, which is exactly what makes a safe view possible.

### The silent copy

With nanobind's default return policy, an `ndarray` that has no owner is exported by *copying* it
(`nb_ndarray.cpp`: `copy = owner == nullptr && self == nullptr`). A "zero-copy" binding can quietly not be, and nothing
fails. The fix is one argument:

```cpp
.def("slots", &TelemetryBuffer::slots, nb::rv_policy::reference_internal, ...)   // owner = the Python TelemetryBuffer
using SlotView = nb::ndarray<nb::numpy, const std::uint64_t, nb::shape<-1, 8>>;   // const => NumPy marks it read-only
```

`reference_internal` attaches the Python buffer object as the array's owner, which both avoids the copy and keeps the block
alive for as long as any view of it, or any NumPy view derived from that view, exists. The test proves it by comparing data
pointers rather than assuming it.

### API

| Member | Meaning |
|---|---|
| `TelemetryBuffer(capacity)` | Power of two, at least 2. `ValueError` otherwise. |
| `slots()` | Read-only `(capacity, 8)` `uint64` array aliasing the slot memory. |
| `head`, `tail` | Records ever published / consumed (acquire loads). |
| `consume(n)` | Retire `n` published slots (release store). `ValueError` if `n > head - tail`. |
| `push(timestamp_cycles, event_id, trace_id, metric_value)` | Native-producer stand-in; `False` when full, nothing overwritten. |
| `produce(count, stall_timeout_ms=5000)` | Native producer loop with the GIL released; returns how many it published. Gives up if the buffer stays full for the timeout, so a dead consumer cannot wedge the process. |
| `capacity`, `nbytes`, `data_ptr` | Slot count, byte size, address of slot 0. |
| `SLOT_BYTES`, `SLOT_WORDS`, `SLOT_FIELDS` | Layout: `(name, numpy typestr, byte offset)` per field, little-endian. |

### The consumer contract

This is the engine's own SPSC protocol. The producer publishes slot `i` with a release store of `head`. Python is the
single consumer: read `head` first (an acquire load), then read slots `(i & (capacity - 1))` for `i` in `[tail, head)`.
Those slots are not being written. `consume(n)` retires `n` of them, **after which the producer may overwrite them at any
time**. The view does not know this: slots outside `[tail, head)`, and any slot after it has been consumed, are mid-write
or stale. One producer at a time: do not call `push()` while `produce()` is running.

```python
import threading, numpy as np
import animus_bridge as ab

cap = 1 << 12
buf = ab.TelemetryBuffer(cap)
slots = buf.slots()                                   # read-only (cap, 8) uint64, aliases the C++ block
assert slots.__array_interface__["data"][0] == buf.data_ptr   # same address: nothing was copied

total = 1_000_000
threading.Thread(target=buf.produce, args=(total,), daemon=True).start()   # native producer, GIL released

seen = checksum = 0
while seen < total:
    head = buf.head                                   # acquire: slots [seen, head) are published and stable
    n = head - seen
    if n == 0:
        continue
    i = seen & (cap - 1)
    first = min(n, cap - i)
    rows = slots[i:i + first] if first == n else np.concatenate((slots[i:], slots[:n - first]))
    checksum ^= int(np.bitwise_xor.reduce(rows[:, 2]))    # do the work on rows BEFORE consume()
    buf.consume(n)                                    # from here the producer may overwrite those slots
    seen = head
```

`SLOT_FIELDS` builds a structured dtype without the module importing NumPy:
`np.dtype({"names": [...], "formats": [...], "offsets": [...], "itemsize": ab.SLOT_BYTES})`, and `slots.view(dtype)`
stays zero-copy, as do the strided column views taken from it.

### What the tests prove

- **No copy:** the array's data pointer equals the C++ block's slot-0 address; a record published natively appears in an
  array obtained *before* the push; `tracemalloc` sees no allocation proportional to the 4 MiB array. Negative controls:
  a real `.copy()` has a different address and *is* seen by `tracemalloc`.
- **Lifetime:** the view and views derived from it keep the buffer alive after every reference to the
  `TelemetryBuffer` is gone, and release it afterwards (checked with a weak reference).
- **Read-only:** writing through the view raises.
- **Protocol:** backpressure, wrap-around onto a physical slot, rejected over-consumption leaves state unchanged.
- **Concurrency:** 2,000,000 records through a 4096-slot ring against a native producer thread. The ring is roughly 488
  times smaller than the stream, so the producer can only finish by overlapping with the consumer; every record is checked
  (timestamp equals position, event/trace halves, the metric hash). Reading slots *after* `consume()` fails this test
  hard, which is the hazard the contract above warns about.
- **Layout, argument validation, and the stall timeout** of `produce()`.

The twelfth test only prints the per-call cost of `slots()` for information and asserts nothing beyond a sanity bound; it
is a per-call figure for a view of the whole buffer, not a per-record or per-event one.

## One-line verification

| Toolchain | Command, from this directory |
|---|---|
| Clang 18+ | `make check bench pybridge CXX=clang++` (or `clang++-18`) |
| GCC 14+ | `make check bench pybridge CXX=g++` (or `g++-14`) |
| MSVC (bridge only) | `cmake -S . -B build\msvc && cmake --build build\msvc --config Release && python python\test_python_bridge.py` |

`pybridge` needs nanobind and NumPy in the Python the Makefile uses (`PYTHON=path/to/python` selects another); the Makefile
says so and stops if they are missing. The allocation audit and the TSC profiler are x86-64 Linux only and stop with an
`#error` elsewhere; on Windows only the bridge builds.

**Windows:** run the MSVC command with a Visual Studio generator and with the `python` on `PATH` being the one that has
nanobind and NumPy. Keep the checkout path short. The deepest file the build creates sits about 106 characters below the
package root, so the root should stay under roughly 150 characters. This reproduced: from a 161-character root (deepest
file about 267) CMake's compiler check failed with `FileTracker: error FTK1011: could not create the new file tracking log
file`; the identical command from a 61-character root (deepest about 167) built and passed.

### What was run

| Toolchain | Versions | Targets |
|---|---|---|
| Clang | 18.1.8, 19.1.7, 22.1.2 | `check`, `bench`, `pybridge` |
| GCC | 14.3.0, 15.2.0, 16 (pre-release snapshot 20260322) | `check`, `bench`, `pybridge` |
| MSVC | 19.51 (Visual Studio 18, toolset 14.51), x64 | `pybridge` |

All with `-Wall -Wextra -Werror -pedantic` (`/W4 /WX /permissive-` on MSVC) and zero warnings. Linux runs: x86-64, kernel
6.18 as a **WSL2 guest** (not bare metal), glibc 2.43, Python 3.14.4, NumPy 2.5.3, nanobind 3.0.1. Windows run: Python 3.14.5,
NumPy 2.5.2, nanobind 3.0.1. Older compilers and other Python or nanobind versions are untested. Because the Linux runs were
in a guest, the functional checks (allocation audit, bridge) are the portable result; any latency you quote should come from
your own host.

## Attack it

These are the defects the suite was developed against. Each is a one-line edit and each makes the suite fail:

| Edit | Expected result |
|---|---|
| In `run_spsc` (`src/test_no_alloc.cpp`), add `{ void* leak = ::operator new(24); escape(leak); }` to the producer loop | `make check` aborts: `ZERO-ALLOC VIOLATION: heap call 'operator new' (24 bytes)` plus a stack trace |
| Delete `guard::note("posix_memalign", n);` | Both negative-control lines fail and name `posix_memalign` |
| Change `std::atomic<bool> release{ false };` to `{ true }` | The window reports one `operator delete(size)` per worker: thread teardown inside the window |
| Drop `nb::rv_policy::reference_internal` from the `slots` binding | 8 of the 12 bridge tests fail: nanobind now copies |
| Drop `const` from the `SlotView` scalar type, and `const_cast` the pointer in `slots()` so it compiles | The read-only test fails |
| In the concurrent test's consumer, call `buf.consume(n)` before checking `rows` | A large share of the 2,000,000 records fail the integrity check: the producer has reused those slots |

## Layout and provenance

| Path | Role |
|---|---|
| `src/test_no_alloc.cpp` | Interposer, negative controls, hot-path windows (`make check`) |
| `src/bench_ring_tsc.cpp` | Profiler driver and self-checks (`make bench`) |
| `src/python_bridge.cpp` | The nanobind module |
| `python/test_python_bridge.py` | The bridge validation suite |
| `include/animus/bench_tsc.hpp` | The profiler: header-only and standalone |
| `include/animus/ring_buffers.hpp` | `TelemetryPayload`, `LockFreeRingBuffer`, `SpscRingBuffer`: extracted verbatim from the engine |
| `include/animus/spsc_queue.hpp` | `SpscQueue` over caller-owned memory: verbatim copy |
| `include/animus/aligned_block.hpp` | A 64-byte-aligned heap block to hold a `SpscQueue`; allocated before a measured window, freed after |
| `CMakeLists.txt`, `Makefile` | Bridge build; targets |
| `LICENSE` | Apache-2.0, covering every file here |

The ring types under test are not lookalikes. `ring_buffers.hpp` is cut mechanically from AnimusCore's single-header
engine (`animus.hpp`, SHA-256 `1dde92a2abd26e1163986df0a19c256713b12d4e26e9e6c2879ce96803e2e34d`): lines 58 to 75 and 334 to
534, byte for byte, inside the same namespace and wrapped only in a provenance banner, the includes it needs and an MSVC
padding-warning pragma. Its comments are verbatim too, so a few refer to parts of `animus.hpp` that are not shipped here.
`spsc_queue.hpp` is the engine's file unchanged (SHA-256 `acb2554504576b6a6bfcba79b6ea954b567a2ca38ccb5e22ceef2f904c141384`;
check with `sha256sum`). Both hashes are of the files with LF line endings, which is how git stores them; a checkout that
converts to CRLF hashes differently. The rest of the engine is not needed to build, run or verify anything here, so it is
not included.

## License and evaluation notice

Benchmark harnesses and nanobind interop examples are open-source under Apache-2.0. The AnimusCore high-performance
telemetry engine is proprietary commercial software licensed separately.

Everything in this repository is licensed under Apache-2.0 (`LICENSE`): the harness and test drivers, and the two
AnimusCore ring-buffer headers vendored so that the suite builds on its own, `include/animus/ring_buffers.hpp` and
`include/animus/spsc_queue.hpp`. Anyone may clone it and run `make check bench` without a separate agreement.

The rest of AnimusCore is not in this repository: the full engine, its transport layers and its enterprise modules
remain proprietary and are licensed separately.

Institutional licensing, evaluation support and procurement: **https://animusinfra.com**
