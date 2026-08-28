# Animus Core v1.0: High-Performance Event Processing Engine

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Python: 3.8+](https://img.shields.io/badge/python-3.8+-blue.svg)
![C++: 17}hhttps://img.shields.io/badge/C+%-17-blue.svg)

## Overview
Animus Core is an enterprise-grade, low-latency telemetry ingestion and automated response engine engineered in C++ with native Python SDK bindings. It bridges C-ABI execution memory boundaries with high-level orchestrators to process high-throughput telemetry streams without zero-copy buffer degradation.

## Key Architectural Principles
* **Direct C-ABI Shared Library Interop:** Bypasses IPC overhead by loading native compiled binaries directly (.dll / .so).
* **Deterministic Execution:** Engineered for high-frequency telemetry parsing and automated mitigation.
* **Zero-Dependency SDK Integration:** Packaged as an installable Python SDK (`pip install -e .`) for seamless staging and production pilots.

## Quick Start

```bash
# Install sdk bindings in editable mode
pip install -e .

# Run SDK validation
python test_sdk.py

3 Run production benchmark suite
python benchmark_suite.py
 ```

## Benchmark Performance
* **Peak Throughput:** >238 Million ops/sec
* **Latency Profile:** Sub-millisecond batch ingestion

