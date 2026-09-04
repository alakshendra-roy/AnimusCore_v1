"""Unit tests for scripts/animus_stat.py -- Milestone 3 (Non-Blocking
Telemetry & Observability Layer).

Builds a real ring header directly with struct.pack_into (mirroring the
exact byte layout include/animus/shm_ipc.hpp's RingHeader/SpmcRingHeader
define), rather than depending on the compiled _animus_shm_native
extension -- keeps this test dependency-free and always-running (never
skipped) regardless of whether the nanobind extension has been built,
consistent with animus_stat.py's own "independent Python mirror, not a
wrapper" design (see that module's docstring).

Run with:
    python -m unittest discover -s tests
or:
    python -m pytest tests
"""
import os
import struct
import sys
import threading
import time
import unittest
from multiprocessing import shared_memory

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))

import animus_stat  # noqa: E402


def _round_up_pow2(v: int) -> int:
    p = 1
    while p < v:
        p <<= 1
    return p


class _FakeSpscRing:
    """A minimal stand-in for a real ShmRing<T>::create()'d segment --
    writes exactly the bytes animus_stat.sample_ring() expects at the
    offsets it expects them, using the same struct.pack_into approach
    benchmarks/consumer.py's own ShmExecutionConsumer uses for reading.
    schema_version_hash/payload_size/stride/wire_format are filled with
    harmless placeholder values: animus_stat.py never validates them
    against a compiled-in expectation (unlike ShmRing<T>::open() itself)
    -- it is a diagnostic tool meant to work on rings it was never
    compiled against, so it has nothing to validate them against in the
    first place.
    """

    def __init__(self, name: str, capacity: int, ring_kind: int = animus_stat.RING_KIND_SPSC):
        self.capacity = _round_up_pow2(capacity)
        self.mask = self.capacity - 1
        self.ring_kind = ring_kind
        size = animus_stat._SPSC_HEADER_SIZE if ring_kind == animus_stat.RING_KIND_SPSC else animus_stat._SPMC_HEADER_SIZE
        self.shm = shared_memory.SharedMemory(name=name, create=True, size=size)
        buf = self.shm.buf
        struct.pack_into(animus_stat._PREFIX_FORMAT, buf, 0,
                          self.capacity, self.mask, ring_kind,
                          0xDEADBEEF, 40, 40)
        wire_format = b"<QQqqII"
        buf[animus_stat._WIRE_FORMAT_OFF:animus_stat._WIRE_FORMAT_OFF + len(wire_format)] = wire_format

        head_off, pid_off, hb_off = self._offsets()
        self._set_u64(head_off, 0)
        self._set_u64(pid_off, os.getpid())
        self._set_u64(hb_off, 1)
        if ring_kind == animus_stat.RING_KIND_SPSC:
            self._set_u64(animus_stat._SPSC_DROPPED_COUNT_OFF, 0)
            self._set_u64(animus_stat._SPSC_TAIL_OFF, 0)

    def _offsets(self):
        if self.ring_kind == animus_stat.RING_KIND_SPSC:
            return animus_stat._SPSC_HEAD_OFF, animus_stat._SPSC_PRODUCER_PID_OFF, animus_stat._SPSC_PRODUCER_HEARTBEAT_OFF
        return animus_stat._SPMC_HEAD_OFF, animus_stat._SPMC_PRODUCER_PID_OFF, animus_stat._SPMC_PRODUCER_HEARTBEAT_OFF

    def _set_u64(self, offset: int, value: int) -> None:
        struct.pack_into("<Q", self.shm.buf, offset, value)

    def _get_u64(self, offset: int) -> int:
        return struct.unpack_from("<Q", self.shm.buf, offset)[0]

    def push(self) -> None:
        # Plain 8-byte-aligned load/store is atomic on x86/x64 hardware --
        # same reasoning benchmarks/consumer.py and animus/shm.py already
        # document for their own unrelated wire formats -- so this is a
        # genuine (if simplified) concurrent producer, safe to call from a
        # background thread while the main thread samples concurrently.
        head_off, _, _ = self._offsets()
        head = self._get_u64(head_off)
        self._set_u64(head_off, head + 1)

    def drop(self, n: int = 1) -> None:
        dropped = self._get_u64(animus_stat._SPSC_DROPPED_COUNT_OFF)
        self._set_u64(animus_stat._SPSC_DROPPED_COUNT_OFF, dropped + n)

    def close(self) -> None:
        self.shm.close()

    def unlink(self) -> None:
        self.shm.unlink()


