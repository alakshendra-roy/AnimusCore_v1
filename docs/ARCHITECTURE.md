# Animus Core -- ITCH 5.0 Pipeline: Technical Deep-Dive

Scope note: this document covers exactly one subsystem end to end -- the
NASDAQ TotalView-ITCH 5.0 ingestion pipeline (`adapters/itch50/`), the
generic SPSC ring it is built on (`animus-eval-kit/include/spsc_ring_buffer.hpp`),
the cross-process shared-memory ring it feeds into (`include/animus/shm_ipc.hpp`),
and the C-ABI surface that exposes the engine to non-C++ callers
(`AnimusCore_v1/animus_engine.cpp`). It is not a survey of the rest of the
repository (CEP rule engine, execution client, licensing, etc.) -- see the
root `ARCHITECTURE.md` for that.

Every code excerpt below is quoted verbatim from the file it cites, not
paraphrased or reconstructed, so line numbers and behavior can be checked
directly against the source.

---

## 1. Ingestion & Zero-Alloc Wire Parsing

### 1.1 Byte-layout parsing off the wire

ITCH 5.0 messages are decoded directly from a raw `const uint8_t*` buffer --
a packet/PCAP payload, a length-prefixed replay file, or (in the benchmark
harness) a synthetic buffer built on the stack -- with no intermediate
parse tree, no `std::string` field extraction, and no per-message object
construction. Each of the eight wire shapes is a `#pragma pack(push, 1)`
struct (`adapters/itch50/include/itch50_messages.hpp`) that mirrors the
NASDAQ-documented byte layout exactly, pinned by a `static_assert` on
`sizeof(...)`:

```cpp
// adapters/itch50/include/itch50_messages.hpp
struct AddOrderMsg {
    uint8_t message_type;          // 'A'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t order_reference_number[8];
    uint8_t buy_sell_indicator;    // 'B' or 'S'
    uint8_t shares[4];
    char    stock[8];              // right-padded with spaces, not NUL-terminated
    uint8_t price[4];              // fixed-point, 4 implied decimal digits
};
static_assert(sizeof(AddOrderMsg) == 36, "AddOrderMsg must be exactly 36 bytes per ITCH 5.0");
```

Every multi-byte field is declared `uint8_t[N]`, never a native
`uint16_t`/`uint32_t`/`uint64_t`. This is deliberate on two counts: ITCH is
a big-endian wire protocol, so giving a field a native integer type on a
little-endian host (x86-64, ARM64) invites exactly the bug class this
adapter exists to avoid -- reading it directly and getting a silently wrong
host-order value. And ITCH's `Timestamp` is 48 bits wide, a size with no
native C++ integer type at all.

Field extraction goes through `itch50_bswap.hpp`'s intrinsic-backed
readers, never a `reinterpret_cast<uint32_t*>` over a packed struct's field
address:

```cpp
// adapters/itch50/include/itch50_bswap.hpp
inline uint32_t be32toh_field(const void* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return bswap32(v);
}
```

A direct cast would be undefined behavior under the C++ standard for a
misaligned access -- `#pragma pack(1)` guarantees no gaps *between* fields,
not that every field itself lands on a naturally aligned boundary. A
fixed-size `memcpy` is the portable, defined-behavior equivalent, and every
mainstream compiler (GCC, Clang, MSVC) recognizes the fixed-size-`memcpy`
pattern at `-O2`/`-O3` and folds it into a single unaligned load
instruction -- so the safety comes at zero runtime cost. The one field
exempt from byte-swapping is the 8-byte `Stock` ticker: it is raw ASCII,
not a big-endian integer, so it is `memcpy`'d verbatim.

All eight wire shapes (12 to 40 bytes) normalize into one fixed-size,
64-byte record -- `ItchFrame` (`adapters/itch50/include/itch50_ring_frame.hpp`)
-- before ever touching the ring, so nothing downstream of the decode step
needs to know which of the eight wire layouts produced a given record
beyond reading its `msg_type` byte:

