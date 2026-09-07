# animus-py

Zero-copy Python SDK for Animus Core's C++20 SPSC ring buffer. Pulls
`TelemetryFrame` records (the same 64-byte wire format `animus_bench`
measures) directly into NumPy, with no per-frame Python object
construction and no heap allocation on the ingestion hot path.

## Install

From a full source checkout, with [nanobind](https://github.com/wjakob/nanobind) available to the target interpreter:

```bash
pip install ./sdk/python
```

For fast local iteration without a wheel build, see the CMake steps at the
bottom of `CMakeLists.txt` in this directory.

## Quickstart

```python
from animus_sdk import AnimusRingBuffer, AnimusConsumer

ring = AnimusRingBuffer(capacity=1 << 20)
consumer = AnimusConsumer(ring)

ring.start_synthetic_load(target_frames_per_sec=0)  # unthrottled synthetic load
frames = consumer.to_numpy(consumer.drain())          # zero-copy structured ndarray
print(frames.dtype.names, len(frames))
ring.stop_synthetic_load()
```

## Benchmark

```bash
python bench_python_throughput.py --duration 5
```

Prints the real, measured sustained ingestion rate on the machine it runs
on -- see that script's own header comment for why no number is
hardcoded.

See `../../docs/INSTITUTIONAL_INTEGRATION_GUIDE.md` for the full
architectural background (memory ordering, cache-line isolation, kernel
tuning for production deployments).
