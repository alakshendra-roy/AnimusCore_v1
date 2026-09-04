# Animus Core v1.2.0 — System Architecture

Audience: engineers evaluating Animus for production integration into a
low-latency ingestion, market-data, or telemetry-fan-out path. This
document describes the shared-memory IPC and observability subsystem as
implemented in `include/animus/{shm_ipc,shm_lifecycle,schema,telemetry,
thread_affinity}.hpp`, `bindings/animus_shm_py.cpp`, `animus/*.py`, and
`scripts/animus_stat.py`. Every claim below is traceable to a specific
file and, where useful, a line-level construct in this repository as of
the v1.2.0 tag — nothing here is aspirational.

Scope note: Animus ships two distinct IPC primitives that are easy to
conflate:

- `animus::SpscRingBuffer` / `animus::SharedTelemetryChannel` (`animus.hpp`)
  — the original, wire-compatible-with-pure-Python primitive
  (`animus/shm.py`), deliberately **not** cache-line-padded so its byte
  layout matches the stdlib-only Python side exactly.
- `animus::sys::ipc::ShmRing<T>` / `SpmcRing<T>` (`include/animus/shm_ipc.hpp`)
  — the Milestone 1/2/3 generic, cache-line-padded, schema-registered ring
  for arbitrary trivially-copyable `T`, built for raw producer/consumer
  latency between two native processes. This document is primarily about
  this second primitive, since it is the one with the dynamic-schema,
  SPMC, telemetry, and NumPy-interop surface an evaluator will actually
  integrate against.

---

## 1. High-Level Memory Layout & IPC Topography

### 1.1 Shared memory primitives

`animus::sys::ipc::SharedMemoryRegion` (`shm_ipc.hpp:73`) is the sole OS
abstraction layer. Two backends, selected at compile time:

| | Linux / POSIX | Windows |
|---|---|---|
| Allocate | `shm_open(O_CREAT\|O_RDWR\|O_EXCL)` → `ftruncate` → `mmap(MAP_SHARED)` | `CreateFileMappingA(INVALID_HANDLE_VALUE, ...)` (pagefile-backed, not a disk file) → `MapViewOfFile` |
| Attach | `shm_open(O_RDWR)` → `fstat` for size → `mmap` | `OpenFileMappingA` → `MapViewOfFile(size=0)` → `VirtualQuery` for size (MapViewOfFile has no size return) |
| Destroy | `shm_unlink` (POSIX object destroyed once unlinked and all mappings closed) | No explicit unlink API for named file mappings — the kernel object is reference-counted and destroyed automatically once the last `HANDLE` across every attached process closes; `SharedMemoryRegion::unlink()` is a documented no-op on this platform |

Both `create()` paths refuse a name collision outright (`EEXIST` / Windows
`ERROR_ALREADY_EXISTS`) rather than silently attaching to a possibly
differently-sized, possibly in-use segment. On the POSIX attach path, the
implementation specifically uses `shm_open()`, not the raw `::open()`
syscall — `shm_open()` resolves a name through the shared-memory namespace
(`/dev/shm/<name>`) the same way `create()`'s own `shm_open()` call did;
`::open()` on a bare name instead performs an ordinary filesystem lookup
relative to the current working directory, which is never where `create()`
put it. This exact bug was caught on Linux CI, not by inspection — it is
the kind of platform-parity defect this abstraction exists to centralize
and fix once.

### 1.2 Cache-line alignment

`thread_affinity.hpp` defines `ANIMUS_CACHE_LINE_SIZE` — `64` on x86/x64
and most 32/64-bit ARM, `128` on `__aarch64__`/`_M_ARM64` targets (Apple
Silicon and Neoverse-class server ARM use a 128-byte physical line; using
64 there would under-pad and reintroduce false sharing). `RingHeader` and
`SpmcRingHeader` are both `alignas(ANIMUS_CACHE_LINE_SIZE)` and each field
group that crosses a producer/consumer ownership boundary gets its own
explicit `alignas(ANIMUS_CACHE_LINE_SIZE)` restart:

```cpp
// include/animus/shm_ipc.hpp:305
struct alignas(ANIMUS_CACHE_LINE_SIZE) RingHeader {
    static constexpr size_t kWireFormatBufSize = 80;

    uint64_t capacity = 0;
    uint64_t mask = 0;
    uint64_t ring_kind = static_cast<uint64_t>(RingKind::Spsc);
    uint64_t schema_version_hash = 0;
    uint64_t payload_size = 0;
    uint64_t stride = 0;
    char wire_format[kWireFormatBufSize] = {};   // 48 + 80 = 128 bytes = 2 lines

    alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> head{ 0 };
    std::atomic<uint64_t> dropped_count{ 0 };
    std::atomic<uint64_t> producer_pid{ 0 };
    std::atomic<uint64_t> producer_heartbeat{ 0 };   // producer-owned line

    alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> tail{ 0 };
    std::atomic<uint64_t> consumer_pid{ 0 };
    std::atomic<uint64_t> consumer_heartbeat{ 0 };   // consumer-owned line
};
```