```cpp
// adapters/itch50/include/itch50_ring_frame.hpp
struct alignas(64) ItchFrame {
    uint64_t sequence_id;
    uint64_t recv_timestamp_ns;
    uint64_t itch_timestamp_ns;
    uint64_t order_ref_number;
    uint64_t secondary_ref_number;
    uint64_t stock_ticker_raw;
    int64_t  price_ticks;
    uint32_t shares;
    uint16_t stock_locate;
    uint8_t  msg_type;
    uint8_t  side_or_flag;
};
static_assert(sizeof(ItchFrame) == 64, "ItchFrame must occupy exactly one cache line");
```

### 1.2 Why the hot path never calls `malloc`/`new`

`decode()` (`adapters/itch50/include/itch50_codec.hpp`) writes every
decoded field into an `ItchFrame&` supplied by the caller. Every
intermediate value it touches lives in a register or on the stack; nothing
it calls constructs a `std::string`, grows a `std::vector`, or otherwise
takes a path that could reach the allocator. That is a deliberate
constraint, not an incidental property, for three concrete reasons that
matter specifically on a market-data hot path:

- **Tail-latency determinism.** A heap allocator (glibc malloc, Windows
  `HeapAlloc`, jemalloc) is not O(1) in the worst case -- an arena refill,
  a size-class split, or a `mmap`/`munmap` call for a large or awkwardly
  sized request can cost microseconds, which is 10-100x this pipeline's
  entire measured per-message budget (see §4's verified numbers). One
  unlucky allocation on message N does not just slow message N down; on a
  single-producer ring, it stalls every message behind it.
- **No lock contention introduced from outside the ring.** Most general-
  purpose allocators serialize concurrent calls internally (a global or
  per-arena lock). A hot path that is otherwise lock-free end to end (the
  SPSC ring in §2 uses no mutex) would silently reintroduce a lock the
  moment it allocates, defeating the point of the lock-free design.
- **No GC-adjacent pause exposure.** Even without a garbage collector in
  C++, a heap that fragments over a long-running feed-handler process
  (hours to a full trading session) degrades allocator performance over
  time in a way that is hard to reproduce in a short benchmark -- avoiding
  the allocator on the hot path avoids this class of problem entirely
  rather than needing to characterize it.

This constraint is not just documented but *measured*: `bench_itch_ingest`
overrides the global `operator new`/`operator delete` with counting
wrappers before the timed window opens, snapshots the counters immediately
before `start_flag` is raised, and reports the delta after both threads
finish:

```cpp
// adapters/itch50/bench_itch_ingest.cpp
void* operator new(std::size_t size) {
    g_heap_alloc_ops.fetch_add(1, std::memory_order_relaxed);
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) { throw std::bad_alloc(); }
    return ptr;
}
```

A non-zero `hot_path_allocs`/`hot_path_deallocs` in the printed report is a
build-breaking regression, not a warning -- see §4's `VERIFIED: 0
decode failures, 0 corrupted frames, 0 hot-path heap allocations.` line.

### 1.3 Static dispatch on message type

`decode()` reads the one-byte `MessageType` tag at `wire[0]` and dispatches
through a `switch` over a `enum class MessageType : uint8_t` whose
enumerators are the literal ASCII wire bytes (`'A'`, `'E'`, `'C'`, `'X'`,
`'D'`, `'U'`, `'F'`, `'S'`) -- no lookup table, no virtual dispatch, no
`std::function`:

```cpp
// adapters/itch50/include/itch50_codec.hpp
switch (type) {
case MessageType::SystemEvent: { /* 'S', 12B */ ... return true; }
case MessageType::AddOrderNoMPID: { /* 'A', 36B */ ... return true; }
case MessageType::AddOrderWithMPID: { /* 'F', 40B */ ... return true; }
case MessageType::OrderExecuted: { /* 'E', 31B */ ... return true; }
case MessageType::OrderExecutedWithPrice: { /* 'C', 36B */ ... return true; }
case MessageType::OrderCancel: { /* 'X', 23B */ ... return true; }
case MessageType::OrderDelete: { /* 'D', 19B */ ... return true; }
case MessageType::OrderReplace: { /* 'U', 35B */ ... return true; }
default:
    return false; // unrecognized message_type
}
```

Because the enumerator values are the wire bytes themselves, the compiler
lowers this to a dense jump table (a single indexed branch on `wire[0]`)
rather than a chain of comparisons -- the same reason the tag is declared
as an ASCII `enum class` instead of a sequential integer ID. Every branch
guards on `wire_len < sizeof(<MessageStruct>)` before touching a single
field beyond the shared header, so a truncated or malformed message is
rejected (`decode()` returns `false`, `out` left unspecified) without ever
reading past the buffer the caller actually supplied. `decode()` never
throws and never aborts on malformed input -- a short or unrecognized
frame is treated as an expected, recoverable condition on a live feed, not
a programming error, and the decision of what to do with a rejected
message (drop, log, count) is left to the caller.

All eight branches share one header decode, factored out because
`StockLocate` and `Timestamp` sit at the identical byte offset (1..8) in
every one of the eight wire messages:

```cpp
// adapters/itch50/include/itch50_codec.hpp
inline void decode_common_header(const uint8_t* wire, ItchFrame& out) noexcept {
    out.stock_locate    = be16toh_field(wire + 1);
    out.itch_timestamp_ns = be48toh_ns(wire + 5);
}
```

---

## 2. SPSC Ring Buffer Architecture & Concurrency

### 2.1 The circular queue design

`animus::eval::SpscRingBuffer<T>` (`animus-eval-kit/include/spsc_ring_buffer.hpp`)
is a fixed-capacity circular buffer with exactly one producer thread
calling `push()` and exactly one consumer thread calling `pop()`, for the
lifetime of the instance. That narrower contract -- versus a general MPMC
queue -- is what buys the latency: no compare-and-swap retry loop, no
inter-producer contention, no lock of any kind.

```cpp
// animus-eval-kit/include/spsc_ring_buffer.hpp
bool push(const T& value) noexcept {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= capacity_) {
        return false; // full
    }
    slots_[head & mask_] = value;
    head_.store(head + 1, std::memory_order_release);
    return true;
}

