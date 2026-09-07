# AnimusCore NASDAQ TotalView-ITCH 5.0 Reference Adapter

## 1. Overview

This adapter demonstrates **zero-copy, zero-heap-allocation wire decoding**
of NASDAQ TotalView-ITCH 5.0 binary messages directly into the Animus SPSC
lock-free ring buffer (`animus::eval::SpscRingBuffer<T>`,
`animus-eval-kit/include/spsc_ring_buffer.hpp`) -- the same cache-line-padded,
single-producer/single-consumer ring the core Animus engine benchmark
(`animus_bench`) is built on, applied here to a real, named binary market
data protocol instead of a synthetic telemetry frame.

The pipeline end to end:

```
raw big-endian ITCH bytes  --decode()-->  ItchFrame (64B, normalized)  --push()-->  SPSC ring  --pop()-->  consumer
        (wire)                  no heap alloc, no copy of the wire            lock-free, cache-line-padded
                                 buffer beyond one memcpy per multi-
                                 byte field into a local
```

No step in that pipeline allocates from the heap, and no step serializes or
deep-copies a record beyond the one, unavoidable plain-assignment copy into
the ring slot itself -- the same zero-allocation discipline
`benchmarks/animus_harness.cpp` (`animus_bench`) already proves for
synthetic telemetry, extended here to a real exchange wire format. This is
the intended pattern for a latency-sensitive market-data feed handler: parse
once at the network boundary into a fixed-size normalized record, then move
that record through the rest of the pipeline (ring, cross-process shared
memory, a Python analytics process) with no further per-message parsing.