Two `static_assert`s enforce the design invariant at compile time rather
than leaving it to convention:

```cpp
static_assert(std::atomic<uint64_t>::is_always_lock_free, /* ... */);
static_assert(sizeof(RingHeader) % ANIMUS_CACHE_LINE_SIZE == 0, /* ... */);
```

The lock-free assertion is not cosmetic: `RingHeader` lives in memory
shared *across process boundaries*. A non-lock-free `std::atomic<uint64_t>`
on some exotic target would fall back to a libatomic-managed mutex/futex
keyed by address — a synchronization primitive that is not valid to share
this way across independently-mapped virtual address spaces. MSVC's
`/W4`-triggered `C4324` ("structure was padded due to alignment
specifier") is explicitly suppressed on this struct with a documented
`#pragma warning(disable: 4324)` — the padding is the entire point, not
an accident the warning should be flagging.

Rationale for padding *at all*: producer and consumer here are two
different OS processes, potentially on two different physical cores or
even sockets. False sharing between `head` and `tail` in that
configuration is not merely an L1-contention cost the way it would be for
two threads on the same core-complex — it is cross-socket cache-coherency
(MESI/MOESI) traffic on every write to either cursor. This is the
specific reason `ShmRing<T>` pads where `animus.hpp`'s
`SharedTelemetryChannel` (the wire-compatible-with-pure-Python primitive)
deliberately does **not**: that struct's byte layout must match a
`struct.pack`-decoded Python reader field-for-field, and padding would
break that contract for no latency benefit in a context where the Python
side is inherently not a hot-path peer.

### 1.3 Ring buffer topology

Both ring kinds share an identical 128-byte read-only wire-descriptor
prefix — `capacity, mask, ring_kind, schema_version_hash, payload_size,
stride, wire_format[80]` — written once at `create()` and never mutated
again. This is a deliberate design choice, not an accident of struct
layout: it lets a process that has never heard of `T` (a monitoring
tool, `scripts/animus_stat.py`, or the schema-agnostic Python binding)
read `ring_kind` and the rest of the descriptor through *either* header
type before it knows which one a given segment actually is (see
`animus::sys::ipc::RingKind`, `shm_ipc.hpp:268`, and
`telemetry.hpp:147-157`).

**SPSC (`ShmRing<T>`, `RingKind::Spsc`) — 256 bytes on a 64-byte-line
target:**

```
Offset   Line   Field                          Owner
0        1-2    capacity, mask, ring_kind,      read-only (write-once)
                 schema_version_hash,
                 payload_size, stride,
                 wire_format[80]
128      3      head, dropped_count,            producer-owned
                 producer_pid, producer_heartbeat
192      4      tail, consumer_pid,              consumer-owned
                 consumer_heartbeat
256+     —       T slots[capacity]                shared, mask-indexed
```

**SPMC (`SpmcRing<T>`, `RingKind::SpmcBroadcast`) — 192 bytes:**

```
Offset   Line   Field                          Owner
0        1-2    (identical prefix)              read-only
128      3      head, producer_pid,             producer-owned (only line after the prefix)
                 producer_heartbeat
192+     —       T slots[capacity]                shared, mask-indexed
```

The SPMC header has no shared tail and no `dropped_count` at all — see
§2.3. `capacity` is always rounded up to a power of two at `create()`
(`round_up_pow2`), so slot indexing throughout is `index & mask`, never a
modulo. `T` must be trivially copyable and `alignof(T) <= 64`
(`static_assert`ed in both `ShmRing<T>` and `SpmcRing<T>`): records are
placed directly in raw shared memory with no serialization step, so a
byte-copy must be a complete, valid copy, and slots are packed
back-to-back with no per-slot padding beyond `T`'s own size — an
over-aligned `T` could violate the "every slot after the first is validly
aligned" guarantee that packing relies on.

`open()` on both ring types is a hard structural gate, not a best-effort
attach: it rejects a segment smaller than the header, a non-power-of-two
or inconsistent `capacity`/`mask`, a `ring_kind` mismatch (attaching
`ShmRing<T>::open` to a segment created by `SpmcRing<T>::create`, or vice
versa, fails outright rather than silently misreading a
differently-sized/shaped header), a `payload_size` mismatch, and a
`schema_version_hash` mismatch (§3.1). All four checks happen before a
single payload byte is read.

---

## 2. Concurrency Model & Memory Ordering

### 2.1 `std::atomic<uint64_t>` cursors

Every cursor (`head`, `tail`, `dropped_count`, `producer_heartbeat`, …) is
a plain `std::atomic<uint64_t>`, monotonically incrementing and never
reset. There is no compare-exchange retry loop anywhere in the SPSC or
SPMC hot path — by construction there is exactly one writer for `head`
(the producer) and, for `ShmRing<T>`, exactly one writer for `tail` (the
one consumer). A CAS loop exists to arbitrate between *multiple*
concurrent writers to the same location; here that condition never
arises, so a plain relaxed-then-release store suffices and is strictly
cheaper.

### 2.2 Store-release / load-acquire, not a full fence

`ShmRing<T>::try_push` (`shm_ipc.hpp:497`):

```cpp
bool try_push(const T& value) noexcept {
    const uint64_t head = header_->head.load(std::memory_order_relaxed);   // my own last write, uncontended
    const uint64_t tail = header_->tail.load(std::memory_order_acquire);   // the OTHER side's cursor
    if (head - tail >= header_->capacity) return false;
    slots_[head & header_->mask] = value;
    header_->head.store(head + 1, std::memory_order_release);
    return true;
}
```

`ShmRing<T>::try_pop` (`shm_ipc.hpp:554`):

```cpp
bool try_pop(T& out) noexcept {
    const uint64_t tail = header_->tail.load(std::memory_order_relaxed);
    const uint64_t head = header_->head.load(std::memory_order_acquire);
    if (tail == head) return false;
    out = slots_[tail & header_->mask];
    header_->tail.store(tail + 1, std::memory_order_release);
    return true;
}
```

The correctness argument is the standard release/acquire handoff, applied
across a process boundary rather than across threads in one address
space: the producer's `slots_[head & mask] = value` write is
program-order-before its `head.store(..., release)`. The consumer's
`head.load(..., acquire)` that observes that new `head` value is
therefore guaranteed — by the C++ memory model's release-acquire
synchronizes-with relation, which x86-64's TSO and ARM's weaker model
both implement via ordinary store-with-release-semantics /
load-with-acquire-semantics instructions, not a `mfence`/`dmb sy` full
barrier — to also observe the slot write that preceded it. No hardware
full fence is issued on either side: on x86-64 a release store is a plain
`MOV` (TSO already provides the needed ordering) and an acquire load is
also a plain `MOV`; on ARM64 the compiler emits `stlr`/`ldar` rather than
`dmb`. This is precisely why the header comment on the SPSC algorithm
describes it as "no compare-exchange retry loop... no hardware full
fence": the cost model here is "two ordinary loads/stores plus whatever
the ISA's native release/acquire encoding costs," not "a bus-locking
fence on every push/pop."

The `head - tail` fullness check is safe under unsigned wraparound by
construction: both cursors are monotonically increasing `uint64_t`s that
only ever move forward, so `head - tail` computed with wraparound
arithmetic yields the correct pending-count even after either counter
wraps (at 2^64 events, not a practically reachable bound at any sustained
throughput this system targets).

### 2.3 SPSC vs. SPMC isolation — how a slow consumer cannot block the writer

**SPSC bounded mode (`try_push`/`push_spin`).** A full ring simply
refuses the write and returns `false`. This is bounded backpressure: the
producer decides what "full" means to its own control flow — retry,
spill elsewhere, or count a hard drop at the call site. The producer is
never blocked by the OS scheduler; `push_spin` bounds even its own
spin-retry by `max_spins` (default `200'000'000`) and returns `false`
rather than looping forever against a peer that never shows up.

**SPSC decoupled/overwrite mode (`push_overwrite`,
`shm_ipc.hpp:528`).** This is the actual "producer must never be slowed
by a lagging consumer" contract:

```cpp
void push_overwrite(const T& value) noexcept {
    const uint64_t head = header_->head.load(std::memory_order_relaxed);
    const uint64_t tail = header_->tail.load(std::memory_order_acquire);
    if (head - tail >= header_->capacity) {
        header_->tail.fetch_add(1, std::memory_order_acq_rel);   // forcibly reclaim the oldest slot
        header_->dropped_count.fetch_add(1, std::memory_order_relaxed);
    }
    slots_[head & header_->mask] = value;
    header_->head.store(head + 1, std::memory_order_release);
}
```

On a full ring the producer forcibly advances `tail` itself
(`fetch_add`, not a plain store, so this remains correct even if the
consumer's own `try_pop` concurrently advanced `tail` past the same
value) and increments `dropped_count` — a deterministic, always-visible
loss counter, not a silent drop. This is documented as a deliberate race,
not an oversight: reclaiming a slot the consumer may be mid-read on can
produce a torn record at the reclaim boundary, since the consumer's
acquire-load of `head` and relaxed-load of `tail` give it no way to
distinguish "genuinely empty" from "just overwritten out from under me."
The API surface makes this an explicit choice at the call site —
`try_push`/`push_spin` for a channel that must never observe a torn
record, `push_overwrite` only for a channel that tolerates one (telemetry,
sampling, market-data snapshots where the newest value dominates).

**SPMC broadcast mode (`SpmcRing<T>::broadcast`/`poll`,
`shm_ipc.hpp:951` / `:1006`).** There is no shared tail at all — this is
the structural, not just behavioral, difference from the SPSC ring. Every
consumer's read position (`local_tail_`) lives only in that consumer's
own process memory and is never written back to shared memory:

```cpp
void broadcast(const T& value) noexcept {
    slots_[producer_head_ & header_->mask] = value;
    header_->head.store(producer_head_ + 1, std::memory_order_release);
    ++producer_head_;
}
```

`broadcast()` never checks any consumer's position — it cannot, since it
has no way to enumerate how many consumers exist or where each one is.
This is what makes N independent consumers truly independent: there is
no cursor for them to contend over, so a stalled or dead consumer's
`local_tail_` simply falls further behind `head` with zero effect on the
producer's write rate or on any other consumer's read rate. `poll()`
detects and self-corrects an overrun locally:

```cpp
size_t poll(T* out, size_t max_count) noexcept {
    const uint64_t head = header_->head.load(std::memory_order_acquire);
    if (head - local_tail_ > header_->capacity) {
        const uint64_t new_tail = head - header_->capacity;
        overrun_count_ += (new_tail - local_tail_);
        local_tail_ = new_tail;
        last_poll_overran_ = true;
    }
    // copy [local_tail_, head) into out, up to max_count
}
```

A consumer that has fallen more than `capacity()` slots behind `head`
jumps its own `local_tail_` forward to `head - capacity()` and accounts
the skipped span into `overrun_count()` — the "detect the overrun,
calculate dropped ticks, jump forward" contract, entirely local to that
one consumer instance. A consumer that never falls behind never pays this
branch beyond one comparison. The same torn-read caveat as
`push_overwrite` applies at the correction boundary (a single `poll()`
call's own duration bounds the exposure, not the consumer's total lag) —
documented in the class comment as the same accepted tradeoff, not a new
one.

### 2.4 `memory_order_relaxed` where it is actually sufficient

Heartbeat counters (`producer_heartbeat()`/`consumer_heartbeat()`) use
`fetch_add(1, std::memory_order_relaxed)` — they gate no subsequent
payload read on either side, so there is nothing for an acquire/release
pairing to protect; only monotonic progress matters, which relaxed
already guarantees. `dropped_count` increments are relaxed for the same
reason. Contrast this with `head`/`tail` themselves, which *do* gate a
payload array read on the other side and therefore require release/
acquire, not relaxed.

---

## 3. Zero-Copy Dynamic Wire Schemas & Python Interop

### 3.1 Compile-time schema registration (`include/animus/schema.hpp`)

`ANIMUS_DEFINE_SCHEMA(TypeName, WireFormatString)` specializes
`animus::schema::Traits<TypeName>` with three constants: `kName`,
`kWireFormat` (a `struct.calcsize`-compatible format string, e.g.
`"<QQqqII"`), and `kVersionHash` — a `constexpr` FNV-1a hash folded over
`{name, wire_format, sizeof(T)}` at compile time:

```cpp
constexpr uint64_t schema_version_hash(const char* name, const char* wire_format, uint64_t payload_size) noexcept {
    return fnv1a_mix_u64(fnv1a(wire_format, fnv1a(name)), payload_size);
}
```

`ShmRing<T>::create()` stamps `Traits<T>::kVersionHash`, `sizeof(T)`, and
`kWireFormat` into the header at allocation time; `open()` recomputes
`Traits<T>::kVersionHash` for whatever `T` the *attaching* translation
unit was compiled against and refuses to attach on any mismatch. This
closes a real gap that predates Milestone 1: two distinct
same-size trivially-copyable types could previously attach to the same
segment and silently misinterpret each other's fields, with the only
prior check (`payload_size`) unable to catch it. A `T` with no
`ANIMUS_DEFINE_SCHEMA` call still works (the primary `Traits<T>` template
falls back to a hash derived from `sizeof(T)` alone), preserving
attach-by-size-only for legacy record types that predate this milestone.

### 3.2 Python inspects the shared buffer directly — no `Py_BuildValue`

`RawSchemaView` (`shm_ipc.hpp:699`) is the schema-agnostic C++ half: it
opens a segment and reads `payload_size`/`stride`/`schema_version_hash`/
`wire_format` from the identical-offset prefix without ever naming `T`.
`bindings/animus_shm_py.cpp`'s `SharedSchemaChannel` wraps it and exposes
`raw_view()`:

```cpp
nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> raw_view() {
    return nb::ndarray<uint8_t, nb::memview, nb::ndim<2>>(
        reinterpret_cast<uint8_t*>(view_->slots_base()),
        {view_->capacity(), view_->stride()},
        nb::find(*this)
    );
}
```

This constructs an `nb::ndarray` view directly over `slots_base()` — a
raw pointer into the live mmap'd segment — with no intermediate copy and
no per-field `Py_BuildValue`/`PyLong_FromLongLong` marshalling. The
`nb::find(*this)` owner argument ties the view's lifetime to the
`SharedSchemaChannel` object rather than to a copy, so the returned
buffer is a live alias: valid exactly as long as the channel is alive,
and only `[tail(), head())` mod `capacity()` is data an active producer
has actually published and not yet overwritten — the same reader contract
the typed `ShmRing<T>::try_pop` enforces internally, just left for the
caller to apply explicitly here since there is no typed `pop()` on this
schema-agnostic path.

### 3.3 NumPy Buffer Protocol / `memoryview` → structured dtype

`animus/dynamic_schema.py`'s `wire_format_to_dtype()` parses the
`struct.calcsize` format string into a real NumPy structured `dtype` at
runtime:

```python
_STRUCT_TO_NUMPY = {
    "b": "i1", "B": "u1", "h": "i2", "H": "u2",
    "i": "i4", "I": "u4", "l": "i4", "L": "u4",
    "q": "i8", "Q": "u8", "f": "f4", "d": "f8",
}
```

Field names are not part of a `struct.calcsize` format string, so the
resulting dtype uses generic `f0, f1, ...` names in declaration order —
matching the C++ struct's own field order by construction, since
`Traits<T>::kWireFormat` is always hand-written to mirror it.
`to_structured_array()` then does:

```python
dtype = wire_format_to_dtype(channel.wire_format)
return np.frombuffer(channel.raw_view(), dtype=dtype)
```

`np.frombuffer` on a buffer-protocol object (the `nb::ndarray` from §3.2)
performs **no copy**: it reinterprets the existing memoryview's bytes
in-place as the structured dtype. A live model-ingestion loop reading
`ExecutionEvent` or a client-registered `OrderBookL2`/custom schema
therefore touches the shared segment exactly once per element — the same
one memcpy-equivalent any ring `pop()` already costs — never a Python
object-per-field deserialization pass. `to_structured_array()` validates
`dtype.itemsize == channel.stride` before returning, surfacing a
padding/stale-format-string mismatch as a `ValueError` rather than a
silently misaligned view.

The compiled-`T` fast path (`SharedExecutionChannel.poll()`/`.drain()`,
`bindings/animus_shm_py.cpp:166`) follows the identical zero-copy pattern
for the primary `ExecutionEvent` schema, batching into a pre-sized
`std::vector<WireRecord> scratch_` and returning an `(n, sizeof(T))`
`uint8` `ndarray` view over it — the buffer-lifetime contract is
identical: valid only until the next `poll()`/`drain()` call on that same
channel object.

### 3.4 GIL isolation

The hot spin-wait inside `poll()` runs entirely inside a
`nb::gil_scoped_release` block:

```cpp
nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> poll(size_t max_count, uint64_t max_spins) {
    size_t n = 0;
    {
        nb::gil_scoped_release release;   // no Python API touched in here
        WireRecord rec;
        while (n < limit && ring_->pop_spin(rec, max_spins)) {
            scratch_[n++] = rec;
        }
    }
    return nb::ndarray<uint8_t, nb::memview, nb::ndim<2>>(/* ... */, nb::find(*this));
}
```

The GIL is reacquired implicitly when the scope ends, *before*
constructing the returned `ndarray` — `nb::find()` is itself a Python-API
call and must run under the GIL. This means a producer running in a
different OS process, and lagging or momentarily absent, blocks only the
calling Python thread's own `poll()` — never the interpreter's other
threads, since the GIL is released for the full duration of the native
spin-wait. `push()`/`push_overwrite()`/`broadcast()` are not spin/blocking
calls (the underlying `try_push`/`push_overwrite`/`broadcast` never wait),
so they intentionally do **not** release the GIL — doing so would only
add overhead around an already-O(1) call. `SpmcConsumerChannel.poll()`
follows the identical convention.

On the pure-C++ side, the producer/ingestion loop has no GIL dependency
whatsoever — `ShmRing<T>`/`SpmcRing<T>` are plain C++ templates with no
Python runtime linkage; GIL isolation is purely a property of the Python
*binding* layer around them, not something the ring itself needs to know
about.

---

## 4. Telemetry & Observability Architecture

### 4.1 Lock-free, relaxed-order sampling (`include/animus/telemetry.hpp`)

`AnimusGetMetrics()` is a read-only diagnostic sampler that never attaches
as a consumer — no cursor registration, no pid/heartbeat writes, and
critically, not even the same-`T` schema check `ShmRing<T>::open()`
enforces, since a monitoring tool by definition has to work against a
ring whose exact record type it was never compiled against:

```cpp
inline bool AnimusGetMetrics(const char* ring_name, TelemetrySnapshot* out) noexcept {
    // opens the segment, reads ring_kind through the shared prefix,
    // then reads either RingHeader or SpmcRingHeader's fields with
    // std::memory_order_relaxed throughout, and closes the mapping
    // again before returning.
}
```

Every field this call samples is already a plain load of a value the
producer (and, for the SPSC ring, its one consumer) publishes via
`memory_order_release` regardless of whether anyone is watching.
`AnimusGetMetrics()` adds no synchronization of its own on top of that,
so it introduces zero contention on the producer's or consumer's own
atomic operations, no matter how many processes call it concurrently or
how frequently. `memory_order_relaxed` is correct here specifically
*because* nothing in this file uses a sampled value to gate a subsequent
read of the `T` payload array — there is no `slots_[...]` access anywhere
in `telemetry.hpp`, only numbers read for display, so there is no
acquire/release pairing to uphold and a relaxed load is both correct and
strictly cheaper (no memory fence, no cross-core cache-coherency stall)
than the acquire loads the ring's own consumer-facing accessors use.

`TelemetrySnapshot` exposes `ring_kind`, `capacity`, `schema_version_hash`,
`payload_size`, `stride`, `wire_format`, `current_write_head` /
`total_pushed_events` (the same monotonic `head` counter, exposed under
both names), and — SPSC-only — `dropped_events`/`total_overruns` and
`consumer_lag = head - tail`. For an SPMC ring, `has_dropped_count` and
`has_consumer_lag` are both `false`: overruns and lag are inherently
per-consumer, local-only concepts in broadcast mode (§2.3), unobservable
from outside any one specific consumer, so an unattached sampler
correctly reports "not applicable" rather than fabricating a number.
`producer_alive` is computed at sample time via `sys::lifecycle::
is_process_alive()` (§5.2) — a genuine OS liveness check, not a cached or
stale flag.

### 4.2 Out-of-band Prometheus / OpenMetrics scraping

`scripts/animus_stat.py --prometheus` is a **standalone, zero-dependency
(stdlib-only) Python mirror** of `AnimusGetMetrics()`'s exact wire-layout
contract — not a wrapper around the compiled extension, so it works even
where the nanobind module isn't built:

```python
_PREFIX_FORMAT = "<QQQQQQ"          # capacity, mask, ring_kind, schema_version_hash, payload_size, stride
_SPSC_HEAD_OFF, _SPSC_DROPPED_COUNT_OFF, _SPSC_PRODUCER_PID_OFF, ... = 128, 136, 144, ...
_SPSC_TAIL_OFF, _SPSC_HEADER_SIZE = 192, 256
_SPMC_HEAD_OFF, _SPMC_PRODUCER_PID_OFF, _SPMC_HEADER_SIZE = 128, 136, 192
```

These offsets are asserted to match `RingHeader`/`SpmcRingHeader` byte for
byte (`tests/test_animus_stat.py` is what actually catches drift if the
C++ layout ever changes — the comment in the script is documentation,
not enforcement). Sampling opens the segment via
`multiprocessing.shared_memory.SharedMemory(create=False)`, reads the
header fields with `struct.unpack_from`, and closes (never unlinks) the
mapping — a strictly read-only observation with no write path back into
the segment at all.

`format_prometheus()` emits standard OpenMetrics plaintext exposition:

```
# TYPE animus_ring_events_total counter
animus_ring_events_total{ring="my_ring",kind="spsc"} 48213
# TYPE animus_ring_dropped_total counter
animus_ring_dropped_total{ring="my_ring",kind="spsc"} 12
# TYPE animus_ring_consumer_lag gauge
animus_ring_consumer_lag{ring="my_ring",kind="spsc"} 3
# TYPE animus_ring_producer_alive gauge
animus_ring_producer_alive{ring="my_ring",kind="spsc"} 1
```

Counters (`events_total`, `dropped_total`) are exported as raw cumulative
values, deliberately not pre-computed rates — matching Prometheus's own
convention that `rate()` derives a rate from a counter at scrape time,
not the exporter. `dropped_total` and `consumer_lag` are simply omitted
from the output (not zeroed) for an SPMC ring, mirroring
`TelemetrySnapshot::has_dropped_count`/`has_consumer_lag`.

**Runs entirely out-of-band from the critical path.** This is a separate
OS process invoked on a scrape interval (or ad hoc via `--once`), touching
only header bytes the producer/consumer would have written regardless of
whether any scraper exists. It never maps into the producer's or
consumer's own process, never contends for a lock (there are none), and
never perturbs `head`/`tail`/`dropped_count` — it only reads them. The
live terminal dashboard (`--interval`, default 1.0s) and the Prometheus
exporter share the identical `sample_ring()` code path; the only
difference is output formatting. On POSIX, `discover_rings()` auto-lists
candidate segments under `/dev/shm` (filtering happens implicitly —
anything that isn't a recognized animus header just renders as
`(not a recognized animus ring — skipped)`, never a crash); Windows has
no enumerable namespace for named `CreateFileMapping` objects reachable
without elevated privileges, so `--name` is required there.

---

## 5. Technical FAQ (Hard Evaluation Questions)

### Q: How do you mitigate consumer starvation or slow-consumer backpressure?

There is no single answer — the ring topology you choose *is* the
backpressure policy:

- **SPSC, bounded (`try_push`/`push_spin`).** A full ring refuses the
  write (`false`); the producer decides how to react (retry, spill, hard
  drop with its own accounting). The producer never blocks on the OS
  scheduler — `push_spin` is a bounded spin-retry (`cpu_relax()` between
  attempts, default cap 200M iterations), not a wait/futex.
- **SPSC, decoupled (`push_overwrite`).** Never blocks and never
  refuses. On a full ring it forcibly reclaims the oldest unconsumed slot
  (`tail.fetch_add(1, acq_rel)`) and increments `dropped_count` — a lagging
  or dead consumer can only ever cost the producer visibility into old
  data, never latency or throughput. This is a documented lossy contract:
  a concurrent reader mid-`try_pop`/`pop_spin` on the exact slot being
  reclaimed can observe a torn record at that boundary. Only pair this
  mode with a consumer that tolerates that (telemetry, sampling,
  latest-value snapshots) — a channel needing guaranteed-clean reads
  belongs on the bounded path instead.
- **SPMC broadcast (`broadcast`/`poll`).** There is no backpressure
  concept at all, by construction — with an unbounded number of possible
  anonymous consumers there is no single tail position that would even
  define "full." `broadcast()` unconditionally publishes; each consumer's
  `poll()` detects locally if it has fallen more than `capacity()` behind
  and self-corrects by jumping to `head - capacity()`, tallying the
  skipped span into that consumer's own `overrun_count()`. A slow
  consumer's only cost is to itself — it loses visibility into overwritten
  history, and neither the producer nor any other consumer is affected,
  since there is no shared cursor for a slow reader to hold back.

### Q: What happens during ungraceful process crashes (SIGKILL) inside the shm ring?

`SIGKILL` runs no signal handler at all — nothing in a process can react
to its own `kill -9`. Detection is therefore not signal-based; it is
pid-liveness-based, in `include/animus/shm_lifecycle.hpp`:

```cpp
inline bool is_process_alive(uint64_t pid) noexcept {
    // POSIX: kill(pid, 0) — sends no signal, only checks existence/permission; ESRCH means gone.
    // Windows: OpenProcess + GetExitCodeProcess == STILL_ACTIVE.
}
```

The OS answers this correctly and immediately even for a process that was
`SIGKILL`ed and therefore never ran any cleanup of its own — that is the
entire reason this primitive, not signal handling, is what makes
zombie-peer detection possible. `ShmRing<T>::is_producer_alive(uint64_t
stale_after)` / `is_consumer_alive(...)` layer a heartbeat-staleness
check on top of pid liveness for the *complementary* case (a peer that is
alive but wedged — stopped under a debugger, deadlocked elsewhere) —
`stale_after == 0` means pid-liveness only.

What survives the crash, mechanically:

- **Data already published is intact.** A producer killed between
  writing `slots_[head & mask]` and the subsequent
  `head.store(..., release)` never advances `head` — the consumer's
  `acquire`-load of `head` therefore never observes that in-flight slot as
  written, by the same release/acquire ordering guarantee described in
  §2.2. A crash mid-write cannot produce a torn record visible to a
  well-behaved `try_pop`/`poll` caller *through this mechanism* — that
  guarantee is separate from, and does not cover, the already-documented
  `push_overwrite`/broadcast-overrun torn-read races (§2.3), which are an
  accepted tradeoff of lossy mode, not a crash-consistency gap.
- **The segment itself outlives the crashed process.** POSIX:
  `shm_unlink()` is never called implicitly on process death — a killed
  producer leaves the `/dev/shm` node in place, and a surviving consumer
  keeps reading whatever was last published; the owning side is
  responsible for calling `unlink()` explicitly during a normal shutdown
  path, only after every attached process (including itself) is done.
  Windows: named file mappings are reference-counted by `HANDLE` — the
  kernel object is destroyed automatically once the last handle across
  every process (including the crashed one, whose handles the OS closes
  on process termination) is released, so a crashed sole-owner's mapping
  is cleaned up as a natural consequence with no explicit unlink concept
  to get wrong.
- **`SignalGuard` (`shm_lifecycle.hpp`) is a complementary, not
  overlapping, mechanism** — it handles *graceful* `SIGINT`/`SIGTERM`
  (sets a flag an ordinary control loop polls, then does an orderly
  detach) and best-effort `SIGSEGV`/`SIGABRT` on POSIX (sets a
  `fault_detected` flag for a *separate* monitor thread or supervisor
  process to observe, then re-raises with the default disposition
  restored so the OS still produces a core dump — deliberately doing
  nothing more inside the handler itself, since post-fault process state
  may already be corrupt and attempting to unmap/unlink from inside a
  signal handler would not be async-signal-safe). It explicitly does not
  and cannot help with `SIGKILL`; that gap is exactly what pid-liveness
  checking exists to close.

### Q: How does Animus compare to traditional shared-memory rings (e.g. LMAX Disruptor port, Boost.Interprocess, Aeron IPC)?

| | **Animus `ShmRing`/`SpmcRing`** | **LMAX Disruptor** | **Boost.Interprocess** | **Aeron IPC** |
|---|---|---|---|---|
| Process boundary | Real OS shared memory (`mmap`/`CreateFileMapping`) — genuinely cross-process | In-process (JVM heap) — no cross-process story of its own | Cross-process, general-purpose managed segments | Cross-process via a memory-mapped log buffer, driven by a separate media-driver process |
| Producer/consumer topology | SPSC (`ShmRing<T>`) or SP, unbounded-MC broadcast (`SpmcRing<T>`) — no MPMC in this milestone | MPMC via sequence barriers and configurable `WaitStrategy` | Whatever you build on its primitives — typically mutex/condvar-guarded containers, not lock-free by default | SPSC/MPSC framed streams with publication limits and flow control |
| Backpressure | Explicit per-mode choice: bounded refusal (`try_push`) or deterministic lossy overwrite (`push_overwrite`) with a counted drop; SPMC has none by design | Configurable `WaitStrategy` (busy-spin / yield / blocking) applied uniformly | OS-level blocking (mutex/condition variable) in typical usage — pays a futex/syscall on contention | Publication back-pressure via term-buffer limits; driver-mediated flow control |
| Wait strategy | Fixed: bounded busy-spin with `cpu_relax()` (`_mm_pause`/`yield`) between attempts, no configurable backoff/blocking tier | Pluggable per use case | Typically OS wait primitives, not spin-poll | Configurable idle strategies in the client, driver runs its own conductor loop |
| Schema/versioning | Compile-time `ANIMUS_DEFINE_SCHEMA` registration, FNV-1a version hash stamped in-header and checked at `open()`, wire-format string exposed for dynamic NumPy decode | Pre-allocated, reusable Java event objects — no wire-format/version negotiation needed (same JVM, same class) | None built in — caller-defined layout | Framed messages with Aeron's own header, no user-schema versioning primitive |
| Extra infrastructure required | None — a named segment plus this header; no separate driver/conductor process | JVM + Disruptor runtime | Boost only | A running Aeron media-driver process doing term-buffer rotation, heartbeats, and conductor housekeeping |
| Where it's smaller by design | No term-buffer rotation (flat power-of-two slot array + mask, not a rotating log), no stream framing beyond the fixed-record schema, no MPMC | — | — | No general pub/sub bus, no multi-transport (UDP/IPC) unification — this is IPC-only |

The practical read: Animus's ring is intentionally narrower in scope than
Aeron (the closest production analogue) in exchange for zero extra
infrastructure — no media-driver process, no term-buffer cleaning thread,
no conductor — because it targets exactly one thing, a fixed-record-type
ring attached to one named segment, not a general message bus. Compared
to a Disruptor port, the fundamental difference is the process boundary
itself: Disruptor's ring lives in one JVM's heap and its wait strategies
assume threads, not separate OS processes with separate address spaces
and separate cache-coherency domains, which is exactly the problem
`RingHeader`'s cache-line padding (§1.2) and the release/acquire discipline
(§2.2) are built to address. Compared to Boost.Interprocess, the
distinction is lock-free-by-construction versus general-purpose:
Boost.Interprocess can build anything (including this exact ring) but
does not hand you a lock-free SPSC/SPMC ring with schema versioning and
crash-liveness detection out of the box — you would be implementing this
document's §2 and §5 yourself on top of it.