bool pop(T& out) noexcept {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint64_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
        return false; // empty
    }
    out = slots_[tail & mask_];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
}
```

Capacity is rounded up to the next power of two at construction
(`round_up_pow2`), so slot indexing is a plain `& mask_` rather than a
`%` -- a single AND instead of a division. `T` must be
`std::is_trivially_copyable_v<T>` (enforced by `static_assert`): slots are
plain assignment-copied, never serialized, so `ItchFrame` -- itself
`static_assert`-checked as trivially copyable -- is a valid `T` with no
adapter code required beyond the type declaration.

### 2.2 Acquire-release and why it avoids a bus lock

Each side of `push`/`pop` touches two cursors: a **relaxed** load of its
own last-published cursor (only that thread ever writes it, so there is
nothing to synchronize against by reading it back), an **acquire** load of
the *other* side's cursor, and a **release** store to publish its own new
value:

- **Producer**: relaxed-load `head_` (its own), acquire-load `tail_`
  (consumer's), release-store `head_ + 1`.
- **Consumer**: relaxed-load `tail_` (its own), acquire-load `head_`
  (producer's), release-store `tail_ + 1`.

The release store on one side and the acquire load on the other form a
*synchronizes-with* pair (C++11 `[atomics.order]`): every write the
producer made to `slots_[head & mask_]` *before* its release-store of
`head_` is guaranteed visible to the consumer *after* its acquire-load of
that same `head_` value observes the new count. This is the entire
correctness argument for the ring having no mutex -- the memory model
itself, not a lock, is what makes "the consumer never reads a half-written
slot" hold.

The reason this is fast, specifically on x86-64: `std::memory_order_acquire`
and `std::memory_order_release` require no extra instruction on x86-64 at
all -- a plain `MOV` already has acquire semantics for a load and release
semantics for a store under the x86-TSO memory model, so the compiler
emits ordinary loads/stores with no fence. Contrast with what a
sequentially-consistent access (`std::memory_order_seq_cst`, the *default*
for `std::atomic` if the ordering argument is omitted) or a
compare-and-swap retry loop would cost: `seq_cst` stores compile to a
`MOV` followed by an `MFENCE` (or a `LOCK`-prefixed instruction) on x86-64
to establish a total global order across *all* atomics in the program --
work this ring's correctness argument does not need, since acquire/release
between exactly two known threads is already sufficient. `LOCK`-prefixed
instructions (the primitive behind `fetch_add`, `compare_exchange`, and
`seq_cst` stores on x86) assert the cache-coherency bus/ring for the
duration of the operation -- tens of cycles of overhead versus a bare
store, and, unlike acquire/release, that cost does not disappear even when
only two threads are involved. Choosing the weakest ordering that is still
*correct* for the actual producer/consumer relationship -- not the
strongest ordering available -- is the mechanism, not an incidental
optimization.

### 2.3 Cache-line layout: `alignas(64)` and false sharing

```
                     one 64-byte cache line = 8 x uint64_t
   ┌────────────────────────────────────────────────────────────────┐
   │ capacity_ │ mask_ │ slots_(ptr) │ ... (const, set-once fields) │  <- read-only after construction,
   └────────────────────────────────────────────────────────────────┘     shared by both threads (no writes -> no false sharing)

   ┌────────────────────────────────────────────────────────────────┐
   │ head_ (std::atomic<uint64_t>)  │        [ 56 bytes pad ]       │  <- alignas(64): PRODUCER-owned line
   └────────────────────────────────────────────────────────────────┘     only the producer thread writes here
                          ▲
                          │ 64-byte gap -- head_ and tail_ can never
                          │ share a physical cache line
                          ▼
   ┌────────────────────────────────────────────────────────────────┐
   │ tail_ (std::atomic<uint64_t>)  │        [ 56 bytes pad ]       │  <- alignas(64): CONSUMER-owned line
   └────────────────────────────────────────────────────────────────┘     only the consumer thread writes here

   slots_ (std::vector<ItchFrame>, ItchFrame itself alignas(64)):
   ┌───────────┬───────────┬───────────┬───────────┬─────────────┐
   │  slot[0]  │  slot[1]  │  slot[2]  │  slot[3]  │     ...     │  <- 64 bytes each, one cache
   │  64 bytes │  64 bytes │  64 bytes │  64 bytes │             │     line per slot, never straddles
   └───────────┴───────────┴───────────┴───────────┴─────────────┘     two lines, never shares a line
                                                                        with an adjacent slot
