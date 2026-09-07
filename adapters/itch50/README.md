# Animus ITCH 5.0 Adapter

Reference market-data adapter shim demonstrating zero-allocation ingest of
NASDAQ TotalView-ITCH 5.0 binary feeds into the Animus SPSC ring buffer,
plus zero-copy consumption of the decoded records from Python as a NumPy
structured array.

## Scope

This adapter decodes the eight ITCH 5.0 message types most relevant to
reconstructing top-of-book / order-flow state from a live feed:

| Tag | Message | Wire size |
|---|---|---|
| `S` | System Event | 12 bytes |
| `A` | Add Order (No MPID Attribution) | 36 bytes |
| `F` | Add Order (With MPID Attribution) | 40 bytes |
| `E` | Order Executed | 31 bytes |
| `C` | Order Executed With Price | 36 bytes |
| `X` | Order Cancel | 23 bytes |
| `D` | Order Delete | 19 bytes |
| `U` | Order Replace | 35 bytes |

It is an **evaluation/benchmark-grade reference decoder**, not a
vendor-certified production one: field offsets follow the publicly
documented ITCH 5.0 message specification, excluding the outer
SoupBinTCP/MoldUDP64 session-layer framing (packet length prefix, sequence
headers) that wraps every message in a live feed -- that framing is a
transport concern this adapter deliberately leaves to its caller. Verify
field offsets against your own copy of Nasdaq's official ITCH 5.0
specification before pointing this at a production feed. It also does not
attempt full order-book reconstruction (e.g. MPID attribution on `F` is
parsed off the wire but not carried into the ring frame) -- see
`include/itch50_ring_frame.hpp`'s own scope note.

## Layout

```
adapters/itch50/
  include/
    itch50_bswap.hpp        Portable big-endian field readers/writers
                             (__builtin_bswap* / _byteswap_* intrinsics --
                             no glibc-only be64toh dependency, so this
                             builds on MSVC too)
    itch50_messages.hpp      #pragma pack(1) wire structs for all 8 message
                             types + the MessageType tag enum
    itch50_ring_frame.hpp    ItchFrame -- the uniform, fixed-size (one
                             cache line) record every decoded message
                             normalizes into before hitting the ring, plus
                             its ANIMUS_DEFINE_SCHEMA registration
    itch50_codec.hpp         decode() -- the zero-allocation wire-to-
                             ItchFrame parser
  bench_itch_ingest.cpp      In-process SPSC ring benchmark harness (RDTSC-
                             calibrated parse+enqueue latency, sustained
                             throughput, zero-allocation proof)
  itch50_shm_bridge.cpp      Cross-process shared-memory producer for the
                             Python zero-copy consumption demo
  verify_zero_copy_numpy.py  Python-side zero-copy NumPy verification script
```

## Building

Both C++ binaries are wired into the root `CMakeLists.txt`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_itch_ingest --target itch50_shm_bridge
```

Lands at `build/bin/bench_itch_ingest` and `build/itch50_shm_bridge`
respectively (Windows: `build/bin/Release/bench_itch_ingest.exe` /
`build/Release/itch50_shm_bridge.exe` with a multi-config generator).
Compiles cleanly under GCC 13+/Clang 17+ (`-O3 -Wall -Wextra -Wpedantic`,
zero warnings) and MSVC (`/W4 /permissive-`, zero warnings other than the
deliberately-suppressed `/wd4324` cache-line-padding note -- see the
CMakeLists.txt comment on that target).

## Running the ingestion benchmark

```bash
build/bin/bench_itch_ingest --messages 10000000
```

Generates 10,000,000 synthetic ITCH messages across all eight message
types (weighted toward Add Order / Order Delete / Order Executed, the
heaviest real-world traffic on a liquid single-name symbol), decodes and
pushes each onto an `animus::eval::SpscRingBuffer<ItchFrame>`, and prints:

- **Message type distribution** actually generated, for sanity-checking
  the synthetic mix.
- **Sustained ingest rate** (M msgs/sec) across the full producer +
  consumer pipeline.
- **Sequence integrity**: the consumer verifies strict ring ordering
  independent of any ITCH-level field (the synthetic generator does not
  guarantee referentially consistent order-reference numbers across
  message types -- only `ItchFrame.sequence_id`, the ring's own ordering
  guarantee, is asserted here).
- **Hot-path heap allocations/frees**: instrumented `operator new`/
  `delete` counters, snapshotted immediately before and after the timed
  run -- both must read `0` for a verified pass.
- **Latency profile (parse + enqueue overhead)**: min/p50/p90/p99/p99.9/max
  in nanoseconds, from a calibrated RDTSC clock (`lfence`-serialized,
  calibrated against `std::chrono::steady_clock` over a 200ms window at
  startup -- falls back to `clock_gettime(CLOCK_MONOTONIC_RAW)` on non-x86
  targets). Deliberately excludes the synthetic message's own encode step
  from the timed window -- see the file header comment in
  `bench_itch_ingest.cpp` for why.

A run prints `VERIFIED: 0 decode failures, 0 corrupted frames, 0 hot-path
heap allocations.` when every one of those three checks passes.

## Python zero-copy verification

Proves that `animus-engine-sdk` (the `animus/` package) and
`animus-native-stream` (`bindings/`, its compiled `_animus_shm_native`
extension) can consume `ItchFrame` records directly into a NumPy
structured array with **no copy and no ItchFrame-specific binding code** --
`ItchFrame` qualifies for the existing schema-agnostic
`SharedSchemaChannel` + `animus.dynamic_schema.to_structured_array()`
mechanism (Milestone 1, `include/animus/schema.hpp`) purely by being a
registered `ANIMUS_DEFINE_SCHEMA` wire schema.

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

This attaches to the live segment, builds a zero-copy NumPy structured
array over it (asserting `arr.flags['OWNDATA'] is False`), and prints a
handful of decoded records -- message type, ticker, price, shares, order
reference number -- read directly out of the shared-memory bytes the C++
side wrote, with no intermediate per-record Python object construction.
