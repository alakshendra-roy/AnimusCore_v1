"""Python smoke test for the lossy 1-writer -> N-reader broadcast ring --
bindings/animus_shm_py.cpp's BroadcastRing (include/animus/broadcast_ring.hpp).

Same skip-if-not-built convention as tests/test_dynamic_schema.py: these
drive a real _animus_shm_native.BroadcastRing end to end (create/open,
publish/poll round trips, independent reader cursors, drop accounting) all
in-process -- two open() calls from the same Python process still get their
own independent read cursors, exercising the identical code path a second OS
process attaching would (BroadcastRing::attach() has no in-process-only fast
path), which is exactly what tests/test_spmc.cpp and
tests/stress_broadcast_ring.cpp verify again across real threads/processes.

poll() returns a zero-copy (n, WIRE_RECORD_SIZE) uint8 view over the reader's
scratch buffer, so every test below decodes it with struct.iter_unpack(
WIRE_FORMAT, bytes(view)) rather than holding on to it across polls.

Run with:
    python -m unittest discover -s tests
or:
    python -m pytest tests
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))


def _native_shm_extension_available() -> bool:
    try:
        from animus import _animus_shm_native  # noqa: F401
    except ImportError:
        return False
    return True


@unittest.skipUnless(_native_shm_extension_available(),
                      "no compiled _animus_shm_native extension found; build it via "
                      "`pip install ./bindings` or bindings/CMakeLists.txt's direct-CMake steps first")
class BroadcastRingIntegrationTests(unittest.TestCase):
    """Small capacities/counts throughout -- correctness, not a benchmark
    (see tests/stress_broadcast_ring.cpp for the real multi-core, at-scale
    version)."""

    def setUp(self):
        from animus._animus_shm_native import BroadcastRing, ExecutionEvent, WIRE_FORMAT, WIRE_RECORD_SIZE
        self.BroadcastRing = BroadcastRing
        self.ExecutionEvent = ExecutionEvent
        self.WIRE_FORMAT = WIRE_FORMAT
        self.WIRE_RECORD_SIZE = WIRE_RECORD_SIZE
        self._counter = getattr(BroadcastRingIntegrationTests, "_counter", 0) + 1
        BroadcastRingIntegrationTests._counter = self._counter
        self.name = f"animus_test_broadcast_{os.getpid()}_{self._counter}"

    def _create(self, capacity=256, batch_capacity=256):
        writer = self.BroadcastRing.create(self.name, capacity=capacity, batch_capacity=batch_capacity)
        self.addCleanup(writer.unlink)
        return writer

    def _publish(self, writer, sequences):
        for i in sequences:
            writer.publish(self.ExecutionEvent(sequence=i, price_ticks=100 + i, quantity=1, instrument_id=1))

    def _decode(self, view):
        """One (sequence, dispatch_ts_raw, price_ticks, quantity, instrument_id, flags) tuple per row."""
        self.assertEqual(view.shape[1], self.WIRE_RECORD_SIZE)
        return list(struct.iter_unpack(self.WIRE_FORMAT, bytes(view)))

    def _sequences(self, view):
        return [rec[0] for rec in self._decode(view)]

    def test_capacity_rounds_up_to_a_power_of_two(self):
        writer = self._create(capacity=100)
        self.assertEqual(writer.capacity, 128)

    def test_capacity_has_a_floor_of_two(self):
        writer = self._create(capacity=1)
        self.assertEqual(writer.capacity, 2)

    def test_open_raises_a_clear_error_for_a_nonexistent_segment(self):
        with self.assertRaises(RuntimeError):
            self.BroadcastRing.open(f"{self.name}_does_not_exist")

    def test_create_twice_with_the_same_name_raises(self):
        self._create()
        with self.assertRaises(RuntimeError):
            self.BroadcastRing.create(self.name, capacity=256, batch_capacity=256)

    def test_open_refuses_a_segment_of_the_wrong_ring_kind(self):
        from animus._animus_shm_native import SpscQueue
        queue = SpscQueue.create(self.name, capacity=16)
        self.addCleanup(queue.unlink)
        with self.assertRaises(RuntimeError):
            self.BroadcastRing.open(self.name)

    def test_publish_rejects_anything_but_an_execution_event(self):
        writer = self._create()
        with self.assertRaises(TypeError):
            writer.publish((1, 2, 3, 4))

    def test_poll_on_an_empty_ring_returns_an_empty_view(self):
        writer = self._create()
        view = writer.poll(16)
        self.assertEqual(view.shape, (0, self.WIRE_RECORD_SIZE))
        self.assertEqual(writer.dropped_count, 0)

    def test_published_fields_round_trip_exactly(self):
        writer = self._create()
        reader = self.BroadcastRing.open(self.name, batch_capacity=16)
        writer.publish(self.ExecutionEvent(sequence=7, price_ticks=-5, quantity=3, instrument_id=9,
                                           dispatch_ts_raw=123456789, flags=2))
        self.assertEqual(self._decode(reader.poll(16)), [(7, 123456789, -5, 3, 9, 2)])

    def test_single_reader_receives_everything_published_before_it_attaches(self):
        # A reader's cursor starts at 0 (snapping forward, and counting the
        # loss, if that's already stale) -- attaching AFTER a small amount of
        # history still exists in the ring sees all of it.
        writer = self._create()
        self._publish(writer, range(10))
        reader = self.BroadcastRing.open(self.name, batch_capacity=256)
        view = reader.poll(100)
        self.assertEqual(view.shape, (10, self.WIRE_RECORD_SIZE))
        self.assertEqual(self._sequences(view), list(range(10)))
        self.assertEqual(reader.dropped_count, 0)

    def test_two_independent_readers_each_get_their_own_cursor(self):
        writer = self._create()
        reader1 = self.BroadcastRing.open(self.name, batch_capacity=256)
        reader2 = self.BroadcastRing.open(self.name, batch_capacity=256)

        self._publish(writer, range(5))
        self.assertEqual(self._sequences(reader1.poll(256)), [0, 1, 2, 3, 4])

        self._publish(writer, range(5, 8))
        # reader1 only sees the NEW records (it already consumed 0-4); reader2
        # has not polled at all, so its own still-untouched cursor at 0 sees
        # everything -- reader1's poll must not have moved it.
        self.assertEqual(self._sequences(reader1.poll(256)), [5, 6, 7])
        self.assertEqual(self._sequences(reader2.poll(256)), list(range(8)))
        self.assertEqual(reader1.dropped_count, 0)
        self.assertEqual(reader2.dropped_count, 0)

    def test_the_writer_handle_can_poll_its_own_ring_with_its_own_cursor(self):
        writer = self._create()
        reader = self.BroadcastRing.open(self.name, batch_capacity=256)
        self._publish(writer, range(3))
        self.assertEqual(self._sequences(writer.poll(16)), [0, 1, 2])
        self.assertEqual(self._sequences(reader.poll(16)), [0, 1, 2])

    def test_slow_reader_detects_and_accounts_a_lap(self):
        # Tiny capacity, no polling until well past it -- guarantees a lap
        # deterministically, in-process, with no timing dependency (unlike the
        # real multi-core tests/stress_broadcast_ring.cpp). Only the newest
        # capacity - 1 records are ever readable: write w recycles the slot of
        # record w - capacity while cursor still == w, so that oldest slot
        # cannot be proven intact (see include/animus/broadcast_ring.hpp).
        capacity, total = 64, 500
        writer = self._create(capacity=capacity, batch_capacity=256)
        reader = self.BroadcastRing.open(self.name, batch_capacity=256)
        self._publish(writer, range(total))

        received = []
        view = reader.poll(256)
        while view.shape[0] > 0:
            received.extend(self._sequences(view))
            view = reader.poll(256)

        self.assertEqual(received, list(range(total - (capacity - 1), total)))
        self.assertEqual(reader.dropped_count, total - (capacity - 1))
        self.assertEqual(len(received) + reader.dropped_count, total)

    def test_a_reader_attaching_after_a_lap_counts_the_missed_records_as_dropped(self):
        capacity, total = 64, 500
        writer = self._create(capacity=capacity, batch_capacity=256)
        self._publish(writer, range(total))
        reader = self.BroadcastRing.open(self.name, batch_capacity=256)
        self.assertEqual(reader.dropped_count, 0, "nothing is counted until the reader actually polls")

        received = self._sequences(reader.poll(256))
        self.assertEqual(received, list(range(total - (capacity - 1), total)))
        self.assertEqual(reader.dropped_count + len(received), total)

    def test_dropped_count_only_moves_when_records_are_actually_lost(self):
        writer = self._create(capacity=64, batch_capacity=256)
        reader = self.BroadcastRing.open(self.name, batch_capacity=256)
        self._publish(writer, range(500))

        reader.poll(256)  # first poll after falling behind -- must lap
        lost = reader.dropped_count
        self.assertGreater(lost, 0)

        self.assertEqual(reader.poll(256).shape[0], 0)  # caught up to the (now-idle) writer
        self.assertEqual(reader.dropped_count, lost)

        self._publish(writer, range(500, 510))  # well inside the window -- nothing lost
        self.assertEqual(self._sequences(reader.poll(256)), list(range(500, 510)))
        self.assertEqual(reader.dropped_count, lost)

    def test_dropped_count_is_a_property_not_a_method(self):
        writer = self._create()
        self.assertIsInstance(writer.dropped_count, int)
        self.assertEqual(writer.dropped_count, 0)

    def test_batch_size_is_clamped_to_the_readers_batch_capacity(self):
        writer = self._create(capacity=256)
        reader = self.BroadcastRing.open(self.name, batch_capacity=4)
        self._publish(writer, range(10))

        self.assertEqual(self._sequences(reader.poll(100)), [0, 1, 2, 3])   # 100 requested, 4 max
        self.assertEqual(self._sequences(reader.poll(2)), [4, 5])           # smaller than the cap: honoured
        self.assertEqual(self._sequences(reader.poll(100)), [6, 7, 8, 9])
        self.assertEqual(reader.poll(100).shape[0], 0)

    def test_poll_zero_returns_an_empty_view_and_consumes_nothing(self):
        writer = self._create()
        self._publish(writer, range(3))
        reader = self.BroadcastRing.open(self.name, batch_capacity=16)
        self.assertEqual(reader.poll(0).shape, (0, self.WIRE_RECORD_SIZE))
        self.assertEqual(self._sequences(reader.poll(16)), [0, 1, 2])

    def test_a_zero_batch_capacity_is_raised_to_one(self):
        writer = self._create()
        reader = self.BroadcastRing.open(self.name, batch_capacity=0)
        self.assertEqual(reader.batch_capacity, 1)
        self._publish(writer, range(3))
        self.assertEqual(self._sequences(reader.poll(16)), [0])

    def test_poll_view_aliases_the_readers_scratch_buffer(self):
        # The documented lifetime contract: the view is zero-copy over a
        # per-reader scratch buffer and is only valid until the next poll()
        # on the SAME reader. Proven here by the old view changing under us.
        writer = self._create()
        reader = self.BroadcastRing.open(self.name, batch_capacity=16)
        self._publish(writer, [1])
        first = reader.poll(16)
        self.assertEqual(self._sequences(first), [1])

        self._publish(writer, [2])
        second = reader.poll(16)
        self.assertEqual(self._sequences(second), [2])
        self.assertEqual(self._sequences(first), [2], "the first view must alias the scratch buffer the second poll reused")

    def test_unlink_from_a_non_owner_raises(self):
        self._create()
        reader = self.BroadcastRing.open(self.name, batch_capacity=256)
        with self.assertRaises(RuntimeError):
            reader.unlink()

    def test_metadata_matches_execution_event(self):
        writer = self._create(capacity=256, batch_capacity=32)
        reader = self.BroadcastRing.open(self.name, batch_capacity=8)
        self.assertEqual(self.WIRE_RECORD_SIZE, struct.calcsize(self.WIRE_FORMAT))
        self.assertEqual(self.WIRE_RECORD_SIZE, 40)
        self.assertTrue(writer.is_owner)
        self.assertFalse(reader.is_owner)
        self.assertEqual(writer.name, self.name)
        self.assertEqual(reader.name, self.name)
        self.assertEqual(writer.capacity, 256)
        self.assertEqual(reader.capacity, 256)
        self.assertEqual(writer.batch_capacity, 32)
        self.assertEqual(reader.batch_capacity, 8)


if __name__ == "__main__":
    unittest.main()
