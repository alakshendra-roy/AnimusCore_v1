"""Stress-tests the AnimusNative/AnimusCore_v1 engine along two independent
axes that benchmarks/benchmark_engine.py doesn't cover:

1. Sustained-load memory check (Part 1): pushes 1,000,000+ events through
   the full real pipeline (init -> add_rule -> start_logging ->
   record_events_batch in a loop -> stop_logging), sampling this process's
   resident memory (RSS) between batches. A real leak shows up as RSS
   growing roughly linearly with events processed; a one-time jump right
   after init (the ring buffer's fixed allocation) followed by a plateau
   is the expected, healthy shape.

2. C-ABI boundary fuzzing (Parts 2 and 3): feeds malformed input across
   animus_record_events_batch to check it's rejected safely rather than
   corrupting memory or crashing. Two different threat models, handled
   two different ways:

   - Part 2 (in-process, always safe): malformed *event data* through the
     public AnimusBindings.record_events_batch() API -- out-of-range
     field values, wrong tuple arity. struct.pack() validates every field
     before any ctypes/native call happens, so these can never corrupt
     memory; they're checked in-process.
   - Part 3 (subprocess-isolated, can genuinely crash): malformed *pointer/
     count* arguments against the raw C-ABI export (bypassing the SDK's
     `_lib` the way a caller never should, but a fuzzer must, to test the
     boundary itself). animus_record_events_batch has no way to verify a
     caller-supplied `count` against the buffer's real size -- same trust
     contract as memcpy -- so a lying `count` is a genuine out-of-bounds
     read that can and does segfault the process running it. Each such
     case therefore runs in its own subprocess so a real crash is
     *observed and reported*, not something that also kills this script.

Run with:
    python benchmarks/stress_test_engine.py
"""
import ctypes
import gc
import os
import signal as signal_module
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from typing import List, Tuple

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.bindings import AnimusBindings, NativeEvent, RuleComparator  # noqa: E402

TOTAL_EVENTS = 1_200_000  # ">1,000,000" with headroom, not exactly the boundary
BATCH_SIZE = 50_000
RING_CAPACITY = 1 << 17  # 131,072 -- bigger than one batch, so most batches push in one shot
RULE_EVENT_ID = 1
RULE_THRESHOLD = 90  # metric_value = i % 100, so ~9% of events match
RSS_GROWTH_FLAG_PCT = 10.0  # flag if final RSS exceeds the warm baseline (see below) by more than this


# ---------------------------------------------------------------------------
# Part 1: sustained load + memory leak check
# ---------------------------------------------------------------------------

def get_rss_bytes() -> int:
    """This process's resident set size, in bytes. stdlib-only (no psutil),
    matching this SDK's zero-dependency philosophy (see CLAUDE.md,
    animus/bindings.py's module docstring): calls the native Windows API
    directly on win32, falls back to /proc or the `resource` module
    elsewhere.
    """
    if sys.platform == "win32":
        from ctypes import wintypes

        class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.windll.kernel32
        psapi = ctypes.windll.psapi
        # Explicit argtypes/restype are required here: ctypes' default (c_int,
        # 32-bit) truncates GetCurrentProcess()'s 64-bit pseudo-handle
        # (0xFFFFFFFFFFFFFFFF) to a 32-bit value that then zero-extends back
        # out incorrectly when passed on, producing WinError 6 ("The handle
        # is invalid") -- reproduced and fixed while writing this function.
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        kernel32.GetCurrentProcess.argtypes = []
        psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
        psapi.GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), wintypes.DWORD,
        ]

        counters = PROCESS_MEMORY_COUNTERS()
        counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
        handle = kernel32.GetCurrentProcess()
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            raise ctypes.WinError()
        return counters.WorkingSetSize

    try:
        with open("/proc/self/status") as fh:
            for line in fh:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) * 1024
    except FileNotFoundError:
        pass
    import resource
    ru = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return ru if sys.platform == "darwin" else ru * 1024  # ru_maxrss is bytes on macOS, KB on Linux