It is an **evaluation/benchmark-grade reference decoder**, not a
vendor-certified production one -- see [§2's compliance note](#2-specification--wire-architecture)
below.

## 2. Specification & Wire Architecture

### 2.1 Supported message types

This adapter decodes the eight ITCH 5.0 message types most relevant to
reconstructing top-of-book / order-flow state from a live feed. Field
offsets follow the publicly documented ITCH 5.0 message specification,
excluding the outer SoupBinTCP/MoldUDP64 session-layer framing (the 2-byte
packet length prefix and sequence headers that wrap every message in a live
multicast feed) -- that framing is a transport concern this adapter
deliberately leaves to its caller.

| Tag | Message | Wire size | Key fields carried into `ItchFrame` |
|---|---|---:|---|
| `S` | System Event | **12 bytes** | `stock_locate`, `itch_timestamp_ns`, `side_or_flag` = EventCode |
| `A` | Add Order (No MPID Attribution) | **36 bytes** | `order_ref_number`, `side_or_flag` = BuySellIndicator, `shares`, `stock_ticker_raw`, `price_ticks` |
| `F` | Add Order (With MPID Attribution) | **40 bytes** | same as `A`; Attribution (MPID) is parsed off the wire but not carried into `ItchFrame` -- see the scope note below |
| `E` | Order Executed | **31 bytes** | `order_ref_number`, `shares` = ExecutedShares, `secondary_ref_number` = MatchNumber |
| `C` | Order Executed With Price | **36 bytes** | same as `E`, plus `side_or_flag` = Printable, `price_ticks` = ExecutionPrice |
| `X` | Order Cancel | **23 bytes** | `order_ref_number`, `shares` = CancelledShares |
| `D` | Order Delete | **19 bytes** | `order_ref_number` |
| `U` | Order Replace | **35 bytes** | `order_ref_number` = OriginalOrderReferenceNumber, `secondary_ref_number` = NewOrderReferenceNumber, `shares`, `price_ticks` |

**Compliance note:** verify field offsets against your own copy of Nasdaq's
official ITCH 5.0 specification before pointing this at a production feed.
This adapter also does not attempt full order-book reconstruction -- it
normalizes each message into a flat, uniform record for ring transport, not
into a live book data structure. Every wire struct is defined as a
`#pragma pack(push, 1)` layout in
[`include/itch50_messages.hpp`](include/itch50_messages.hpp), each with a
`static_assert(sizeof(...) == N, ...)` pinning it to the exact byte count
above.

### 2.2 The `ItchFrame` normalized record (64 bytes, one cache line)

All eight wire shapes above (12 to 40 bytes each) decode into one uniform,
fixed-size record -- `ItchFrame`
([`include/itch50_ring_frame.hpp`](include/itch50_ring_frame.hpp)) -- before
ever touching the ring. `alignas(64)` plus the field layout below pads
`sizeof(ItchFrame)` to **exactly 64 bytes**, so one frame never straddles two
cache lines and an array of frames never causes false sharing between
adjacent ring slots (`static_assert(sizeof(ItchFrame) == 64, ...)` enforces
this at compile time):

| Offset | Field | Type | Bytes | Meaning |
|---:|---|---|---:|---|
| 0 | `sequence_id` | `uint64_t` | 8 | Adapter-assigned monotonic ingest sequence (not an ITCH wire field) -- verifies ring ordering end to end |
| 8 | `recv_timestamp_ns` | `uint64_t` | 8 | Host ingest clock sample, taken immediately before decode |
| 16 | `itch_timestamp_ns` | `uint64_t` | 8 | Decoded ITCH Timestamp (nanoseconds since midnight) |
| 24 | `order_ref_number` | `uint64_t` | 8 | OrderReferenceNumber / OriginalOrderReferenceNumber |
| 32 | `secondary_ref_number` | `uint64_t` | 8 | MatchNumber (E/C) or NewOrderReferenceNumber (U) |
| 40 | `stock_ticker_raw` | `uint64_t` | 8 | A/F only: the 8-byte ITCH Stock field, carried as an opaque byte blob (not byte-swapped -- see §2.3) |
| 48 | `price_ticks` | `int64_t` | 8 | Decoded Price/ExecutionPrice (ITCH's native fixed-point unit, 4 implied decimal digits) |
| 56 | `shares` | `uint32_t` | 4 | Shares / ExecutedShares / CancelledShares |
| 60 | `stock_locate` | `uint16_t` | 2 | StockLocate -- every message type carries this; the per-symbol routing key |
| 62 | `msg_type` | `uint8_t` | 1 | The ASCII message tag (`'S'`/`'A'`/.../`'U'`) |
| 63 | `side_or_flag` | `uint8_t` | 1 | BuySellIndicator, Printable, or EventCode depending on `msg_type` |

Field reuse across message types (documented per row above) is deliberate:
this adapter's stated scope is proving zero-copy/zero-allocation ring
ingestion of parsed ITCH telemetry, not shipping a complete order-book
builder.

### 2.3 Endian conversion pipeline

ITCH 5.0 is a big-endian wire protocol; every host this adapter targets
(x86_64, ARM64) is little-endian. glibc's `be16toh`/`be32toh`/`be64toh`
would handle this on Linux, but don't exist on MSVC -- and this adapter's
own build matrix requires compiling cleanly there too -- so
[`include/itch50_bswap.hpp`](include/itch50_bswap.hpp) provides the same
operation as a small, portable wrapper over each toolchain's native
byte-swap intrinsic, with **no unaligned-load undefined behavior**:

```cpp
// Compiler-intrinsic byte reversal -- one BSWAP (x86) / REV (ARM64)
// instruction under GCC, Clang, or MSVC at -O1 and above.
inline uint32_t bswap32(uint32_t v) noexcept {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

// Field reads go through memcpy -- never a reinterpret_cast<uint32_t*>
// over a #pragma pack(1) struct's field address, which the C++ standard
// treats as undefined behavior for a misaligned access even though x86
// tolerates it in practice. A fixed-size memcpy is recognized by every
// mainstream compiler at -O2/-O3 and folded into a single unaligned load,
// so this costs nothing at runtime versus the unsafe direct-cast version.
inline uint32_t be32toh_field(const void* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return bswap32(v);
}
```

The same pattern covers 16- and 64-bit fields (`be16toh_field`/
`be64toh_field`). ITCH's 6-byte (48-bit) Timestamp field has no native C++
integer type of that width, so `be48toh_ns()` assembles it by hand via six
shifts rather than an 8-byte `memcpy` that would read 2 bytes past a
Timestamp field with nothing after it. `stock_ticker_raw` is the one
exception to "every multi-byte field gets byte-swapped": the ITCH Stock
field is eight raw ASCII bytes, not a big-endian integer, so it is `memcpy`'d
verbatim with no swap -- see the field's own decode note in
`itch50_ring_frame.hpp`.

## 3. Verified Benchmark Telemetry (Dual-Platform, 10,000,000-Message Runs)

Both rows below are one real `bench_itch_ingest --messages 10000000` run
each, captured on the same physical development machine (not a dedicated,
CPU-isolated benchmark rig -- no `isolcpus`/`nohz_full`/real-time scheduling;
see `docs/PILOT_EVAL_CHECKLIST.md` §2 for what that tuning buys). As with
every other benchmark in this repository, these are real, reproducible
numbers from an actual run, not projected or hand-typed figures --
reproduce them yourself with the command in §5. Tail latency (p99.9, max)
in particular varies run to run on an untuned host; p50/p90/sustained
throughput are the stable, representative figures.

| Platform / Toolchain | Sustained Rate | p50 | p90 | p99 | p99.9 | Heap Allocs | Decode Failures | Sequence Corruption |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **Windows** -- MSVC 19.51 (`cl.exe` 19.51.36256), Release `/O2` | **6.82M msgs/sec** | 82 ns | 86 ns | 98 ns | 106 ns | **0** | **0** | **0** |
| **Linux** -- GCC 15.2 (Ubuntu, WSL2 kernel), `-O3` | **7.80M msgs/sec** | 70 ns | 106 ns | 130 ns | 350 ns | **0** | **0** | **0** |

Both runs generated the identical message-type distribution (same seeded
generator: ~30.0% Add Order, ~25.0% Order Delete, ~20.0% Order Executed,
~12.0% Order Cancel, ~5.0% Add Order MPID, ~4.0% Order Replace, ~3.0% Order
Executed+Price, ~1.0% System Event, out of 10,000,000 messages) and printed
`VERIFIED: 0 decode failures, 0 corrupted frames, 0 hot-path heap
allocations.` -- confirmed bit-identical decode behavior across both
byte-swap intrinsic paths (`_byteswap_ulong` on MSVC, `__builtin_bswap32` on
GCC).

## 4. Python Zero-Copy Ingestion

`animus-engine-sdk` (the `animus/` package) and `animus-native-stream`
(`bindings/`, its compiled `_animus_shm_native` extension) consume
`ItchFrame` records directly into a NumPy structured array with **no copy
and no `ItchFrame`-specific binding code** -- `ItchFrame` qualifies for the
*existing*, schema-agnostic `SharedSchemaChannel` +
`animus.dynamic_schema.to_structured_array()` mechanism (Milestone 1,
`include/animus/schema.hpp`) purely by being a registered
`ANIMUS_DEFINE_SCHEMA` wire schema:

```python
from animus._animus_shm_native import SharedSchemaChannel
from animus.dynamic_schema import to_structured_array

# Attach to the segment itch50_shm_bridge (a separate C++ process) created
# and is holding open -- see §5's runbook for the exact launch sequence.
channel = SharedSchemaChannel.open("animus_itch50_demo")

# Zero-copy: internally np.frombuffer(channel.raw_view(), dtype=...), where
# the dtype itself is derived at runtime from the wire_format string
# ItchFrame's ANIMUS_DEFINE_SCHEMA registration stamped into the segment
# header -- "<QQQQQQqIHBB", decoded field-for-field, byte-for-byte.
arr = to_structured_array(channel)

assert arr.flags["OWNDATA"] is False          # a view over shared memory, not a copy
assert arr.shape[0] == channel.capacity       # covers the whole ring, not just filled slots

# Decode one live record (fields are unnamed f0..f10 in ItchFrame's
# declaration order -- struct.calcsize carries no field names).
row = arr[channel.tail]
(sequence_id, recv_timestamp_ns, itch_timestamp_ns, order_ref_number,
 secondary_ref_number, stock_ticker_raw, price_ticks, shares,
 stock_locate, msg_type, side_or_flag) = (row[f"f{i}"] for i in range(11))
```

Run [`verify_zero_copy_numpy.py`](verify_zero_copy_numpy.py) for the full,
runnable version of this (message-type decoding, ticker/price formatting,
and the zero-copy assertions above, all in one script) -- see §5.

## 5. Build & Runbook Commands

### 5.1 Build

Both C++ binaries are wired into the root `CMakeLists.txt`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_itch_ingest --target itch50_shm_bridge
```

Lands at `build/bin/bench_itch_ingest` and `build/itch50_shm_bridge`
respectively (Windows multi-config generator: `build/bin/Release/
bench_itch_ingest.exe` / `build/Release/itch50_shm_bridge.exe`). Compiles
cleanly under GCC 13+/Clang 17+ (`-O3 -Wall -Wextra -Wpedantic`, zero
warnings -- verified on GCC 15.2/Ubuntu/WSL2) and MSVC (`/W4 /permissive-`,
zero warnings other than the deliberately-suppressed `/wd4324` cache-line-
padding note -- verified on MSVC 19.51).

### 5.2 Run the ingestion benchmark

```bash
build/bin/bench_itch_ingest --messages 10000000
```

Generates the requested count of synthetic ITCH messages across all eight
message types, decodes and pushes each onto an
`animus::eval::SpscRingBuffer<ItchFrame>`, and prints the message-type
distribution, sustained throughput, sequence-integrity check, hot-path
heap-allocation counters, and the RDTSC-calibrated parse+enqueue latency
profile -- see §3 above for a real, verified sample of this output.
`--capacity <slots>` overrides the default 1,048,576-frame ring.

### 5.3 Run the Python zero-copy verification

Build the extension first if you haven't already (see
`docs/EVALUATION_KIT.md`):

```bash
pip install ./bindings
```

Then, in one terminal, start the bridge and leave it running:

```bash
build/itch50_shm_bridge --name animus_itch50_demo --messages 100000 --hold-seconds 60
```

**Windows note:** a Windows named file mapping is destroyed the instant its
last open `HANDLE` closes -- unlike POSIX `shm_open`, which persists across
process exit until an explicit `shm_unlink`. The bridge above must still be
running (inside its `--hold-seconds` wait) when the script below runs;
launch them concurrently, in two terminals, exactly as
`eval_kit/README.md`'s own Troubleshooting section documents for the
identical reason.

While it's still holding the segment open, in a second terminal:

```bash
python adapters/itch50/verify_zero_copy_numpy.py --name animus_itch50_demo
```

This attaches to the live segment, builds the zero-copy NumPy structured
array shown in §4, and prints a handful of decoded records -- message type,
ticker, price, shares, order reference number -- read directly out of the
shared-memory bytes the C++ side wrote, with no intermediate per-record
Python object construction.

## Layout

```
adapters/itch50/
  include/
    itch50_bswap.hpp        Portable big-endian field readers/writers (§2.3)
    itch50_messages.hpp      #pragma pack(1) wire structs for all 8 message
                             types + the MessageType tag enum (§2.1)
    itch50_ring_frame.hpp    ItchFrame -- the uniform 64-byte record (§2.2)
                             + its ANIMUS_DEFINE_SCHEMA registration
    itch50_codec.hpp         decode() -- the zero-allocation wire-to-
                             ItchFrame parser
  bench_itch_ingest.cpp      In-process SPSC ring benchmark harness (§3)
  itch50_shm_bridge.cpp      Cross-process shared-memory producer for the
                             Python zero-copy consumption demo (§4)
  verify_zero_copy_numpy.py  Python-side zero-copy NumPy verification script
```
