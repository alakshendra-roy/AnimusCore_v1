# Animus Core -- Production Evaluation Kit v1.0 (Windows x86_64)

Self-contained evaluation package for Animus Core's cross-process
shared-memory execution telemetry engine. Both included binaries
(`bin/harness_benchmark.exe` and `lib/AnimusNative.dll`) are Release
builds with all symbols stripped (`strip --strip-all`) -- no debug
info, no embedded PDB path, no internal source paths.

## Quickstart

1. Run `.\scripts\get_fingerprint.ps1` and email the printed 64-hex-character
   output to **access@animusinfra.com** to receive an offline, 30-day
   RSA-2048 evaluation license file (`animus.lic`), signed against your
   machine's hardware fingerprint. This step involves no network call and
   no private key on your side -- it only reads your machine's registry
   `MachineGuid` and primary NIC MAC address.
2. Drop `animus.lic` into this directory's root.
3. Run `.\bin\animus_bench_saturation.ps1` to reproduce the ~16.1M events/sec
   decoupled-overwrite saturation numbers (`docs/EVALUATION_KIT.md` §1.1).
4. Run `.\bin\animus_bench_backpressure.ps1` to verify zero packet drops
   under bounded-retry backpressure mode against the bundled Python
   consumer (`docs/EVALUATION_KIT.md` §1.2).

> **Note on the license step:** core event ingestion and both benchmarks
> above run with **no license at all** -- steps 1-2 are only required if
> your evaluation also needs the opt-in, hardware-gated CPU core-pinning
> tuning path (see `docs/EVALUATION_KIT.md` §2.4). Feel free to run steps
> 3-4 first and come back to licensing only if you need that path.

## Directory layout

```
bin/       animus_bench_saturation.ps1, animus_bench_backpressure.ps1 --
           thin launchers around the one real stripped producer binary,
           harness_benchmark.exe, pinning the arguments that reproduce
           each documented benchmark mode.
include/   animus/*.hpp -- public ShmRing<T> transport headers
           (include/animus/shm_ipc.hpp and its own dependencies) for
           building your own native producer/consumer against the same
           shared-memory ring.
lib/       AnimusNative.dll (stripped) + AnimusNative.lib (import library)
           -- the C-ABI shim, for the ctypes/Python SDK integration path.
python/    consumer.py -- reference zero-copy Python reader: attaches to
           the named shared-memory segment a producer created and decodes
           records directly against the ShmRing header layout, no
           serialization step.
scripts/   get_fingerprint.ps1 -- read-only hardware fingerprint utility
           for the license-activation flow above.
docs/      BENCHMARK_DATASHEET.md, EVALUATION_KIT.md -- full architecture,
           methodology, and reproduction detail behind the numbers above.
```

## Prerequisites

- Windows 10/11, x86_64
- Python 3.8+ on PATH (for `python/consumer.py`, used by
  `animus_bench_backpressure.ps1`) -- stdlib only, no third-party packages

## License Notice

Access to this evaluation kit and the Software it contains is granted
solely under Animus's institutional, non-production evaluation license --
see [https://animusinfra.com/terms](https://animusinfra.com/terms) for
the full scope. Production use requires a separate, executed commercial
agreement.
