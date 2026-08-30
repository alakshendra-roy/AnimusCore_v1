#!/usr/bin/env python3
"""CI regression gate for AnimusCore's tick-to-trade latency benchmark.

Reads benchmark_results.json -- the BENCHMARK_RESULT_TICK_TO_TRADE payload
that AnimusCore_v1/animus_benchmark_suite.cpp prints and
.github/workflows/benchmark.yml extracts -- and fails the build if median
(p50) tick-to-trade latency has regressed past the fixed budget below.
"""
import json
import sys
from pathlib import Path

RESULTS_PATH = Path("benchmark_results.json")
P50_BUDGET_NS = 1000.0


def main() -> int:
    if not RESULTS_PATH.exists():
        print(
            f"ERROR: {RESULTS_PATH} not found -- did the benchmark step run first?",
            file=sys.stderr,
        )
        return 1

    try:
        data = json.loads(RESULTS_PATH.read_text())
    except json.JSONDecodeError as exc:
        print(f"ERROR: {RESULTS_PATH} is not valid JSON: {exc}", file=sys.stderr)
        return 1

    if "p50_ns" not in data:
        print(
            f"ERROR: {RESULTS_PATH} has no 'p50_ns' field (keys found: {sorted(data.keys())})",
            file=sys.stderr,
        )
        return 1

    p50_ns = float(data["p50_ns"])

    if p50_ns <= P50_BUDGET_NS:
        print(f"OK: p50 tick-to-trade latency {p50_ns:.1f}ns is within the {P50_BUDGET_NS:.0f}ns budget.")
        return 0

    over_pct = (p50_ns / P50_BUDGET_NS - 1.0) * 100.0
    print(
        "PERFORMANCE REGRESSION DETECTED\n"
        "  Metric:  p50 tick-to-trade latency\n"
        f"  Budget:  <= {P50_BUDGET_NS:.0f}ns\n"
        f"  Actual:  {p50_ns:.1f}ns\n"
        f"  Delta:   +{p50_ns - P50_BUDGET_NS:.1f}ns ({over_pct:.1f}% over budget)\n"
        f"  Source:  {RESULTS_PATH}",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