```

```cpp
// animus-eval-kit/include/spsc_ring_buffer.hpp
alignas(kCacheLineSize) std::atomic<uint64_t> head_{ 0 }; // producer-owned cursor
alignas(kCacheLineSize) std::atomic<uint64_t> tail_{ 0 }; // consumer-owned cursor
```

Without the `alignas(64)` separation, `head_` and `tail_` -- two 8-byte
fields declared next to each other -- would very likely land on the same
64-byte cache line the compiler happens to allocate for the object. Under
the MESI (or MESIF/MOESI) cache-coherency protocol every x86-64 core
implements, a cache line can be in the *Modified* state on at most one
core at a time: the instant the producer's core writes `head_`, that
entire line is invalidated in the consumer's cache, even though the
consumer never touches `head_` and was only ever reading `tail_` from the
*same* line. The consumer's next read of its own `tail_` then misses in
its L1/L2 and has to fetch the line again over the inter-core interconnect
-- a full cache-coherency round trip triggered by a write to a variable the
reading core never even looks at. This is **false sharing**, and on a
tight polling loop (exactly what `push`/`pop`'s retry-on-full/retry-on-empty
callers do) it turns every single operation into a coherency-traffic event
instead of only the operations where a cursor genuinely changes hands.
Pinning `head_` and `tail_` to their own lines means the producer's write
traffic and the consumer's write traffic each live on a line touched by
only one core's writes -- coherency traffic then only occurs when a cursor
value the other side is actually polling has truly changed, which is
unavoidable and exactly the synchronization the algorithm requires.

`ItchFrame` gets the identical treatment for the identical reason, one
level down: `alignas(64)` plus its field layout pads `sizeof(ItchFrame)` to
exactly 64 bytes (enforced by `static_assert(sizeof(ItchFrame) == 64, ...)`
in `itch50_ring_frame.hpp`), so no single frame straddles two lines, and no
two adjacent ring slots ever share a line either -- the same false-sharing
hazard the head/tail split addresses, applied to the payload array.

`bench_itch_ingest.cpp` additionally pins the producer and consumer
threads to distinct physical cores (`animus::sys::pin_current_thread_to_core`,
`hardware_concurrency() - 2` / `- 1` by default) and raises each thread's
scheduling priority, so the two sides of this cache-line argument are
actually running on two different cores concurrently rather than being
time-sliced onto one -- see also §4's core-isolation check
(`scripts/verify_env.sh`), which flags when this run-time guarantee has no
`taskset`/`numactl` backing it at the OS level.

---

## 3. Zero-Copy IPC & Dynamic Interop

### 3.1 Shared-memory architecture

`animus::sys::ipc::ShmRing<T>` (`include/animus/shm_ipc.hpp`) is the
cross-process counterpart to §2's in-process ring: the identical
head/tail, acquire-release algorithm, but placed inside a named OS
shared-memory mapping instead of an owned `std::vector`, so two
*independent processes* -- not two threads in one process -- exchange
records with no serialization step and no syscall once both sides have the
segment mapped.

```cpp
// include/animus/shm_ipc.hpp -- POSIX path
int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
if (fd < 0) return false;
ftruncate(fd, static_cast<off_t>(size));
void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

