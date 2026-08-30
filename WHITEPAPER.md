# Animus Core v1.1.0: Technical Architecture & Low-Latency Event Processing Engine

**Author:** Alakshendra Roy | Founder & Core Architect  
**Classification:** Institutional Technical Specification  
**Target Audience:** Chief Technology Officers, Quantitative Infrastructure Architects  

---

## Executive Summary
Animus Core is an enterprise-grade telemetry ingestion and automated execution engine engineered in C++ with real-time Python SDK interop. Designed specifically for high-frequency trading desks and quantitative execution platforms, Animus eliminates the trade-off between low-level execution speed and modern language accessibility.

## Key Architectural Principles
* **Direct C-ABI Shared Library Interop:** Bypasses IPC overhead by loading native compiled binaries directly into high-level runtimes.
* **Deterministic Execution:** Zero dynamic heap allocation post-initialization (0 Bytes on hot path) to eliminate GC pauses and latency jitter.
* **Zero-Dependency SDK Integration:** Packaged as an installable Python SDK for seamless pilots.
* **Security Hardening:** Hardware-Bound RSA-2048 licensing enforcement ensuring zero performance degradation on shared memory rings.

## Core Performance Metrics
* **P50 Latency (Median):** 0.84 µs
* **P99.9 Latency (Tail):** < 1.85 µs
* **Max Sustained Throughput:** 8.21M events/sec
* **Memory Isolation Footprint:** < 16 MB Static Overhead