class SampleRingTests(unittest.TestCase):
    def setUp(self):
        self.name = f"animus_stat_test_{os.getpid()}_{id(self)}"
        self.ring = _FakeSpscRing(self.name, capacity=64)
        self.addCleanup(self.ring.close)
        self.addCleanup(self.ring.unlink)

    def test_sample_ring_reads_a_static_snapshot_correctly(self):
        for _ in range(10):
            self.ring.push()
        self.ring.drop(3)

        snap = animus_stat.sample_ring(self.name)
        self.assertTrue(snap.valid)
        self.assertEqual(snap.ring_kind, animus_stat.RING_KIND_SPSC)
        self.assertEqual(snap.capacity, 64)
        self.assertEqual(snap.current_write_head, 10)
        self.assertEqual(snap.total_pushed_events, 10)
        self.assertTrue(snap.has_dropped_count)
        self.assertEqual(snap.total_overruns, 3)
        self.assertEqual(snap.dropped_events, 3)
        self.assertTrue(snap.has_consumer_lag)
        self.assertEqual(snap.consumer_lag, 10)
        self.assertEqual(snap.producer_pid, os.getpid())
        self.assertEqual(snap.wire_format, "<QQqqII")
        self.assertTrue(animus_stat.is_producer_alive(snap.producer_pid))

    def test_sample_ring_returns_invalid_for_a_nonexistent_segment(self):
        snap = animus_stat.sample_ring(f"{self.name}_does_not_exist")
        self.assertFalse(snap.valid)

    def test_sample_ring_accurately_tracks_a_live_producer_while_it_writes(self):
        # The actual requirement this milestone calls for: animus_stat
        # samples accurately WHILE an active producer is concurrently
        # writing, not just a snapshot of an already-finished ring.
        #
        # CPython's default GIL switch interval (sys.getswitchinterval(),
        # 5ms) is coarse enough that two CPU-bound pure-Python threads can
        # each run to completion within a single scheduling slice --
        # observed directly during development: even 200,000 producer
        # iterations sometimes finished inside one slice, starving the
        # sampling loop of a chance to interleave at all. Shrinking the
        # interval (restored in the finally block below) forces genuine
        # interleaving without needing an even larger, slower iteration
        # count to paper over it.
        original_switch_interval = sys.getswitchinterval()
        sys.setswitchinterval(0.0005)
        target_count = 200_000
        stop = threading.Event()
        pushed_count = [0]

        def producer_loop():
            while not stop.is_set() and pushed_count[0] < target_count:
                self.ring.push()
                pushed_count[0] += 1

        t = threading.Thread(target=producer_loop)
        t.start()
        try:
            samples = []
            deadline = time.monotonic() + 5.0
            # Busy-poll (no sleep) so as many samples as possible land
            # during the producer's active window -- this is a test, not
            # the live dashboard's own more leisurely --interval pacing.
            while pushed_count[0] < target_count and time.monotonic() < deadline:
                samples.append(animus_stat.sample_ring(self.name))
        finally:
            stop.set()
            t.join(timeout=5)
            sys.setswitchinterval(original_switch_interval)

        final = animus_stat.sample_ring(self.name)
        self.assertTrue(final.valid)
        self.assertEqual(pushed_count[0], target_count, "producer thread should have finished all pushes")
        self.assertEqual(final.current_write_head, pushed_count[0])

        heads = [s.current_write_head for s in samples if s.valid]
        self.assertGreaterEqual(len(heads), 2, "should have collected multiple samples while the producer was active")
        self.assertEqual(heads, sorted(heads), "sampled head must be monotonically non-decreasing")
        self.assertLess(heads[0], final.current_write_head,
                         "the first sample should have caught the ring mid-write, not already finished")

    def test_sample_ring_works_for_an_spmc_broadcast_header_too(self):
        spmc_name = f"{self.name}_spmc"
        ring = _FakeSpscRing(spmc_name, capacity=128, ring_kind=animus_stat.RING_KIND_SPMC)
        self.addCleanup(ring.close)
        self.addCleanup(ring.unlink)
        for _ in range(5):
            ring.push()

        snap = animus_stat.sample_ring(spmc_name)
        self.assertTrue(snap.valid)
        self.assertEqual(snap.ring_kind, animus_stat.RING_KIND_SPMC)
        self.assertEqual(snap.current_write_head, 5)
        self.assertFalse(snap.has_dropped_count, "SPMC has no shared drop counter")
        self.assertFalse(snap.has_consumer_lag, "SPMC has no shared consumer cursor")