def fmt_mb(nbytes: int) -> str:
    return f"{nbytes / (1024 * 1024):,.2f} MB"


@dataclass
class MemorySample:
    events_processed: int
    rss_bytes: int


def run_sustained_load_memory_check(total_events: int, batch_size: int) -> None:
    print(f"--- Sustained load: {total_events:,} events in batches of {batch_size:,} ---")

    bindings = AnimusBindings()
    if not bindings.using_native_engine:
        print(
            "WARNING: no compiled native engine found -- this run is exercising "
            "the pure-Python fallback, not the native C++ core."
        )

    gc.collect()
    cold_rss = get_rss_bytes()
    print(f"Cold baseline RSS (before init): {fmt_mb(cold_rss)}")

    bindings.init(buffer_capacity=RING_CAPACITY)
    bindings.add_rule(
        rule_id=1, event_id=RULE_EVENT_ID, threshold=RULE_THRESHOLD,
        comparator=RuleComparator.GREATER_THAN, severity=5,
    )

    samples: List[MemorySample] = []
    total_matches = 0

    with tempfile.TemporaryDirectory() as tmp:
        log_path = os.path.join(tmp, "stress_telemetry.log")
        bindings.start_logging(log_path)

        gc.collect()
        post_init_rss = get_rss_bytes()
        print(f"Post-init RSS (reference point for leak detection): {fmt_mb(post_init_rss)}")
        samples.append(MemorySample(0, post_init_rss))

        processed = 0
        num_batches = (total_events + batch_size - 1) // batch_size

        for batch_idx in range(num_batches):
            this_batch = min(batch_size, total_events - processed)
            events = [
                (RULE_EVENT_ID, processed + i, (processed + i) % 100)
                for i in range(this_batch)
            ]

            pushed = 0
            stall_iterations = 0
            while pushed < len(events):
                n = bindings.record_events_batch(events[pushed:])
                if n == 0:
                    # Ring momentarily full: drain signals to make room and retry.
                    total_matches += len(bindings.poll_signals(max_count=4096))
                    stall_iterations += 1
                    if stall_iterations > 10_000:  # ~10s of retries at 1ms each
                        print(f"  WARNING: batch {batch_idx} stalled -- ring buffer "
                              f"not draining, aborting sustained-load check early.")
                        bindings.stop_logging()
                        return
                    time.sleep(0.001)
                    continue
                pushed += n

            processed += this_batch
            total_matches += len(bindings.poll_signals(max_count=4096))

            rss = get_rss_bytes()
            samples.append(MemorySample(processed, rss))
            print(f"  {processed:>10,}/{total_events:,} events | RSS: {fmt_mb(rss)}")

        bindings.stop_logging()
        total_matches += len(bindings.poll_signals(max_count=1_000_000))

    gc.collect()
    time.sleep(0.05)
    final_rss = get_rss_bytes()
    samples.append(MemorySample(processed, final_rss))
    print(f"Final RSS (after stop_logging + gc.collect):  {fmt_mb(final_rss)}")
    print(f"Threat signals matched and drained: {total_matches:,}")

    # The leak-detection reference point is deliberately NOT post_init_rss.
    # The ring buffers are allocated (reserved) at init, but their pages are
    # only *committed* (and so counted in RSS) as each cell is first written
    # to -- so RSS legitimately climbs from post_init_rss through the first
    # cycle or two of the ring before plateauing, even with zero leaks. Using
    # post_init_rss as the baseline made this show up as a false "24.85%
    # growth" leak flag the first time this script ran; caught here, not
    # left in. warm_rss (after the ring has been fully cycled twice, so
    # every cell's page is provably resident) is the correct baseline.
    warm_threshold = min(2 * RING_CAPACITY, total_events)
    warm_sample = next((s for s in samples if s.events_processed >= warm_threshold), samples[-1])
    print(
        f"Warm baseline RSS (after {warm_sample.events_processed:,} events, "
        f">= 2x ring capacity so every cell's page is resident): {fmt_mb(warm_sample.rss_bytes)}"
    )

    growth = final_rss - warm_sample.rss_bytes
    growth_pct = (growth / warm_sample.rss_bytes * 100) if warm_sample.rss_bytes else 0.0
    print(f"\nRSS growth from warm baseline to final: {fmt_mb(growth)} ({growth_pct:+.2f}%)")
    if growth_pct > RSS_GROWTH_FLAG_PCT:
        print(
            f"FLAG: RSS grew by more than {RSS_GROWTH_FLAG_PCT:.0f}% between the "
            f"warm baseline and the end of the run -- investigate for a leak "
            "(this does not by itself prove one; allocator fragmentation can "
            "also produce modest growth, but this exceeds what that would "
            "normally explain)."
        )
    else:
        print(
            "No sustained growth beyond the ring buffers' one-time page-commit "
            "warm-up detected -- consistent with no leak."
        )


