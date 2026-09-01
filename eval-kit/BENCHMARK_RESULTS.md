# Animus Engine — Technical Evaluation Results

Results from a native C++ build/run of the [evaluation kit](README.md)'s
ring buffer benchmark, plus verification of the Python SDK's zero-copy
shared-memory interop against the compiled native engine
(`animus/AnimusNative.dll`). All figures below are from a single
development workstation, not an isolated benchmark host — see
[Caveats](#caveats).

## 1. Build

Compiled `eval-kit/benchmark/benchmark_native.cpp` with MSVC (not the
GCC/Clang path in the README) to validate the second toolchain:

```
cl /std:c++20 /O2 /EHsc /W4 benchmark_native.cpp
```

| | |
|---|---|
| Compiler | MSVC 19.51 (Visual Studio 2026, VC Tools 14.51.36231) |
| Standard | C++20 (`/std:c++20`) |
| Optimization | `/O2` |
| Result | **Build succeeded, exit code 0** |
| Warnings | 2× `C4324` ("structure was padded due to alignment specifier") on `RingBuffer<uint64_t>`'s `head_`/`tail_` — expected and intentional, not a defect: this is MSVC confirming the `alignas(64)` cache-line isolation the header calls for actually took effect. No other warnings under `/W4`. |

The header (`eval-kit/include/animus.hpp`) is unmodified from C++17; the
C++20 flag was a compiler-mode check, not a language-feature requirement
— it compiles clean under either standard.

## 2. Ring buffer round-trip benchmark

Methodology: two threads (best-effort pinned to distinct logical cores),
two `RingBuffer<uint64_t>` instances, ping-pong round trip per sample —
see [README.md](README.md) for the full methodology and why this counts
as simulated high-throughput load (busy-spin producer and consumer, no
artificial delay, 1M measured round trips back-to-back after a 100K
warm-up). Three independent runs, MSVC-built binary:

| Percentile | Run 1 (cycles / ns) | Run 2 (cycles / ns) | Run 3 (cycles / ns) | Median (ns) |
|---|---|---|---|---|
| min   | 71 / 29.3    | 69 / 28.5     | 75 / 31.0     | 29.3 |
| **p50**   | 144 / 59.5   | 139 / 57.5    | 140 / 57.9    | **57.9** |
| **p90**   | 165 / 68.2   | 157 / 64.9    | 157 / 64.9    | **64.9** |
| **p99**   | 191 / 79.0   | 191 / 79.0    | 175 / 72.3    | **79.0** |
| p99.9 | 215 / 88.9   | 240 / 99.2    | 199 / 82.3    | 88.9 |
| **max**   | 205,943 / 85,128.6 | 244,379 / 101,016.6 | 1,669,799 / 690,230.9 | — (see below) |

TSC calibrated at ~2.419 GHz on all three runs. Figures are full
round-trip (two ring crossings, producer → consumer → producer); halve
for a rough one-way estimate.

**On `max`:** this workstation has no core isolation configured (no
`isolcpus`/`nohz_full`-equivalent on Windows, background processes free
to land on the pinned cores), so `max` is dominated by OS scheduler
preemption, not the ring buffer — exactly the effect the README's
"Core isolation notes" section describes. The tight, consistent p50–p99.9
band across all three runs (57–99 ns) is the ring buffer's actual
steady-state behavior; treat `max` here as "workstation jitter ceiling,"
not a hardware latency figure. A representative `max` requires the
isolated-core setup documented in the README, on dedicated hardware.

Test machine: Intel Core i7-14650HX, 16 cores / 24 threads, 2.2 GHz base.

## 3. Python SDK zero-copy shared-memory interop

Ran the existing integration test suites (`tests/test_bindings.py`)
against the compiled `animus/AnimusNative.dll` — both native shared-memory
transports the SDK exposes:

```
python -m unittest tests.test_bindings.SharedTelemetryChannelIntegrationTests -v
python -m unittest tests.test_bindings.ShmRingChannelIntegrationTests -v
```

| Suite | Tests | Result |
|---|---|---|
| `SharedTelemetryChannelIntegrationTests` (`animus::SharedTelemetryChannel`, wire-compatible with the pure-Python `SharedTelemetryRing`) | 9 | **All passed** |
| `ShmRingChannelIntegrationTests` (`animus::sys::ipc::ShmRing<RawEvent>`, native-only) | 9 | **All passed** |

The determining test in each suite is `test_real_cross_process_round_trip`
— both passed. Each spawns this test file as a **genuinely separate OS
process** (`subprocess.Popen`, not a second handle inside the same
Python process), has the parent push records after the child has already
attached, and asserts the child reads them back correctly. This is the
actual proof of zero-copy shared-memory semantics, not just a docstring
claim: a second Python interpreter in a different process address space
can only see data it never received through any argument, pipe, or
return value if the two sides are mapped onto the same physical memory —
copying (serializing over a socket/pipe, or duplicating a buffer) would
still pass a same-process round-trip test but cannot pass this one.

Both channels also carry a fixed-size, single-record decode per `pop()`
(`SharedRecord` is asserted to be exactly 24 bytes in `bindings.py`) —
the only data ever copied through Python is the one record being
returned to the caller, never the ring's backing storage.

**Result: verified.** The Python SDK's native shared-memory interop
attaches to and correctly exchanges data through the real ring buffer
with no serialization layer and no bulk-buffer copy, confirmed against
the compiled engine rather than assumed from source alone.

## Caveats

- All figures are from one Windows development workstation with no core
  isolation, turbo-boost control, or dedicated benchmark hardware — see
  the README's "Core isolation notes" for what a clean run requires.
  Treat the p50–p99.9 figures above as directionally representative, not
  a certified hardware benchmark result.
- The MSVC build used here is a second-toolchain validation of the same
  header already exercised by the GCC build in the earlier eval-kit
  work; both compile and run correctly.