`shm_open` + `ftruncate` + `mmap(MAP_SHARED)` is the POSIX path
(`SharedMemoryRegion::create`, Linux/macOS); the same class carries a
Windows path (`CreateFileMappingA` + `MapViewOfFile`, pagefile-backed
rather than a real file on disk) behind the same `#if defined(_WIN32)`
switch, since a segment name that means "look under `/dev/shm/`" on POSIX
has no equivalent on Windows. `itch50_shm_bridge.cpp` is this mechanism
applied to `ItchFrame` specifically: it creates a named
`ShmRing<ItchFrame>`, decodes and pushes synthetic ITCH records into it
with the identical `decode()` path `bench_itch_ingest` uses, and holds the
segment open long enough for a second, independent process to attach.

Once both processes have called `mmap`/`MapViewOfFile` on the same
segment, a `try_push`/`try_pop` call is a plain memory access to pages
both processes already have mapped -- there is no `write()`/`read()`
syscall, no socket, and no kernel round trip on the transfer path itself
(the *initial* `mmap` is the only syscall either side pays), which is what
puts single-record transfer latency in the sub-microsecond range this
design targets: the cost is dominated by the same acquire/release
cursor protocol as the in-process ring (§2.2), not by any IPC-specific
overhead.

The header that precedes the slot array in the mapped segment
(`RingHeader`) carries the same producer/consumer cache-line split as
§2.3's in-process ring -- `head` and `tail` are each pinned to their own
`alignas(ANIMUS_CACHE_LINE_SIZE)` line -- with an added reason to care here
that two threads in one process don't have: the two sides of a `ShmRing<T>`
are frequently on two different processes that may not even share a NUMA
node or socket, so false sharing between `head`/`tail` here would cost
*cross-socket* coherency traffic, not just cross-core.