# ---------------------------------------------------------------------------
# Part 2: SDK-level malformed input (in-process -- struct.pack validates
# every field before any ctypes/native call, so these can never corrupt
# memory; there is nothing here that needs subprocess isolation).
# ---------------------------------------------------------------------------

def run_sdk_validation_checks() -> None:
    bindings = AnimusBindings()
    bindings.init(1024)

    cases: List[Tuple[str, list]] = [
        ("oversized_metric_value (2**64)", [(1, 1, 2**64)]),
        ("oversized_event_id (2**32)", [(2**32, 1, 1)]),
        ("negative_event_id (-1)", [(-1, 1, 1)]),
        ("negative_metric_value (-1)", [(1, 1, -1)]),
        ("wrong_arity_short_tuple (2 fields)", [(1, 1)]),
        ("wrong_arity_long_tuple (4 fields)", [(1, 1, 1, 1)]),
        ("non_integer_field ('bad')", [("bad", 1, 1)]),
    ]

    print(f"{'Case':<38}{'Outcome':<18}Detail")
    print("-" * 100)
    for name, events in cases:
        try:
            result = bindings.record_events_batch(events)
        except (struct.error, ValueError) as exc:
            print(f"{name:<38}{'SAFE (rejected)':<18}{type(exc).__name__}: {exc}")
        except Exception as exc:  # unexpected exception type -- worth flagging
            print(f"{name:<38}{'UNEXPECTED':<18}{type(exc).__name__}: {exc}")
        else:
            print(f"{name:<38}{'NOT REJECTED':<18}call returned {result} -- investigate")


# ---------------------------------------------------------------------------
# Part 3: raw C-ABI boundary fuzzing, one subprocess per case.
#
# _run_fuzz_case is dispatched to by re-invoking this same file with
# `--fuzz-case <name>` as a child process (see the __main__ block at the
# bottom) -- never call it directly in this process.
# ---------------------------------------------------------------------------

FUZZ_CASES: List[Tuple[str, str]] = [
    ("null_pointer_nonzero_count", "NULL event buffer, count=5"),
    ("uninitialized_engine", "called before animus_init (no engine yet)"),
    ("count_exceeds_buffer_moderate", "count claims 100,000 events; buffer actually holds 1"),
    ("count_exceeds_buffer_extreme", "count = 2**64-1; buffer actually holds 1"),
    ("negative_count", "count passed as a negative Python int (-1)"),
    ("empty_batch_via_sdk", "record_events_batch([]) via the public SDK"),
]


