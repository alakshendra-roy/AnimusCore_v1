# Animus Core — External Evaluation Guide

**Audience:** an external quant/infra team independently verifying the throughput and tail-latency figures in [`BENCHMARK_DATASHEET.md`](../BENCHMARK_DATASHEET.md), on their own hardware, before or during an outbound conversation.

**What this guide covers, and what it deliberately doesn't:** everything here runs against the public C-ABI (the compiled `AnimusNative.dll` / `libanimus_native.so`) or a self-contained benchmark file with its own throwaway ring buffer — nothing here requires access to the proprietary engine's source, its internal headers, or a license. That's a deliberate scope boundary, not an oversight: the licensed, CPU-pinned SPSC path (see `benchmarks/fintech_tail_latency.py`) is real, measured, and documented, but validating it against your own hardware with vendor-side tuning support is exactly what Week 3 of the [Institutional Pilot Program](PILOT_PROGRAM.md#3-four-week-timeline) is for. This guide is the free, self-serve check you can run before that conversation even starts.

Three independent measurement layers, matching the split `BENCHMARK_DATASHEET.md` itself insists on (§2's methodology note) — don't conflate them:

| Layer | What it measures | Tool | Needs the compiled DLL? | Needs a license? |
|---|---|---|---|---|
| **Python SDK batched-ingestion** | What a Python caller actually sees per `animus_record_events_batch` call, ctypes overhead included | [`scripts/benchmark_eval.py`](../scripts/benchmark_eval.py) (new) | Yes | No |
| **Native cross-core SPSC transport** | Raw producer→consumer handoff latency, RDTSC-resolution, no Python/ctypes in the loop | [`benchmarks/telemetry_benchmark.cpp`](../benchmarks/telemetry_benchmark.cpp) (existing, self-contained) | No — compiles standalone | No |
| **Cross-process shared-memory transport (Linux)** | Two independent OS processes, not threads: a native producer and a zero-copy nanobind Python consumer exchanging records over real `/dev/shm`, GIL released during the consumer's spin-wait | [`eval_kit/`](../eval_kit/README.md) (turnkey tarball, `./run_demo.sh`) | No — prebuilt binary + wheels bundled in | No |

---

## 1. Prerequisites

- **For the Python SDK layer:** Python 3.8+, any platform. Zero third-party packages — `animus/` imports only `ctypes`, `threading`, and `multiprocessing.shared_memory` from the standard library (see `CLAUDE.md`).
- **For the native transport layer:** a C++23 compiler — MSVC (Visual Studio 2022+) on Windows, or GCC 13+/Clang 17+ with `libstdc++` on Linux/macOS. No other dependency; the file is self-contained.
- **For the cross-process shared-memory layer:** Linux x86_64, Python 3.10+, and a writable `/dev/shm` — no compiler needed, `eval_kit/`'s tarball ships a prebuilt binary and both Python wheels.
- A compiled native engine for the SDK layer only: either build it yourself (`cmake -S . -B build && cmake --build build`, or open `AnimusCore_v1.slnx` in Visual Studio on Windows), or use a pre-built wheel if one was provided to you (`pip install path/to/animus_engine_sdk-*.whl`) — the native library is already bundled inside it.

---

## 2. Layer 1 — Python SDK batched-ingestion (throughput + tail latency)

From a full source checkout (or after installing the SDK in editable mode with `pip install -e .`), run:

```bash
python scripts/benchmark_eval.py
```

This reproduces the exact measurement `BENCHMARK_DATASHEET.md`'s "Python SDK batched-ingestion throughput" table cites: batch size 100, 1,000,000 events, each `animus_record_events_batch` call timed individually with `time.perf_counter_ns()` around the call itself (batch-list construction happens before the timer starts, so Python-side overhead never contaminates the measured call latency). It prints throughput and p50/p90/p99/p99.9 call latency, and — at the default batch size of 100 — a side-by-side comparison against the published reference figures.

Run the sweep across batch sizes, the same three the datasheet's own methodology covers:

```bash
python scripts/benchmark_eval.py --batch-size 100    --total-events 1000000
python scripts/benchmark_eval.py --batch-size 1000   --total-events 1000000
python scripts/benchmark_eval.py --batch-size 10000  --total-events 1000000
```

Batch size materially changes both throughput and latency — that's expected, not a bug in either the engine or the script (a larger batch amortizes the fixed per-call ctypes marshalling cost over more events, at the cost of a larger single-call latency).

Save raw results for your own records or to send back if your numbers diverge meaningfully from the published reference:

```bash
python scripts/benchmark_eval.py --batch-size 100 --total-events 1000000 --json-out results_batch100.json
```

The JSON includes every raw per-batch latency sample, not just the summary percentiles, so it can be independently re-analyzed.

**Run each configuration more than once.** A single run on a general-purpose, non-real-time OS is one sample, not a verified number — background scheduler noise, thermal throttling, and whatever else is running on the box all move the tail. This mirrors how every benchmark in this repository is described (see `BENCHMARKS.md`'s "representative run out of N" framing throughout).

---

## 3. Layer 2 — Native cross-core SPSC transport (the 64.9ns p99 number)

This is a different methodology layer, not reachable from Python (interpreter and ctypes-marshalling overhead would drown out sub-100ns figures, which is exactly why the SDK layer above reports microseconds, not nanoseconds). It's measured with a dedicated, self-contained C++23 file — `benchmarks/telemetry_benchmark.cpp` — that implements its own throwaway lock-free SPSC ring for the benchmark and doesn't include or link against any proprietary engine header. Reading the file's `#include` list is enough to confirm that independently before you compile it.

Compile and run:

```bash
# MSVC (Windows)
cl /std:c++latest /EHsc /O2 /DNDEBUG /Fe:telemetry_benchmark.exe benchmarks\telemetry_benchmark.cpp
telemetry_benchmark.exe

# GCC/Clang + libstdc++ (Linux/macOS)
g++ -std=c++23 -O3 -DNDEBUG -pthread -o telemetry_benchmark benchmarks/telemetry_benchmark.cpp -lstdc++exp
./telemetry_benchmark
```

One producer thread and one consumer thread, pinned to separate physical cores by the benchmark itself (no license needed — this is ordinary `std::thread` affinity, not the engine's licensed core-pinning feature), handing off cache-line-aligned events through the lock-free SPSC ring. Timestamps are lfence-serialized RDTSC reads, calibrated against `std::chrono::steady_clock`. It reports both a depth-1 latency phase (producer waits for the consumer's ack before sending the next event — real transport cost, not queueing backlog) and an unthrottled throughput phase. Full methodology detail is in `BENCHMARK_DATASHEET.md` §2 under "Cross-core SPSC dispatch latency."

Run it 3 times (the datasheet's own figures are "representative run out of 3") — max latency across all runs will land in the hundreds of microseconds; that's OS scheduling noise on the pinned cores, not the transport, and is called out explicitly in the datasheet rather than hidden.

---

## 4. Layer 3 — Cross-process shared-memory transport (Linux)

A different question again from Layer 2: not "how fast is a handoff between two threads in one process," but "how fast can two independent OS processes exchange records with no serialization step." `eval_kit/` packages this as a turnkey tarball — a prebuilt `-march=x86-64-v3` producer binary plus both Python wheels it needs, no compiler or `cmake` required on your evaluation machine:

```bash
tar xzf animus-eval-kit-linux-x86_64.tar.gz
cd animus-eval-kit-linux-x86_64
./run_demo.sh
```

`run_demo.sh` creates an isolated venv, installs the bundled wheels, runs the producer (10,000,000 synthetic events, decoupled/non-blocking overwrite mode, so it completes on its own with no consumer required), then runs a zero-copy nanobind Python consumer against whatever survived in the ring, and prints a pass/fail verdict with throughput and latency percentiles from both sides. See [`eval_kit/README.md`](../eval_kit/README.md) for the full architecture (cache-line-isolated `alignas(64)` head/tail cursors, the decoupled-overwrite contract, and the GIL-released consumer spin-wait) and troubleshooting (SHM permission errors, cleaning orphaned rings, CPU pinning via `taskset`).

This is the one layer of the three actually verified against real `/dev/shm` in CI (`.github/workflows/eval_kit_packaging.yml`, `ubuntu-22.04`), not just compiled and assumed — worth knowing if you're deciding which layer's numbers to trust most on a Linux target.

---

## 5. Interpreting your results

| If you see... | Likely cause |
|---|---|
| Numbers roughly in line with the published reference (same order of magnitude, plausible variance) | Working as expected — per-hardware, per-run variance is normal and is what this evaluation exists to characterize honestly, not something to explain away. |
| Layer 1 numbers dramatically worse than reference, but not erroring | Check for background CPU load, a debug (non-`/O2`/`-O3`) build, or a busy/throttled machine. Try a larger `--batch-size` — smaller batches are more sensitive to scheduler noise. |
| A warning that no compiled native engine was found | The script refuses to report numbers from the pure-Python fallback engine as if they were a native-transport reproduction — build the engine first (§1) and re-run. |
| Layer 2 numbers far outside the published range | Confirm you built with optimizations enabled (`/O2` / `-O3` and `-DNDEBUG`) — an unoptimized debug build will not reproduce sub-100ns figures. Confirm your machine actually has ≥2 physical cores available to pin to. |
| Layer 3's `pip install wheels/animus_native_stream-*.whl` fails | That wheel ships a compiled extension — its filename encodes the exact Python version/platform it was built for (see the tarball's `MANIFEST.txt`). Use a matching `python3`, or request a kit rebuilt against yours; `eval_kit/README.md`'s Troubleshooting section covers this and SHM-permission issues. |
| You want the CPU-pinned, license-gated SPSC path's numbers reproduced too | That's out of scope for this free evaluation by design — it requires a vendor-issued evaluation license and is exactly what the [Institutional Pilot Program](PILOT_PROGRAM.md)'s Week 3 (Tail-Latency Analysis) validates on your own hardware, with our engineering team, using your actual workload shape rather than synthetic payloads. |

---

## 6. Questions / reporting results

This is a proprietary, pre-release evaluation build (see `LICENSE` at the repository root). If your numbers diverge meaningfully from the published reference, or you'd like help interpreting a result, reach out with the JSON output from §2 and the console output from §3 attached:

**Alakshendra Roy** — Founder & Chief Architect, Animus Core
alakshendra@animusinfra.com | +91 9891161189
animusinfra.com
