# Animus Core -- Pilot Evaluation Kit

A minimal, self-contained quickstart for evaluating Animus Core's Python
<-> native-DLL interop during a pilot. If you're integrating from source
or need the full API surface (CEP rules, mTLS, clustering, market data
feeds, ...), see [`AnimusCore_v1/QUICKSTART.md`](../AnimusCore_v1/QUICKSTART.md)
and [`AnimusCore_v1/BENCHMARKS.md`](../AnimusCore_v1/BENCHMARKS.md) instead
-- this kit covers only what's needed to get a first real ingestion call
running end to end.

## What's in this kit

| File | Purpose | Who runs it |
|---|---|---|
| `PILOT_README.md` | This document | You |
| `animus_integration_example.py` | Complete, runnable Python sample: loads the engine, ingests events through the real C-ABI, measures and reports per-event ingestion latency | Pilot customer |
| `get_fingerprint.ps1` | Prints this machine's hardware fingerprint (no private key involved -- read-only, safe to run) | Pilot customer |
| `generate_license.py` | Issues a 30-day, hardware-locked evaluation `.lic` file from a fingerprint | **Vendor only** -- see below |

## Prerequisites

* Python 3.8+ (any platform for core ingestion). No third-party Python
  packages -- everything `animus/` imports is stdlib (`ctypes`,
  `threading`, `multiprocessing.shared_memory`); see `CLAUDE.md`.
* A compiled native engine for your platform: `AnimusNative.dll` /
  `libanimus_native.so` / `libanimus_native.dylib` (portable CMake build)
  or `AnimusCore_v1.dll` (MSVC build). If you received a wheel from your
  Animus Core contact, this is already bundled inside it. Otherwise, build
  from source: `cmake -S . -B build && cmake --build build`, or open
  `AnimusCore_v1.slnx` in Visual Studio (Windows).
* License verification (`animus_verify_license`) is Windows-only. Core
  event ingestion -- everything this kit's example demonstrates -- works
  on any platform with or without a license.

## Setup

1. From a full source checkout, install the SDK in editable mode:

   ```bash
   pip install -e .
   ```

   (If you were given a pre-built wheel instead, `pip install path/to/animus_engine_sdk-*.whl` and skip straight to step 2 -- the native library is already inside it.)

2. If you were issued an evaluation license (`your_company_pilot.lic`),
   place it anywhere accessible -- there's no fixed required path, you'll
   pass it explicitly in the next step.

3. Run the example from the repository root:

   ```bash
   python Pilot_Kit/animus_integration_example.py [path/to/your_pilot.lic]
   ```

   The license argument is optional. Without it, the example still runs
   the full event-ingestion demo -- a license is only required for
   features this demo doesn't use (CPU core pinning; see
   `AnimusCore_v1/BENCHMARKS.md`'s licensing phases for why that's
   gated and this isn't).

## What the example does

`animus_integration_example.py` is a complete, real integration, not
pseudocode:

1. Loads the compiled native engine via `animus.bindings.AnimusBindings`
   (the same ctypes interop layer every other script and test in this
   repository uses -- see `animus/bindings.py`).
2. If given a license path, checks and prints its status
   (`VALID` / `EXPIRED` / `WRONG_MACHINE` / ...).
3. Initializes the engine, registers one threshold rule, and starts
   asynchronous disk persistence.
4. Ingests 500,000 synthetic telemetry events in 10,000-event batches via
   `animus_record_events_batch` -- the real C-ABI batched-ingestion call,
   timed with `time.perf_counter_ns()` around the call itself (list
   construction happens before the timer starts, so Python-side overhead
   never contaminates the measured number).
5. Prints the **measured** mean per-event ingestion latency for your run.
   On the reference development machine this consistently lands well
   under 1 microsecond per event at this batch size (see
   `AnimusCore_v1/BENCHMARKS.md` Phase 11/13/26 for numbers gathered
   across many runs) -- the script reports what your machine actually
   measures rather than asserting a fixed number, since batch size, CPU,
   and background load all affect it.
6. Drains and reports the threat signals the registered rule matched.

## Evaluation licenses

Animus Core's licensing is offline and hardware-locked: a `.lic` file is
RSA-2048-signed against one specific machine's hardware fingerprint
(`MachineGuid` + primary NIC MAC address, SHA-256), verified entirely
offline with no network call. **Core event ingestion never requires a
license** -- verification only gates a small set of opt-in features (CPU
core pinning; see `AnimusCore_v1/BENCHMARKS.md`'s licensing phases) that
fail closed with no unlicensed default, not even core 0.

Getting your evaluation license is a two-step, two-party handoff:

1. **You (pilot customer)** run `get_fingerprint.ps1` on the machine you
   want licensed and send the printed fingerprint to your Animus Core
   contact:

   ```powershell
   powershell -ExecutionPolicy Bypass -File Pilot_Kit\get_fingerprint.ps1
   ```

   This is read-only (registry `MachineGuid` + network adapter info),
   makes no network call, writes nothing to disk, and involves no private
   key -- safe to run and safe to send the output from.

2. **Your Animus Core contact (vendor side)** runs `generate_license.py`
   with that fingerprint and sends back the resulting `.lic` file:

   ```bash
   python Pilot_Kit/generate_license.py --out acme_corp_pilot.lic \
       --fingerprint <the 64 hex chars you sent them>
   ```

   This step requires the private RSA signing key
   (`AnimusCore_v1/license_tools/private/`, gitignored, never
   distributed), so it's run by your Animus Core contact from a full
   clone of the source repository, not by you. It's included in this kit
   for transparency about how your license is generated, not as a step in
   your own setup.

## Questions

This is a proprietary, pre-release evaluation build (see `LICENSE` at the
repository root) -- reach out to your Animus Core contact with any
questions about this pilot. Full architecture and verified performance
numbers: [`AnimusCore_v1/QUICKSTART.md`](../AnimusCore_v1/QUICKSTART.md),
[`AnimusCore_v1/BENCHMARKS.md`](../AnimusCore_v1/BENCHMARKS.md).