def _run_fuzz_case(name: str) -> None:
    """Runs exactly one malformed-input scenario against the raw C-ABI
    export. Only ever invoked in a child subprocess (see run_fuzz_case_isolated)
    -- several of these deliberately reproduce an out-of-bounds native read
    and can crash the interpreter that runs them.
    """
    if name == "null_pointer_nonzero_count":
        b = AnimusBindings()
        b.init(1024)
        r = b._lib.animus_record_events_batch(None, ctypes.c_size_t(5))
        print(f"RESULT pushed={r}")

    elif name == "uninitialized_engine":
        b = AnimusBindings()
        b._configure_signatures()  # wire up argtypes without calling animus_init
        buf = (NativeEvent * 3)()
        r = b._lib.animus_record_events_batch(buf, ctypes.c_size_t(3))
        print(f"RESULT pushed={r}")

    elif name == "count_exceeds_buffer_moderate":
        b = AnimusBindings()
        b.init(1 << 20)  # ring large enough that it won't fill before the OOB read goes far
        buf = (NativeEvent * 1)()  # buffer actually allocated for 1 event
        r = b._lib.animus_record_events_batch(buf, ctypes.c_size_t(100_000))  # lies: claims 100,000
        print(f"RESULT pushed={r}")

    elif name == "count_exceeds_buffer_extreme":
        b = AnimusBindings()
        b.init(1 << 20)
        buf = (NativeEvent * 1)()
        r = b._lib.animus_record_events_batch(buf, ctypes.c_size_t(0xFFFFFFFFFFFFFFFF))
        print(f"RESULT pushed={r}")

    elif name == "negative_count":
        b = AnimusBindings()
        b.init(1024)
        buf = (NativeEvent * 1)()
        r = b._lib.animus_record_events_batch(buf, ctypes.c_size_t(-1))
        print(f"RESULT pushed={r}")

    elif name == "empty_batch_via_sdk":
        b = AnimusBindings()
        b.init(1024)
        r = b.record_events_batch([])
        print(f"RESULT pushed={r}")

    else:
        raise ValueError(f"unknown fuzz case {name!r}")


_WINDOWS_CRASH_CODES = {
    0xC0000005: "STATUS_ACCESS_VIOLATION",
    0xC0000374: "STATUS_HEAP_CORRUPTION",
    0xC0000409: "STATUS_STACK_BUFFER_OVERRUN",
    0xC0000417: "STATUS_INVALID_CRT_PARAMETER",
    0xC0000602: "STATUS_FAIL_FAST_EXCEPTION",
}


def _classify_returncode(returncode: int, stderr: str) -> Tuple[str, str]:
    """Turns a subprocess's raw exit code (+ its stderr) into (outcome, detail).

    A process killed by a real crash never gets to print a Python traceback,
    so "no traceback in stderr" plus a recognized crash exit code is the
    signal used here, not the sign of the exit code -- on Windows, a crash
    (e.g. access violation) is reported as returncode=3221225477 (0xC0000005),
    a *positive* number, not the negative value some Python docs describe;
    verified empirically against a real crash before relying on it here.
    """
    if returncode == 0:
        return "SAFE", "process exited normally"

    unsigned32 = returncode & 0xFFFFFFFF
    if sys.platform == "win32" and unsigned32 in _WINDOWS_CRASH_CODES:
        return "CRASH", f"{_WINDOWS_CRASH_CODES[unsigned32]} (0x{unsigned32:08X})"

    if returncode < 0:
        # POSIX: a negative returncode means the process was killed by signal -returncode.
        try:
            sig_name = signal_module.Signals(-returncode).name
        except ValueError:
            sig_name = f"signal {-returncode}"
        return "CRASH", f"killed by {sig_name}"

    if "Traceback (most recent call last):" in stderr:
        last_line = stderr.strip().splitlines()[-1] if stderr.strip() else ""
        return "SAFE (rejected)", last_line

    return "UNKNOWN", f"nonzero exit {returncode} (0x{unsigned32:08X}) with no Python traceback"


@dataclass
class FuzzResult:
    name: str
    description: str
    outcome: str
    detail: str


# These two cases pass a `count` that's a real lie about the buffer's actual
# size -- the native call performs a genuine out-of-bounds read either way.
# When that read doesn't happen to cross into an unmapped page, the process
# doesn't crash, but the ring buffer still received garbage adjacent-memory
# bytes as if they were valid events: "didn't crash" is not "was safe" for
# these two. Confirmed non-deterministic across repeated runs of this exact
# script -- count_exceeds_buffer_moderate crashed in some runs and didn't in
# others, purely as a function of heap layout at the time, not anything
# about the input itself.
_OOB_READ_EVEN_WHEN_NOT_CRASHED = {"count_exceeds_buffer_moderate", "count_exceeds_buffer_extreme"}


