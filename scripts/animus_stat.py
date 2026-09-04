#!/usr/bin/env python3
"""animus_stat -- Milestone 3 (Non-Blocking Telemetry & Observability Layer).

Zero-dependency (stdlib only, matching CLAUDE.md's Python SDK constraint)
live dashboard / Prometheus exporter for animus::sys::ipc::ShmRing<T> (SPSC)
and SpmcRing<T> (SPMC broadcast) segments (include/animus/shm_ipc.hpp).

Mirrors the wire-descriptor header byte-for-byte by hand -- the same
"no C++ toolchain at runtime" convention benchmarks/consumer.py already
uses for its own, narrower ExecutionEvent-only reader. Keep the offsets
below in sync with shm_ipc.hpp's RingHeader/SpmcRingHeader if either ever
changes; tests/test_animus_stat.py is what actually catches drift, not
this comment.

Never attaches as a consumer: every field read below is a plain
struct.unpack_from read of already-published bytes, with no write back to
the segment at all -- the same "sample without side effects" contract
include/animus/telemetry.hpp's AnimusGetMetrics() implements on the C++
side. This script is an independent Python mirror of that same contract
(not a wrapper around it), so it works even where the nanobind extension
isn't built.

Usage:
    python scripts/animus_stat.py --name my_ring                # live dashboard
    python scripts/animus_stat.py --name my_ring --once          # single sample
    python scripts/animus_stat.py --name my_ring --prometheus    # OpenMetrics text
    python scripts/animus_stat.py                                # auto-discover /dev/shm (POSIX only)
"""
from __future__ import annotations

import argparse
import dataclasses
import os
import struct
import sys
import time
from multiprocessing import shared_memory
from typing import Dict, List, Optional

# --- Wire layout: must match include/animus/shm_ipc.hpp's RingHeader / ----
# SpmcRingHeader exactly. Both share an identical prefix (capacity through
# wire_format) by construction -- see RingKind's own comment in that header
# -- so the prefix format/offsets below apply to either ring kind; only
# what comes after wire_format differs.
_PREFIX_FORMAT = "<QQQQQQ"  # capacity, mask, ring_kind, schema_version_hash, payload_size, stride
_PREFIX_SIZE = struct.calcsize(_PREFIX_FORMAT)
assert _PREFIX_SIZE == 48

_WIRE_FORMAT_OFF = 48
_WIRE_FORMAT_SIZE = 80
assert _WIRE_FORMAT_OFF + _WIRE_FORMAT_SIZE == 128

RING_KIND_SPSC = 0
RING_KIND_SPMC = 1

# SPSC (RingHeader): head/dropped_count/producer_pid/producer_heartbeat on
# one cache line, tail/consumer_pid/consumer_heartbeat on the next.
_SPSC_HEAD_OFF = 128
_SPSC_DROPPED_COUNT_OFF = 136
_SPSC_PRODUCER_PID_OFF = 144
_SPSC_PRODUCER_HEARTBEAT_OFF = 152
_SPSC_TAIL_OFF = 192
_SPSC_HEADER_SIZE = 256

# SPMC (SpmcRingHeader): head/producer_pid/producer_heartbeat only -- no
# shared tail, no shared drop counter (see SpmcRing<T>'s own class comment,
# shm_ipc.hpp, for why: both are per-consumer, local-only concepts in
# broadcast mode, unobservable from outside any one consumer).
_SPMC_HEAD_OFF = 128
_SPMC_PRODUCER_PID_OFF = 136
_SPMC_PRODUCER_HEARTBEAT_OFF = 144
_SPMC_HEADER_SIZE = 192


@dataclasses.dataclass
class RingSnapshot:
    """Mirrors include/animus/telemetry.hpp's TelemetrySnapshot -- see that
    struct's own field comments for the full rationale behind each one."""
    name: str
    valid: bool
    ring_kind: int = RING_KIND_SPSC
    capacity: int = 0
    schema_version_hash: int = 0
    payload_size: int = 0
    stride: int = 0
    wire_format: str = ""
    current_write_head: int = 0
    has_dropped_count: bool = False
    total_overruns: int = 0
    has_consumer_lag: bool = False
    consumer_tail: int = 0
    consumer_lag: int = 0
    producer_pid: int = 0
    producer_heartbeat: int = 0
    # time.monotonic() at sample time -- used locally (by this process) for
    # write-rate/drop-rate computation between successive samples; never
    # written anywhere or compared across processes/machines.
    sampled_at_monotonic: float = 0.0

    @property
    def ring_kind_name(self) -> str:
        return "spmc" if self.ring_kind == RING_KIND_SPMC else "spsc"

    @property
    def total_pushed_events(self) -> int:
        return self.current_write_head

    @property
    def dropped_events(self) -> int:
        return self.total_overruns


def _read_u64(buf, offset: int) -> int:
    return struct.unpack_from("<Q", buf, offset)[0]


