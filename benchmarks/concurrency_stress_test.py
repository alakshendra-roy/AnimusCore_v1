"""Phase 22: concurrent multi-tenant stress hardening -- orchestrator.

Launches AnimusCore_v1/concurrency_stress_consumer.exe (N tenant threads,
each its own ShmRing<RawEvent> + Engine) and --tenants genuinely separate
Python producer processes (concurrency_stress_producer.py, one per
tenant) concurrently, repeated for --runs consecutive runs, and prints a
representative-run + range-across-N-runs summary (same format
benchmarks/generate_benchmark_report.py already uses).

Build the consumer first (not part of the default MSBuild/CMake targets,
same convention as every other standalone demo/bench .cpp in this repo):
  cl /std:c++17 /EHsc /O2 AnimusCore_v1/concurrency_stress_consumer.cpp ^
     /Fe:AnimusCore_v1/concurrency_stress_consumer.exe

Run with:
    python benchmarks/concurrency_stress_test.py
"""
import argparse
import json
import os
import subprocess
import sys
import time
from typing import Any, Dict, List

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

_REPO_ROOT = os.path.join(os.path.dirname(__file__), "..")
_CONSUMER_EXE = os.path.join(_REPO_ROOT, "AnimusCore_v1", "concurrency_stress_consumer.exe")
_PRODUCER_SCRIPT = os.path.join(os.path.dirname(__file__), "concurrency_stress_producer.py")


def run_once(tenants: int, events_per_tenant: int, ring_capacity: int, timeout_s: float) -> Dict[str, Any]:
    consumer = subprocess.Popen(
        [_CONSUMER_EXE, str(tenants), str(events_per_tenant), str(ring_capacity)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    # A short, best-effort head start -- not load-bearing for correctness,
    # since each producer's own open() retry loop (concurrency_stress_producer.py)
    # already absorbs the real startup race; this just avoids every producer
    # spending its first several retries spinning for nothing.
    time.sleep(0.3)

    producers = [
        subprocess.Popen(
            [sys.executable, _PRODUCER_SCRIPT, str(tenant_id), str(events_per_tenant)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        for tenant_id in range(tenants)
    ]
    for p in producers:
        _, p_stderr = p.communicate(timeout=timeout_s)
        if p.returncode != 0:
            print(p_stderr, file=sys.stderr)
            raise RuntimeError(f"producer exited {p.returncode}")

    stdout, stderr = consumer.communicate(timeout=timeout_s)
    if consumer.returncode not in (0, 1):  # 1 = all_tenants_ok:false, still a real result to report
        print(stderr, file=sys.stderr)
        raise RuntimeError(f"consumer exited unexpectedly: {consumer.returncode}")

    for line in stdout.splitlines():
        prefix = "BENCHMARK_RESULT_CONCURRENCY_STRESS "
        if line.startswith(prefix):
            return json.loads(line[len(prefix):])
    print(stderr, file=sys.stderr)
    raise RuntimeError("consumer produced no BENCHMARK_RESULT_CONCURRENCY_STRESS line")


def _fmt_range(values: List[float], fmt: str = "{:.1f}") -> str:
    return f"{fmt.format(min(values))} - {fmt.format(max(values))}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Phase 22 concurrent multi-tenant stress test")
    parser.add_argument("--tenants", type=int, default=8)
    parser.add_argument("--events-per-tenant", type=int, default=250_000)
    parser.add_argument("--ring-capacity", type=int, default=65536)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    if not os.path.exists(_CONSUMER_EXE):
        print(
            f"Consumer binary not found at {_CONSUMER_EXE!r} -- build it first:\n"
            f"  cl /std:c++17 /EHsc /O2 AnimusCore_v1/concurrency_stress_consumer.cpp "
            f"/Fe:AnimusCore_v1/concurrency_stress_consumer.exe",
            file=sys.stderr,
        )
        return 1

    results: List[Dict[str, Any]] = []
    for run in range(1, args.runs + 1):
        print(f"=== run {run}/{args.runs} ===")
        result = run_once(args.tenants, args.events_per_tenant, args.ring_capacity, args.timeout)
        print(json.dumps(result))
        results.append(result)

    print("\n" + "=" * 70)
    print(f"Summary across {len(results)} runs "
          f"({args.tenants} tenants x {args.events_per_tenant:,} events each = "
          f"{args.tenants * args.events_per_tenant:,} total per run)")
    print("=" * 70)

    all_ok = all(r["all_tenants_ok"] for r in results)
    throughputs = [r["combined_throughput_eps"] for r in results]
    growths = [r["growth_during_load_pct"] for r in results]
    peaks = [r["peak_rss_mb"] for r in results]
    residuals = [r["post_teardown_residual_mb"] for r in results]

    # Two different memory questions (see concurrency_stress_consumer.cpp's
    # own comment on why these aren't conflated into one growth %):
    # growth_during_load_pct is warm->peak WHILE all tenants are still
    # concurrently active (a real leak grows this without bound);
    # post_teardown_residual_mb is memory left over AFTER every tenant has
    # finished and freed its own Engine/ShmRing -- checked for run-to-run
    # STABILITY here, not for shrinking back to zero (some fixed
    # CRT/allocator/DLL overhead never fully returns to the OS, and that's
    # expected, not a leak).
    residual_spread = max(residuals) - min(residuals) if residuals else 0.0
    residual_stable = residual_spread < 0.25 * max(residuals) if residuals and max(residuals) > 0 else True

    print(f"All tenants zero-loss, every run:      {'PASS' if all_ok else 'FAIL'} ({sum(r['all_tenants_ok'] for r in results)}/{len(results)})")
    print(f"Combined throughput (events/sec):      representative={throughputs[0]:,.0f}  range={_fmt_range(throughputs, '{:,.0f}')}")
    print(f"Peak RSS during load (MB):             representative={peaks[0]:.2f}  range={_fmt_range(peaks, '{:.2f}')}")
    print(f"Growth during load, warm->peak:         representative={growths[0]:.2f}%  range={_fmt_range(growths, '{:.2f}')}%")
    print(f"Post-teardown residual (MB):           representative={residuals[0]:.2f}  range={_fmt_range(residuals, '{:.2f}')}")
    print(f"Residual stable across runs (no leak): {'PASS' if residual_stable else 'FAIL (residual varies by >25% across runs)'}")
    print("=" * 70)

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