def run_fuzz_case_isolated(name: str, description: str, timeout_s: float = 15.0) -> FuzzResult:
    try:
        proc = subprocess.run(
            [sys.executable, __file__, "--fuzz-case", name],
            capture_output=True, text=True, timeout=timeout_s,
        )
    except subprocess.TimeoutExpired:
        return FuzzResult(name, description, "TIMEOUT", f"did not exit within {timeout_s:.0f}s")

    outcome, detail = _classify_returncode(proc.returncode, proc.stderr)
    if outcome == "SAFE" and proc.stdout.strip():
        detail = proc.stdout.strip()
    if outcome == "SAFE" and name in _OOB_READ_EVEN_WHEN_NOT_CRASHED:
        outcome = "NO CRASH*"
        detail += " -- still a real OOB read of adjacent memory; got lucky on page layout this run, not actually safe"
    return FuzzResult(name, description, outcome, detail)


def run_boundary_fuzz_suite() -> None:
    print(f"{'Case':<32}{'Outcome':<18}Detail")
    print("-" * 100)
    results = []
    for name, description in FUZZ_CASES:
        result = run_fuzz_case_isolated(name, description)
        results.append(result)
        print(f"{result.name:<32}{result.outcome:<18}{result.detail}")

    crashes = [r for r in results if r.outcome == "CRASH"]
    unsafe_no_crash = [r for r in results if r.outcome == "NO CRASH*"]
    print()
    if crashes:
        print(
            f"{len(crashes)} of {len(results)} case(s) crashed the process running them: "
            + ", ".join(r.name for r in crashes)
        )
    if unsafe_no_crash:
        print(
            f"{len(unsafe_no_crash)} of {len(results)} case(s) didn't crash this run but are "
            "not actually safe (marked NO CRASH* above): " + ", ".join(r.name for r in unsafe_no_crash)
        )
    if crashes or unsafe_no_crash:
        print(
            "Both are expected for the count_exceeds_buffer_* cases: "
            "animus_record_events_batch has no way to validate a caller-supplied "
            "`count` against the buffer's real size (the same trust contract as "
            "memcpy), so a lying count is always a genuine out-of-bounds read -- "
            "whether that specific run crashes depends on heap layout at the time "
            "(confirmed non-deterministic by repeated runs of this script), not on "
            "anything about the input. The public AnimusBindings.record_events_batch() "
            "API is not reachable this way -- it always derives `count` from "
            "len(events) and sizes the buffer to match in the same call, so this "
            "class of bug requires bypassing the SDK and calling the raw `_lib` "
            "handle directly, which is exactly what these isolated fuzz cases "
            "(and no ordinary caller) do."
        )
    else:
        print("No cases crashed the process running them, and none performed an unguarded OOB read.")


# ---------------------------------------------------------------------------

def main() -> None:
    print("=" * 100)
    print("  ANIMUS ENGINE STRESS TEST")
    print("=" * 100)

    print(f"\n--- Part 1: Sustained load ({TOTAL_EVENTS:,} events) + memory leak check ---\n")
    run_sustained_load_memory_check(TOTAL_EVENTS, BATCH_SIZE)

    print("\n--- Part 2: SDK-level malformed input (in-process, safe by construction) ---\n")
    run_sdk_validation_checks()

    print("\n--- Part 3: Raw C-ABI boundary fuzzing (subprocess-isolated) ---\n")
    run_boundary_fuzz_suite()

    print("\n" + "=" * 100)
    print("  STRESS TEST COMPLETE")
    print("=" * 100)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--fuzz-case":
        _run_fuzz_case(sys.argv[2])
    else:
        main()
