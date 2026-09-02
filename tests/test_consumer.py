"""Unit tests for animus.consumer -- the idiomatic Python wrapper over
bindings/animus_py.cpp's nanobind zero-copy telemetry stream.

Two tiers, split the same way animus.consumer itself splits its
dependency on the compiled extension:

  - Pure-Python tests (WireFormatTests, DecodeTests, ToNumpyTests) exercise
    decode()/decode_iter()/to_numpy()/TelemetryRecord against hand-built
    synthetic buffers. These need no compiled extension at all and always
    run.
  - TelemetryConsumerIntegrationTests drives a real _animus_native.
    TelemetryStream end to end (push/drain round trips, the background
    producer's lifecycle, dropped-event accounting under backpressure).
    Skipped, not failed, when the extension hasn't been built -- same
    convention tests/test_bindings.py uses for find_native_library().

Run with:
    python -m unittest discover -s tests
or:
    python -m pytest tests
"""
import os
import struct
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.consumer import (  # noqa: E402
    TelemetryConsumer,
    TelemetryRecord,
    WIRE_FORMAT,
    WIRE_RECORD_SIZE,
    decode,
    decode_iter,
    to_numpy,
)


def _native_extension_available() -> bool:
    try:
        from animus import _animus_native  # noqa: F401
    except ImportError:
        return False
    return True


def _pack_record(timestamp_cycles: int, event_id: int, trace_id: int, metric_value: int) -> bytes:
    return struct.pack(WIRE_FORMAT, timestamp_cycles, event_id, trace_id, metric_value)


class WireFormatTests(unittest.TestCase):
    """WIRE_FORMAT/WIRE_RECORD_SIZE must stay mutually consistent and
    match animus::SharedTelemetryRecord's 24-byte <QIIQ> layout (animus.hpp
    static_asserts the C++ side to exactly this; nothing enforces the
    Python side matching it except this test and consumer.py's own
    module-level assert)."""

    def test_wire_record_size_matches_struct_calcsize(self):
        self.assertEqual(struct.calcsize(WIRE_FORMAT), WIRE_RECORD_SIZE)

    def test_wire_format_is_the_documented_shared_telemetry_record_layout(self):
        self.assertEqual(WIRE_FORMAT, "<QIIQ")
        self.assertEqual(WIRE_RECORD_SIZE, 24)


class DecodeTests(unittest.TestCase):
    """decode()/decode_iter() against hand-built buffers -- no native
    extension involved, just verifying the unpack logic itself."""

    def test_decode_empty_buffer_returns_empty_list(self):
        self.assertEqual(decode(b""), [])

    def test_decode_single_record(self):
        raw = _pack_record(111, 2, 3, 400000)
        records = decode(raw)
        self.assertEqual(records, [TelemetryRecord(111, 2, 3, 400000)])

    def test_decode_multiple_records_preserves_order(self):
        raw = b"".join(_pack_record(i, i % 256, i * 7, 1000 + i) for i in range(50))
        records = decode(raw)
        self.assertEqual(len(records), 50)
        for i, rec in enumerate(records):
            self.assertEqual(rec, TelemetryRecord(i, i % 256, i * 7, 1000 + i))

    def test_decode_iter_yields_same_records_as_decode(self):
        raw = b"".join(_pack_record(i, i, i, i) for i in range(10))
        self.assertEqual(list(decode_iter(raw)), decode(raw))

    def test_decode_accepts_a_memoryview_not_just_bytes(self):
        # drain() returns a memoryview in real use, not bytes -- decode()
        # must accept whatever supports the buffer protocol, not assume bytes.
        raw = _pack_record(1, 2, 3, 4)
        view = memoryview(bytearray(raw))
        self.assertEqual(decode(view), [TelemetryRecord(1, 2, 3, 4)])

    def test_decode_field_types_are_plain_int_not_numpy_or_ctypes_scalars(self):
        raw = _pack_record(1, 2, 3, 4)
        rec = decode(raw)[0]
        for value in rec:
            self.assertIs(type(value), int)

    def test_telemetry_record_field_order_matches_wire_format(self):
        # (timestamp_cycles, event_id, trace_id, metric_value) -- <QIIQ>
        rec = TelemetryRecord(timestamp_cycles=1, event_id=2, trace_id=3, metric_value=4)
        self.assertEqual(tuple(rec), (1, 2, 3, 4))