class FormatPrometheusTests(unittest.TestCase):
    def test_valid_snapshot_produces_expected_metric_lines(self):
        snap = animus_stat.RingSnapshot(
            name="my_ring", valid=True, ring_kind=animus_stat.RING_KIND_SPSC,
            capacity=1024, current_write_head=500, has_dropped_count=True,
            total_overruns=7, has_consumer_lag=True, consumer_lag=12, producer_pid=0,
        )
        text = animus_stat.format_prometheus([snap])
        self.assertIn('animus_ring_capacity{ring="my_ring",kind="spsc"} 1024', text)
        self.assertIn('animus_ring_events_total{ring="my_ring",kind="spsc"} 500', text)
        self.assertIn('animus_ring_dropped_total{ring="my_ring",kind="spsc"} 7', text)
        self.assertIn('animus_ring_consumer_lag{ring="my_ring",kind="spsc"} 12', text)
        self.assertIn('animus_ring_producer_alive{ring="my_ring",kind="spsc"} 0', text)

    def test_spmc_snapshot_omits_dropped_and_lag_metrics(self):
        snap = animus_stat.RingSnapshot(
            name="broadcast_ring", valid=True, ring_kind=animus_stat.RING_KIND_SPMC,
            capacity=256, current_write_head=100, has_dropped_count=False, has_consumer_lag=False,
        )
        text = animus_stat.format_prometheus([snap])
        # The HELP/TYPE family declarations for dropped_total/consumer_lag
        # are legitimately always present (standard OpenMetrics practice --
        # declaring a metric family doesn't require emitting a series for
        # it every scrape); what must actually be absent is the DATA line.
        self.assertNotIn("animus_ring_dropped_total{", text)
        self.assertNotIn("animus_ring_consumer_lag{", text)
        self.assertIn('animus_ring_events_total{ring="broadcast_ring",kind="spmc"} 100', text)

    def test_invalid_snapshot_produces_no_metric_lines_for_that_ring(self):
        snap = animus_stat.RingSnapshot(name="missing", valid=False)
        text = animus_stat.format_prometheus([snap])
        self.assertNotIn("missing", text)


class RenderDashboardTests(unittest.TestCase):
    def test_dashboard_shows_not_recognized_for_an_invalid_snapshot(self):
        curr = {"missing": animus_stat.RingSnapshot(name="missing", valid=False)}
        text = animus_stat.render_dashboard(curr, {})
        self.assertIn("missing", text)
        self.assertIn("not a recognized animus ring", text)

    def test_rate_is_positive_between_two_increasing_samples(self):
        old = animus_stat.RingSnapshot(name="r", valid=True, current_write_head=100, sampled_at_monotonic=1000.0)
        new = animus_stat.RingSnapshot(name="r", valid=True, current_write_head=200, sampled_at_monotonic=1001.0)
        rate = animus_stat._rate(new, old, new.current_write_head, "current_write_head")
        self.assertEqual(rate, 100.0)

    def test_rate_is_zero_with_no_previous_sample(self):
        new = animus_stat.RingSnapshot(name="r", valid=True, current_write_head=200, sampled_at_monotonic=1001.0)
        rate = animus_stat._rate(new, None, new.current_write_head, "current_write_head")
        self.assertEqual(rate, 0.0)


if __name__ == "__main__":
    unittest.main()