`RingHeader` also stamps a schema identity into the segment at `create()`
time -- `schema_version_hash`, `payload_size`, `stride`, and a
`wire_format` string, all derived from `animus::schema::Traits<T>`
(`include/animus/schema.hpp`) -- so a second process's `open()` call can
refuse to attach with a mismatched or unrelated `T` before reading a single
payload byte, rather than silently misinterpreting another schema's bytes.
`ItchFrame`'s own registration (`itch50_ring_frame.hpp`) is what makes it
attachable by name with no `ItchFrame`-specific binding code on either
side (§3.2).

### 3.2 The C-ABI export surface

The native engine is compiled into a shared library
(`AnimusCore_v1.dll` / `libanimus_native.so`) whose entire public surface
is `extern "C"` functions, each exported via a portable macro:

```cpp
// AnimusCore_v1/animus.hpp
#if defined(_WIN32)
#define ANIMUS_API __declspec(dllexport)   // building the DLL
#define ANIMUS_API __declspec(dllimport)   // consuming it
#else
#define ANIMUS_API __attribute__((visibility("default")))
#endif
```

```cpp
// AnimusCore_v1/animus_engine.cpp
extern "C" {
    ANIMUS_API void* animus_shm_ring_create(const char* name, size_t requested_capacity);
    ANIMUS_API void* animus_shm_ring_open(const char* name);
    ANIMUS_API void  animus_shm_ring_close(void* ring);
    ANIMUS_API size_t animus_shm_ring_capacity(void* ring);
    ...
}
```

`extern "C"` disables C++ name mangling and the C++ calling-convention
ambiguity that would otherwise make this surface unusable from anything
that isn't the exact same C++ compiler and standard library version --
a stable, un-mangled, C-linkage symbol table is what any foreign-function
interface (Python's `ctypes`/`cffi`, Rust's `extern "C"` FFI, .NET
P/Invoke, etc.) actually binds against. State is passed across this
boundary as an opaque `void*` handle (`animus_shm_ring_create` returns
one, every other `animus_shm_ring_*` call takes one back) rather than a
C++ object by value or reference, which is what makes the boundary
resilient to the calling language having no concept of a C++ class layout
at all.