def sample_ring(name: str) -> RingSnapshot:
    """Reads one ring's diagnostic header without attaching as a consumer.

    Returns a RingSnapshot with valid=False if the segment doesn't exist or
    isn't a recognized animus ring header -- never raises for that case
    (matching AnimusGetMetrics()'s own "return false, don't throw"
    contract); a genuinely unexpected OS error still propagates.
    """
    try:
        shm = shared_memory.SharedMemory(name=name, create=False)
    except FileNotFoundError:
        return RingSnapshot(name=name, valid=False)

    try:
        buf = shm.buf
        if len(buf) < _PREFIX_SIZE:
            return RingSnapshot(name=name, valid=False)
        capacity, _mask, ring_kind, schema_version_hash, payload_size, stride = \
            struct.unpack_from(_PREFIX_FORMAT, buf, 0)
        wire_format_raw = bytes(buf[_WIRE_FORMAT_OFF:_WIRE_FORMAT_OFF + _WIRE_FORMAT_SIZE])
        wire_format = wire_format_raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")

        sampled_at = time.monotonic()

        if ring_kind == RING_KIND_SPSC:
            if len(buf) < _SPSC_HEADER_SIZE:
                return RingSnapshot(name=name, valid=False)
            head = _read_u64(buf, _SPSC_HEAD_OFF)
            tail = _read_u64(buf, _SPSC_TAIL_OFF)
            return RingSnapshot(
                name=name, valid=True, ring_kind=ring_kind, capacity=capacity,
                schema_version_hash=schema_version_hash, payload_size=payload_size,
                stride=stride, wire_format=wire_format,
                current_write_head=head,
                has_dropped_count=True,
                total_overruns=_read_u64(buf, _SPSC_DROPPED_COUNT_OFF),
                has_consumer_lag=True,
                consumer_tail=tail,
                consumer_lag=max(0, head - tail),
                producer_pid=_read_u64(buf, _SPSC_PRODUCER_PID_OFF),
                producer_heartbeat=_read_u64(buf, _SPSC_PRODUCER_HEARTBEAT_OFF),
                sampled_at_monotonic=sampled_at,
            )
        elif ring_kind == RING_KIND_SPMC:
            if len(buf) < _SPMC_HEADER_SIZE:
                return RingSnapshot(name=name, valid=False)
            return RingSnapshot(
                name=name, valid=True, ring_kind=ring_kind, capacity=capacity,
                schema_version_hash=schema_version_hash, payload_size=payload_size,
                stride=stride, wire_format=wire_format,
                current_write_head=_read_u64(buf, _SPMC_HEAD_OFF),
                producer_pid=_read_u64(buf, _SPMC_PRODUCER_PID_OFF),
                producer_heartbeat=_read_u64(buf, _SPMC_PRODUCER_HEARTBEAT_OFF),
                sampled_at_monotonic=sampled_at,
            )
        else:
            return RingSnapshot(name=name, valid=False)
    finally:
        shm.close()  # release this process's mapping -- read-only observation, never unlink()


def is_producer_alive(pid: int) -> bool:
    """Zero-dependency cross-platform pid liveness check -- same
    implementation as benchmarks/consumer.py's own is_process_alive(),
    duplicated rather than imported since that script isn't a package
    module and this one is meant to stay standalone/copy-pasteable."""
    if pid == 0:
        return False
    if sys.platform == "win32":
        import ctypes
        import ctypes.wintypes
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return False
        try:
            exit_code = ctypes.wintypes.DWORD()
            ok = ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code))
            return bool(ok) and exit_code.value == STILL_ACTIVE
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    else:
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True  # exists, just not ours to signal -- still alive


def discover_rings() -> List[str]:
    """Best-effort enumeration of candidate ring names. POSIX: every entry
    under /dev/shm is a candidate -- sample_ring() itself is what actually
    filters out anything that isn't a recognized animus ring header, so a
    false positive here just becomes a "(not a recognized animus ring)"
    row, never a crash. Windows has no equivalent filesystem-backed
    namespace for named CreateFileMapping objects reachable without extra
    privileges -- this returns an empty list there, and callers should ask
    for --name explicitly instead.
    """
    if sys.platform == "win32":
        return []
    shm_dir = "/dev/shm"
    try:
        return sorted(os.listdir(shm_dir))
    except OSError:
        return []


def _rate(curr: RingSnapshot, prev: Optional[RingSnapshot], curr_value: int, prev_value_attr: str) -> float:
    if prev is None or not prev.valid:
        return 0.0
    dt = curr.sampled_at_monotonic - prev.sampled_at_monotonic
    if dt <= 0:
        return 0.0
    prev_value = getattr(prev, prev_value_attr)
    return (curr_value - prev_value) / dt


