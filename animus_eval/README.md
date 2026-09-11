# Animus Evaluation Kit

A standalone Proof-of-Performance package for `AnimusEngine`, the
zero-copy, cache-aligned, lock-free SPSC market-data ingestion core at the
heart of Animus. This kit compiles and runs independently of the rest of
the Animus source tree: you get the public header, the engine's shared
library, a replay benchmark harness, and an optional zero-copy Python
bridge -- nothing proprietary, nothing you need the full repository for.

```
animus_eval/
├── include/animus/engine.hpp   # public API -- the only header a client compiles against
├── src/
│   ├── engine.cpp               # AnimusEngine implementation (builds libanimus)
│   └── replay_bench.cpp         # standalone replay benchmark harness
├── python/animus_py.cpp         # nanobind zero-copy bridge (numpy views, no serialization)
├── CMakeLists.txt
├── run_bench.sh                 # turnkey configure+build+run, taskset-confined to cores 2/3
└── README.md
```

## What you're evaluating

`AnimusEngine` wraps a single-producer/single-consumer, lock-free ring
buffer: cache-line-aligned entries (`alignas(64)`), fixed-capacity storage
allocated once at `initialize()`, and zero allocation on `ingest_packet()`
or `poll()`. `replay_bench` measures the *tick-to-ring* latency -- the time
from a packet's send timestamp to the instant it is visible in the ring --
under sustained load from a dedicated producer core against a dedicated
consumer core.

## Prerequisites

- Linux, x86_64, kernel 5.x+ with an invariant TSC
  (`grep constant_tsc /proc/cpuinfo` should show it on every core).
- GCC 12+ or Clang 15+, CMake 3.20+.
- At least 4 logical cores (`replay_bench` pins to cores 2 and 3 by
  default; pass two different core IDs as arguments to target another
  pair, e.g. an isolated pair on a larger box).
- Optional, for the Python bridge only: Python 3.8+ and `nanobind`
  (`pip install nanobind`).

## Build and run (under 2 minutes)

```sh
./run_bench.sh                  # configures, builds, and runs in one step
./run_bench.sh 4 5               # override: producer=core 4, consumer=core 5
```

Or run the three steps yourself if you want to inspect the build in between:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/replay_bench            # default: producer=core 2, consumer=core 3
./build/replay_bench 4 5        # override: producer=core 4, consumer=core 5
```

`replay_bench` pre-generates 1,050,000 synthetic packets in memory (a
50,000-packet warm-up burst plus the 1,000,000-packet measured corpus)
before starting the timed run, so neither `malloc` nor disk I/O appears in
the measured loop. Producer and consumer threads run concurrently, pinned
to the two cores you specify, for the whole run. Output:

```
Tick-to-ring latency (send_timestamp -> ring push), 1000000 samples:
  -------------------------------------------------
  min                XX.X ns
  p50                XX.X ns
  p90                XX.X ns
  p99                XX.X ns
  p99.9              XX.X ns
  max                XX.X ns
  -------------------------------------------------

Throughput and loss (X.XXXs send window, X.XXXs to full drain):
  -------------------------------------------------
  packets ingested:     1050000
  packets dropped:      0
  packets malformed:    0
  packets consumed:     1050000
  sustained throughput: XX.XXX M msgs/sec
  zero-loss verification: PASS
  -------------------------------------------------
```

**Run it yourself and report your own numbers.** Latency figures are only
meaningful measured on quiet, tuned bare metal with the CPU governor pinned
to `performance` (see below) -- a shared CI runner, a laptop on battery, or
an unpinned VM will produce numbers dominated by scheduler noise, not the
engine. This kit exists so you can verify performance independently rather
than take a headline number on faith.

### CPU governor

Frequency scaling is the largest source of run-to-run latency variance on
an otherwise idle box:

```sh
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor   # inspect
for cpu in /sys/devices/system/cpu/cpu[0-9]*; do            # force max P-state (root)
    echo performance | sudo tee "$cpu/cpufreq/scaling_governor" > /dev/null
done
```

For sharper isolation, boot with `isolcpus`/`nohz_full` covering the
producer/consumer core pair so no other scheduled task lands on them
during the run.

## Python bridge (optional)

```sh
pip install nanobind
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DANIMUS_EVAL_BUILD_PYTHON=ON
cmake --build build -j
```

```python
import numpy as np
import struct
import sys
sys.path.insert(0, "build")  # or wherever animus_eval_native.*.so landed
import animus_eval_native as ae

stream = ae.ReplayStream(cpu_core=2, ring_capacity=1 << 16, drain_batch_capacity=8192)

packet = struct.pack("<QQqIIBB2x", 0, 1, 100_000_000_000, 7, 500, 0, 0)
stream.ingest(np.frombuffer(packet, dtype=np.uint8))

view = stream.poll(max_count=1024)          # zero-copy view, valid until the next poll()
dtype = np.dtype(ae.WIRE_FORMAT)
records = np.frombuffer(view, dtype=dtype)  # zero-copy structured view over the same memory
print(stream.telemetry())
```

`view`/`records` alias `ReplayStream`'s internal scratch buffer -- copy
what you need (`records.copy()`) before calling `poll()` again if the data
must outlive that call.

## Contact

Questions about methodology, reproducing a result, or extending the
harness for your own replay data: reach out to the author at
royrichie006@gmail.com.