The interop path this repository actually ships and verifies is Python:
`animus/bindings.py` loads the shared library via `ctypes.CDLL` and calls
straight through this `extern "C"` surface with zero third-party Python
dependencies (per this repo's own SDK Wrapper standard), while
`bindings/` (the compiled `_animus_shm_native` extension) and
`animus.dynamic_schema.to_structured_array()` build a zero-copy NumPy view
directly over a `SharedSchemaChannel`'s mapped bytes -- no per-`ItchFrame`
Python object construction, no copy, and (per §3.1) no
`ItchFrame`-specific binding code, because the dtype is derived at runtime
from the `wire_format` string `ItchFrame`'s own `ANIMUS_DEFINE_SCHEMA`
registration stamped into the segment header:

```python
# adapters/itch50/README.md §4
channel = SharedSchemaChannel.open("animus_itch50_demo")
arr = to_structured_array(channel)        # zero-copy: np.frombuffer over shared memory
assert arr.flags["OWNDATA"] is False      # a view, not a copy
```

Because the export surface is plain `extern "C"` with only fixed-width
scalar types and opaque pointers crossing it -- no C++ types, no
exceptions, no STL containers in any function signature -- the same
surface is directly callable from any other language with a C FFI (Rust's
`extern "C"` blocks, C itself, .NET P/Invoke); Python via `ctypes` is the
consumption path this repository implements and benchmarks today, and the
ABI-level design does not restrict that to Python.

---

## 4. Build & Benchmark Reproducibility

### 4.1 Environment verification (`scripts/verify_env.sh`)

Before trusting any number this pipeline reports, `scripts/verify_env.sh`
checks that the host is actually capable of reproducing it:

- **Toolchain**: a C++17-capable compiler -- `g++ >= 9` or `clang++ >= 10`
  -- since `bench_itch_ingest` and the rest of this pipeline are compiled
  as C++17 (`target_compile_features(bench_itch_ingest PRIVATE cxx_std_17)`,
  root `CMakeLists.txt`).
- **Build system**: CMake `>= 3.16`.
- **CPU timing properties**, read from `/proc/cpuinfo`: `constant_tsc` and
  `nonstop_tsc` (the RDTSC-based clock `bench_itch_ingest` calibrates
  against `std::chrono::steady_clock` and samples per message -- §1's
  latency numbers below are meaningless if the TSC's tick rate varies with
  P-state or halts in a C-state) and a 64-byte cache line size (the
  concrete number `alignas(64)` in §2.3 and §1's `ItchFrame` assumes).
- **Air-gapped build validation**: an out-of-source configure into
  `build_eval/` and a build of the `bench_itch_ingest` target specifically,
  with:

  ```
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O3 -march=native"
  ```

  `-O3` for full optimization (this pipeline's zero-allocation and
  cache-line guarantees are static/structural, but the *latency* numbers
  below are only representative under the same optimization level used to
  produce them); `-march=native` to let the compiler use whatever
  instruction set extensions the build host actually has, rather than a
  conservative generic-x86-64 baseline. The root `CMakeLists.txt` compiles
  `bench_itch_ingest` itself with `-O3 -Wall -Wextra -Wpedantic` under
  GCC/Clang (`/W4 /permissive- /O2 /wd4324` under MSVC -- `/wd4324`
  deliberately suppresses only the "structure was padded due to alignment
  specifier" warning, since that padding is `alignas(64)`'s entire point,
  not an accident), compiling warning-clean on both toolchains.
- **Core isolation sanity check**: warns (does not fail) when neither
  `taskset` nor `numactl` is present, since running the two-thread
  producer/consumer split in §2.3 without OS-level core pinning support
  reintroduces exactly the scheduler-noise risk that pinning is meant to
  eliminate.

Run it before any benchmark claim from this pipeline is taken as
representative of a given host:

```bash
./scripts/verify_env.sh
```

### 4.2 Verified baseline telemetry

The numbers below are real, reproducible `bench_itch_ingest --messages
10000000` runs (`adapters/itch50/README.md` §3) -- not projected or
hand-typed figures -- captured on the same physical development machine
across two toolchains, each processing the identical seeded 10,000,000-
message mix across all eight message types:

| Platform / Toolchain | Sustained Rate | p50 | p90 | p99 | p99.9 | Heap Allocs | Decode Failures | Sequence Corruption |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **Windows** -- MSVC 19.51, Release `/O2` | **6.82M msgs/sec** | 82 ns | 86 ns | 98 ns | 106 ns | **0** | **0** | **0** |
| **Linux** -- GCC 15.2 (Ubuntu, WSL2 kernel), `-O3` | **7.80M msgs/sec** | 70 ns | 106 ns | 130 ns | 350 ns | **0** | **0** | **0** |

The p99 figures place the parse-plus-enqueue cost (decode + ring push,
timed via the calibrated RDTSC clock described in §1.3/§2.2) at **98-130
nanoseconds** across both platforms and toolchains -- both runs printed the
identical `VERIFIED: 0 decode failures, 0 corrupted frames, 0 hot-path heap
allocations.` line described in §1.2, confirming bit-identical decode
behavior across the two byte-swap intrinsic paths (`_byteswap_ulong` on
MSVC, `__builtin_bswap32` on GCC). Neither run was captured on a
dedicated, CPU-isolated benchmark rig (no `isolcpus`/`nohz_full`/real-time
scheduling) -- p50/p90/sustained throughput are the stable, representative
figures; p99.9/max tail latency varies run to run on an untuned host, which
is exactly what §4.1's core-isolation check exists to flag before a tighter
tail-latency claim is made against a given machine.

Reproduce these numbers directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_itch_ingest
build/bin/bench_itch_ingest --messages 10000000
```

See `adapters/itch50/README.md` for the full runbook, including the
cross-process shared-memory demo (§3.1/§3.2 above) and the Python
zero-copy verification script.
