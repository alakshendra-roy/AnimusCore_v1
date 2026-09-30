"""Python smoke test for the lossless 1-producer -> 1-consumer queue --
bindings/animus_shm_py.cpp's SpscQueue (include/animus/spsc_queue.hpp).

The contract under test is strict backpressure: a full queue REFUSES a push
(try_push() -> False) and never overwrites a queued record, and try_pop()
returns queued records in exactly the order they were pushed (None when
empty). Same skip-if-not-built convention as tests/test_spmc.py; everything
runs in-process, with create() and open() handles standing in for the two
sides exactly as two OS processes would use them.

Run with:
    python -m unittest discover -s tests
or:
    python -m pytest tests
"""
import os
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
class SpscQueueIntegrationTests(unittest.TestCase):

    def setUp(self):
        from animus._animus_shm_native import ExecutionEvent, SpscQueue
        self.SpscQueue = SpscQueue
        self.ExecutionEvent = ExecutionEvent
        self._counter = getattr(SpscQueueIntegrationTests, "_counter", 0) + 1
        SpscQueueIntegrationTests._counter = self._counter
        self.name = f"animus_test_spsc_{os.getpid()}_{self._counter}"

    def _create(self, capacity=16):
        queue = self.SpscQueue.create(self.name, capacity=capacity)
        self.addCleanup(queue.unlink)
        return queue

    def _ev(self, sequence):
        return self.ExecutionEvent(sequence=sequence, price_ticks=1000 + sequence, quantity=sequence + 1, instrument_id=7)

    def _drain(self, queue):
        out = []
        while True:
            rec = queue.try_pop()
            if rec is None:
                return out
            out.append(rec.sequence)

    def test_capacity_rounds_up_to_a_power_of_two(self):
        self.assertEqual(self._create(capacity=3).capacity, 4)

    def test_capacity_has_a_floor_of_two(self):
        self.assertEqual(self._create(capacity=1).capacity, 2)

    def test_every_slot_is_usable(self):
        # Unlike the broadcast ring (capacity - 1 readable), a lossless
        # queue hands out all `capacity` slots before it pushes back.
        queue = self._create(capacity=8)
        self.assertEqual(sum(queue.try_push(self._ev(i)) for i in range(8)), 8)

    def test_open_raises_a_clear_error_for_a_nonexistent_segment(self):
        with self.assertRaises(RuntimeError):
            self.SpscQueue.open(f"{self.name}_does_not_exist")

    def test_create_twice_with_the_same_name_raises(self):
        self._create()
        with self.assertRaises(RuntimeError):
            self.SpscQueue.create(self.name, capacity=16)

    def test_open_refuses_a_segment_of_the_wrong_ring_kind(self):
        from animus._animus_shm_native import BroadcastRing
        ring = BroadcastRing.create(self.name, capacity=16)
        self.addCleanup(ring.unlink)
        with self.assertRaises(RuntimeError):
            self.SpscQueue.open(self.name)

    def test_unlink_from_a_non_owner_raises(self):
        self._create()
        consumer = self.SpscQueue.open(self.name)
        with self.assertRaises(RuntimeError):
            consumer.unlink()

    def test_try_push_rejects_anything_but_an_execution_event(self):
        queue = self._create()
        with self.assertRaises(TypeError):
            queue.try_push((1, 2, 3, 4))

    def test_try_pop_on_an_empty_queue_returns_none(self):
        queue = self._create()
        self.assertIsNone(queue.try_pop())

    def test_popped_record_carries_every_pushed_field(self):
        queue = self._create()
        self.assertTrue(queue.try_push(self.ExecutionEvent(
            sequence=7, price_ticks=-5, quantity=3, instrument_id=9, dispatch_ts_raw=123456789, flags=2)))
        rec = queue.try_pop()
        self.assertEqual((rec.sequence, rec.dispatch_ts_raw, rec.price_ticks, rec.quantity, rec.instrument_id, rec.flags),
                         (7, 123456789, -5, 3, 9, 2))

    def test_records_pop_in_fifo_order(self):
        producer = self._create(capacity=16)
        consumer = self.SpscQueue.open(self.name)
        for i in range(10):
            self.assertTrue(producer.try_push(self._ev(i)))
        self.assertEqual(self._drain(consumer), list(range(10)))
        self.assertIsNone(consumer.try_pop())

    def test_full_queue_refuses_the_push_and_keeps_every_queued_record(self):
        capacity = 8
        queue = self._create(capacity=capacity)
        for i in range(capacity):
            self.assertTrue(queue.try_push(self._ev(i)), f"push {i} of {capacity} must be accepted")

        # Saturated: every further push is refused, repeatedly, and none of
        # them may displace a queued record (that would be the broadcast
        # ring's behaviour, not this queue's).
        for i in range(capacity, capacity + 5):
            self.assertFalse(queue.try_push(self._ev(i)))

        self.assertEqual(self._drain(queue), list(range(capacity)), "the refused pushes must leave no trace")

    def test_popping_one_record_frees_exactly_one_slot(self):
        capacity = 4
        producer = self._create(capacity=capacity)
        consumer = self.SpscQueue.open(self.name)
        for i in range(capacity):
            self.assertTrue(producer.try_push(self._ev(i)))
        self.assertFalse(producer.try_push(self._ev(100)))

        self.assertEqual(consumer.try_pop().sequence, 0)
        self.assertTrue(producer.try_push(self._ev(100)))   # the freed slot
        self.assertFalse(producer.try_push(self._ev(101)))  # ...and only that one

        self.assertEqual(self._drain(consumer), [1, 2, 3, 100])

    def test_fifo_order_survives_many_wrap_arounds_of_a_tiny_queue(self):
        # 4 slots, thousands of records: the indices lap the slot array
        # many times over, pushing and popping in interleaved bursts.
        producer = self._create(capacity=4)
        consumer = self.SpscQueue.open(self.name)
        total, next_push, popped = 5000, 0, []
        while len(popped) < total:
            while next_push < total and producer.try_push(self._ev(next_push)):
                next_push += 1
            for _ in range(3):  # deliberately slower than the producer, so the queue keeps hitting full
                rec = consumer.try_pop()
                if rec is None:
                    break
                popped.append(rec.sequence)
        self.assertEqual(popped, list(range(total)))

    def test_creator_and_opener_handles_are_interchangeable_for_either_role(self):
        creator = self._create(capacity=8)
        opener = self.SpscQueue.open(self.name)
        self.assertTrue(opener.try_push(self._ev(1)))
        self.assertTrue(creator.try_push(self._ev(2)))
        self.assertEqual(creator.try_pop().sequence, 1)
        self.assertEqual(opener.try_pop().sequence, 2)

    def test_metadata(self):
        creator = self._create(capacity=64)
        opener = self.SpscQueue.open(self.name)
        self.assertTrue(creator.is_owner)
        self.assertFalse(opener.is_owner)
        self.assertEqual(creator.name, self.name)
        self.assertEqual(opener.name, self.name)
        self.assertEqual(creator.capacity, 64)
        self.assertEqual(opener.capacity, 64)


if __name__ == "__main__":
    unittest.main()
