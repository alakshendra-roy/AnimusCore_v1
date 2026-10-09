#!/usr/bin/env python3
"""Summarise the JSON reports written by benchmarks/telemetry_benchmark.cpp in this directory.

    python summarise_runs.py [DIR]            # DIR defaults to this file's directory

File names are <build>_<kind>_<producer>-<consumer>_run<N>.json, with kind "sep" (producer and consumer on separate physical cores) or
"smt" (the two hyperthreads of one physical core). Prints min / median / max of latency (ns) and flood throughput (M msgs/s) per build
and core pair and pooled. Python 3.8+, standard library only.
"""
import glob
import json
import os
import statistics as st
import sys

KEYS = (("p50", "p50"), ("p90", "p90"), ("p99", "p99"), ("p99_9", "p99.9"), ("thr", "flood M/s"))


def load(directory):
    runs = []
    for path in sorted(glob.glob(os.path.join(directory, "*.json"))):
        j = json.load(open(path, encoding="utf-8"))
        build, kind, pair, run = os.path.basename(path)[:-5].split("_")
        runs.append(dict(build=build, kind=kind, pair=pair, run=run, compiler=j["system"]["compiler"], at=j["generated_at_utc"],
                         producer=j["parameters"]["producer_core"], consumer=j["parameters"]["consumer_core"],
                         thr=j["throughput"]["million_events_per_sec"], **j["latency_ns"]))
    return runs


def mmm(runs, key):
    v = [r[key] for r in runs]
    return min(v), st.median(v), max(v)


def line(label, runs):
    parts = ["%-26s n=%2d" % (label, len(runs))]
    for key, name in KEYS:
        lo, mid, hi = mmm(runs, key)
        parts.append("%s %7.1f / %7.1f / %7.1f" % (name.ljust(9), lo, mid, hi))
    return "  ".join(parts)


def main(directory):
    runs = load(directory)
    print("min / median / max   (latency in ns; flood throughput in M msgs/s)\n")
    for kind, title in (("sep", "SEPARATE physical Performance cores"), ("smt", "ONE physical core, two hyperthreads (the harness default pair 0/1)")):
        sel = [r for r in runs if r["kind"] == kind]
        if not sel:
            continue
        print("== " + title)
        for build in sorted({r["build"] for r in sel}):
            for pair in sorted({r["pair"] for r in sel if r["build"] == build}, key=lambda s: tuple(int(x) for x in s.split("-"))):
                print(line("%s cores %s" % (build, pair), [r for r in sel if r["build"] == build and r["pair"] == pair]))
            print(line("%s pooled" % build, [r for r in sel if r["build"] == build]))
        print(line("both builds pooled", sel))
        print()
    print("compilers:", sorted({r["compiler"] for r in runs}), "| runs from", min(r["at"] for r in runs), "to", max(r["at"] for r in runs))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