class ToNumpyTests(unittest.TestCase):
    """to_numpy() against hand-built buffers -- also no native extension
    needed, only real numpy (skipped if not installed, same as this
    project treats numpy everywhere else: opt-in, never assumed)."""

    def setUp(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest("numpy not installed -- to_numpy() is opt-in, see its own ImportError test below")

    def test_to_numpy_decodes_fields_correctly(self):
        raw = b"".join(_pack_record(i, i % 256, i * 3, 5000 + i) for i in range(20))
        arr = to_numpy(raw)
        self.assertEqual(len(arr), 20)
        for i in range(20):
            self.assertEqual(int(arr[i]["timestamp_cycles"]), i)
            self.assertEqual(int(arr[i]["event_id"]), i % 256)
            self.assertEqual(int(arr[i]["trace_id"]), i * 3)
            self.assertEqual(int(arr[i]["metric_value"]), 5000 + i)

    def test_to_numpy_is_actually_zero_copy(self):
        # A real memoryview over a mutable buffer: mutate the source after
        # the fact and the numpy view must see the mutation, proving it
        # aliases the same memory rather than having copied it.
        buf = bytearray(_pack_record(1, 2, 3, 4))
        view = memoryview(buf)
        arr = to_numpy(view)
        self.assertEqual(int(arr[0]["event_id"]), 2)
        struct.pack_into(WIRE_FORMAT, buf, 0, 1, 999, 3, 4)
        self.assertEqual(int(arr[0]["event_id"]), 999, "to_numpy() must alias the source buffer, not copy it")


class ToNumpyMissingDependencyTest(unittest.TestCase):
    def test_to_numpy_raises_clear_import_error_without_numpy(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            pass
        else:
            self.skipTest("numpy is installed in this environment -- can't exercise the missing-dependency path")
        with self.assertRaises(ImportError) as ctx:
            to_numpy(_pack_record(1, 2, 3, 4))
        self.assertIn("numpy", str(ctx.exception))


@unittest.skipUnless(_native_extension_available(),
                      "no compiled _animus_native extension found; build it via `pip install ./bindings` "
                      "or bindings/CMakeLists.txt's direct-CMake steps first")
class TelemetryConsumerIntegrationTests(unittest.TestCase):
    """Drives a real TelemetryStream end to end. Small capacities/counts
    throughout -- this is a correctness suite, not a benchmark (see
    benchmarks/telemetry_benchmark.cpp / examples/live_stream.py for that)."""

    def test_construction_rejects_nothing_for_small_capacities(self):
        # capacity/drain_batch_capacity=0 must not crash (TelemetryStream's
        # constructor clamps drain_batch_capacity to at least 1; the ring
        # rounds capacity up to at least 2 -- see bindings/animus_py.cpp
        # and animus::SpscRingBuffer::round_up_pow2).
        stream = TelemetryConsumer(capacity=0, drain_batch_capacity=0)
        self.assertGreaterEqual(stream.capacity, 1)
        self.assertGreaterEqual(stream.drain_batch_capacity, 1)

    def test_capacity_rounds_up_to_a_power_of_two(self):
        stream = TelemetryConsumer(capacity=100, drain_batch_capacity=64)
        self.assertEqual(stream.capacity, 128)

    def test_push_then_drain_round_trips_exact_values(self):
        stream = TelemetryConsumer(capacity=256, drain_batch_capacity=256)
        self.assertTrue(stream.push(event_id=7, trace_id=42, metric_value=123456))
        records = stream.poll()
        self.assertEqual(len(records), 1)
        rec = records[0]
        self.assertEqual((rec.event_id, rec.trace_id, rec.metric_value), (7, 42, 123456))
        self.assertGreater(rec.timestamp_cycles, 0)

    def test_drain_on_empty_ring_returns_an_empty_view(self):
        stream = TelemetryConsumer(capacity=64, drain_batch_capacity=64)
        view = stream.drain()
        self.assertEqual(view.shape[0], 0)

    def test_drain_respects_max_count_even_when_more_is_available(self):
        stream = TelemetryConsumer(capacity=64, drain_batch_capacity=64)
        for i in range(10):
            self.assertTrue(stream.push(i, i, i))
        view = stream.drain(max_count=3)
        self.assertEqual(view.shape[0], 3)
        # the remaining 7 are still there for the next drain()
        view2 = stream.drain(max_count=64)
        self.assertEqual(view2.shape[0], 7)

    def test_drain_max_count_is_clamped_to_drain_batch_capacity(self):
        stream = TelemetryConsumer(capacity=64, drain_batch_capacity=8)
        for i in range(20):
            self.assertTrue(stream.push(i, i, i))
        view = stream.drain(max_count=64)  # requests more than drain_batch_capacity
        self.assertEqual(view.shape[0], 8)

    def test_push_returns_false_when_ring_is_full(self):
        stream = TelemetryConsumer(capacity=4, drain_batch_capacity=4)  # rounds up to 4
        results = [stream.push(i, i, i) for i in range(10)]
        self.assertIn(False, results, "ring must report full rather than silently accepting unbounded pushes")
        self.assertEqual(stream.drain(64).shape[0], results.count(True))

    def test_drain_view_is_invalidated_by_the_next_drain_call(self):
        # The documented lifetime contract: a drain() view aliases a
        # reused scratch buffer, so a *later* drain() call overwrites the
        # memory the earlier view still points at. Verify that's actually
        # true, not just documented.
        stream = TelemetryConsumer(capacity=64, drain_batch_capacity=64)
        stream.push(1, 1, 111)
        first_view = stream.drain()
        first_bytes = bytes(first_view)  # snapshot, since first_view itself is about to change

        stream.push(2, 2, 222)
        stream.drain()  # overwrites the same scratch buffer

        self.assertNotEqual(bytes(first_view), first_bytes,
                             "the underlying memory should have changed after a second drain() call")

    def test_start_synthetic_load_bounded_producer_finishes_and_is_observed_running_false(self):
        # Regression test for a real bug: producer_running() used to check
        # std::thread::joinable(), which stays true long after the thread
        # function actually returns -- a bounded producer that finishes on
        # its own (nobody calls stop_producer()) would report "running"
        # forever, which hung examples/live_stream.py's consumer loop.
        stream = TelemetryConsumer(capacity=1024, drain_batch_capacity=1024)
        stream.start_synthetic_load(event_count=500, target_events_per_sec=0.0)
        deadline = time.time() + 10
        while stream.producer_running and time.time() < deadline:
            pass
        self.assertFalse(stream.producer_running,
                          "producer_running must become False on its own once a bounded run finishes")
        stream.stop_synthetic_load()  # must be a safe no-op here, not raise

    def test_synthetic_load_produces_exact_expected_field_values(self):
        # Cross-checks decode() output against bindings/animus_py.cpp's
        # own synthetic-payload formula (run_producer): event_id = i%256,
        # trace_id = (i*2654435761)%4096, metric_value = 100000+(i%4096).
        stream = TelemetryConsumer(capacity=2048, drain_batch_capacity=2048)
        stream.start_synthetic_load(event_count=1000, target_events_per_sec=0.0)

        received = []
        while True:
            view = stream.drain(2048)
            if view.shape[0] == 0:
                if not stream.producer_running:
                    break
                continue
            received.extend(decode(view))
        stream.stop_synthetic_load()

        self.assertEqual(len(received), 1000)
        self.assertEqual(stream.pushed_count, 1000)
        self.assertEqual(stream.dropped_count, 0)
        for i, rec in enumerate(received):
            self.assertEqual(rec.event_id, i % 256)
            self.assertEqual(rec.trace_id, (i * 2654435761) % 4096)
            self.assertEqual(rec.metric_value, 100000 + (i % 4096))

    def test_unthrottled_flood_accounts_every_event_as_pushed_or_dropped(self):
        # A small ring with a flood producer will genuinely drop events --
        # that's expected, not a bug -- but pushed_count + dropped_count
        # must always equal the requested event_count, and every received
        # record must be exactly one that was actually pushed (no torn
        # reads, no duplicates).
        stream = TelemetryConsumer(capacity=64, drain_batch_capacity=64)
        stream.start_synthetic_load(event_count=5000, target_events_per_sec=0.0)

        received = []
        while True:
            view = stream.drain(64)
            if view.shape[0] == 0:
                if not stream.producer_running:
                    break
                continue
            received.extend(decode(view))
        stream.stop_synthetic_load()

        self.assertEqual(stream.pushed_count + stream.dropped_count, 5000)
        self.assertEqual(len(received), stream.pushed_count)
        seen_sequences = {rec.timestamp_cycles for rec in received}
        self.assertEqual(len(seen_sequences), len(received), "no duplicate events should be received")

    def test_start_producer_twice_without_stopping_raises(self):
        stream = TelemetryConsumer(capacity=256, drain_batch_capacity=256)
        stream.start_synthetic_load(event_count=0, target_events_per_sec=0.0)
        try:
            with self.assertRaises(RuntimeError):
                stream.start_synthetic_load(event_count=0, target_events_per_sec=0.0)
        finally:
            stream.stop_synthetic_load()

    def test_stop_synthetic_load_is_a_safe_noop_when_nothing_is_running(self):
        stream = TelemetryConsumer(capacity=64, drain_batch_capacity=64)
        stream.stop_synthetic_load()  # must not raise
        stream.stop_synthetic_load()  # idempotent

    def test_context_manager_stops_the_producer_on_exit(self):
        with TelemetryConsumer(capacity=1024, drain_batch_capacity=1024) as stream:
            stream.start_synthetic_load(event_count=0, target_events_per_sec=0.0)
            self.assertTrue(stream.producer_running)
        self.assertFalse(stream.producer_running)

    def test_poll_returns_decoded_records_directly(self):
        stream = TelemetryConsumer(capacity=64, drain_batch_capacity=64)
        stream.push(9, 8, 7)
        records = stream.poll()
        self.assertEqual(records, [TelemetryRecord(records[0].timestamp_cycles, 9, 8, 7)])


if __name__ == "__main__":
    unittest.main()