def render_dashboard(snapshots: Dict[str, RingSnapshot], prev: Dict[str, RingSnapshot]) -> str:
    lines = []
    if sys.stdout.isatty():
        lines.append("\033[2J\033[H")  # clear screen, home cursor -- only when actually interactive
    lines.append(f"animus_stat -- {time.strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(
        f"{'RING':<28} {'KIND':<5} {'CAPACITY':>10} {'HEAD':>12} {'RATE/s':>10} "
        f"{'DROPPED':>10} {'DROP/s':>8} {'LAG':>8} {'PRODUCER':<8}"
    )
    lines.append("-" * 104)
    for name, snap in snapshots.items():
        if not snap.valid:
            lines.append(f"{name:<28} (not a recognized animus ring -- skipped)")
            continue
        old = prev.get(name)
        rate = _rate(snap, old, snap.current_write_head, "current_write_head")
        dropped_str = str(snap.total_overruns) if snap.has_dropped_count else "n/a"
        if snap.has_dropped_count and old is not None and old.valid and old.has_dropped_count:
            drop_rate_str = f"{_rate(snap, old, snap.total_overruns, 'total_overruns'):.1f}"
        else:
            drop_rate_str = "n/a"
        lag_str = str(snap.consumer_lag) if snap.has_consumer_lag else "n/a"
        if snap.producer_pid == 0:
            producer_str = "none"
        else:
            producer_str = "alive" if is_producer_alive(snap.producer_pid) else "dead"
        lines.append(
            f"{name:<28} {snap.ring_kind_name:<5} {snap.capacity:>10} {snap.current_write_head:>12} "
            f"{rate:>10.1f} {dropped_str:>10} {drop_rate_str:>8} {lag_str:>8} {producer_str:<8}"
        )
    return "\n".join(lines)


def _escape_label_value(value: str) -> str:
    # OpenMetrics/Prometheus text format label-value escaping: backslash and
    # double-quote must be escaped; newlines become literal "\n".
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def format_prometheus(snapshots: List[RingSnapshot]) -> str:
    """Standard OpenMetrics/Prometheus plaintext exposition format --
    counters (events_total, dropped_total) stay raw cumulative counts, not
    pre-computed rates, matching Prometheus's own convention that rate()
    queries derive rates from a counter at scrape time, not the exporter."""
    lines = [
        "# HELP animus_ring_capacity Ring capacity in slots.",
        "# TYPE animus_ring_capacity gauge",
        "# HELP animus_ring_events_total Cumulative events written to the ring (head).",
        "# TYPE animus_ring_events_total counter",
        "# HELP animus_ring_dropped_total Cumulative events dropped (SPSC overwrite mode only; absent for SPMC).",
        "# TYPE animus_ring_dropped_total counter",
        "# HELP animus_ring_consumer_lag Events not yet read by the consumer (SPSC only; absent for SPMC).",
        "# TYPE animus_ring_consumer_lag gauge",
        "# HELP animus_ring_producer_alive Whether the producer process is currently running (1) or not (0).",
        "# TYPE animus_ring_producer_alive gauge",
    ]
    for snap in snapshots:
        if not snap.valid:
            continue
        ring = _escape_label_value(snap.name)
        labels = f'ring="{ring}",kind="{snap.ring_kind_name}"'
        lines.append(f"animus_ring_capacity{{{labels}}} {snap.capacity}")
        lines.append(f"animus_ring_events_total{{{labels}}} {snap.current_write_head}")
        if snap.has_dropped_count:
            lines.append(f"animus_ring_dropped_total{{{labels}}} {snap.total_overruns}")
        if snap.has_consumer_lag:
            lines.append(f"animus_ring_consumer_lag{{{labels}}} {snap.consumer_lag}")
        alive = 1 if is_producer_alive(snap.producer_pid) else 0
        lines.append(f"animus_ring_producer_alive{{{labels}}} {alive}")
    return "\n".join(lines) + "\n"


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", nargs="+", default=None,
                         help="explicit ring name(s) to sample (required on Windows; "
                              "auto-discovers /dev/shm on POSIX if omitted)")
    parser.add_argument("--interval", type=float, default=1.0,
                         help="seconds between refreshes for the live dashboard (default: 1.0)")
    parser.add_argument("--once", action="store_true",
                         help="sample once and exit instead of looping")
    parser.add_argument("--prometheus", action="store_true",
                         help="print OpenMetrics/Prometheus plaintext once instead of the live dashboard")
    args = parser.parse_args(argv)

    names = args.name
    if not names:
        names = discover_rings()
        if not names:
            if sys.platform == "win32":
                print("error: --name is required on Windows (no enumeration available for named mappings)",
                      file=sys.stderr)
            else:
                print("error: no segments found under /dev/shm -- pass --name explicitly", file=sys.stderr)
            return 1

    if args.prometheus:
        snapshots = [sample_ring(n) for n in names]
        sys.stdout.write(format_prometheus(snapshots))
        return 0

    prev: Dict[str, RingSnapshot] = {}
    try:
        while True:
            curr = {n: sample_ring(n) for n in names}
            print(render_dashboard(curr, prev))
            prev = curr
            if args.once:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
