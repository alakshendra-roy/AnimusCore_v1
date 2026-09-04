"""Python smoke test for Milestone 2 (Single-Producer Multi-Consumer
Broadcast Ring) -- bindings/animus_shm_py.cpp's SpmcConsumerChannel.

Same skip-if-not-built convention as tests/test_dynamic_schema.py: these
drive a real _animus_shm_native.SpmcConsumerChannel end to end (create/open,
broadcast/poll/drain round trips, independent consumer cursors, overrun
detection) all in-process -- two open() calls from the same Python process
still get their own independent read cursors, exercising the identical
code path a second OS process attaching would (SpmcRing<T>::open() has no
in-process-only fast path), which is exactly what
tests/test_spmc.cpp verifies again across real process boundaries.

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
class SpmcConsumerChannelIntegrationTests(unittest.TestCase):
    """Small capacities/counts throughout -- correctness, not a benchmark
    (see tests/test_spmc.cpp for the real cross-process, at-scale version)."""

    def setUp(self):
        from animus._animus_shm_native import SpmcConsumerChannel
        self.SpmcConsumerChannel = SpmcConsumerChannel
        self._counter = getattr(SpmcConsumerChannelIntegrationTests, "_counter", 0) + 1
        SpmcConsumerChannelIntegrationTests._counter = self._counter
        self.name = f"animus_test_spmc_{os.getpid()}_{self._counter}"

    def _create(self, capacity=256, drain_batch_capacity=256):
        prod = self.SpmcConsumerChannel.create(self.name, capacity=capacity, drain_batch_capacity=drain_batch_capacity)
        prod.mark_producer_attached()
        self.addCleanup(prod.unlink)
        return prod

    def test_capacity_rounds_up_to_a_power_of_two(self):
        prod = self._create(capacity=100)
        self.assertEqual(prod.capacity, 128)

    def test_open_raises_a_clear_error_for_a_nonexistent_segment(self):
        with self.assertRaises(RuntimeError):
            self.SpmcConsumerChannel.open(f"{self.name}_does_not_exist")

    def test_create_twice_with_the_same_name_raises(self):
        self._create()
        with self.assertRaises(RuntimeError):
            self.SpmcConsumerChannel.create(self.name, capacity=256, drain_batch_capacity=256)

    def test_single_consumer_receives_everything_broadcast_before_it_polls(self):
        # A consumer's cursor starts at 0 (self-correcting via the overrun
        # path if that's already stale) -- attaching AFTER a small amount
        # of history still exists in the ring sees that history.
        prod = self._create()
        for i in range(10):
            prod.broadcast(sequence=i, price_ticks=100 + i, quantity=1, instrument_id=1)
        cons = self.SpmcConsumerChannel.open(self.name, drain_batch_capacity=256)
        view = cons.drain(100)
        self.assertEqual(view.shape[0], 10)
        self.assertEqual(cons.overrun_count, 0)

    def test_two_independent_consumers_each_get_their_own_cursor(self):
        prod = self._create()
        cons1 = self.SpmcConsumerChannel.open(self.name, drain_batch_capacity=256)
        cons2 = self.SpmcConsumerChannel.open(self.name, drain_batch_capacity=256)

        for i in range(5):
            prod.broadcast(sequence=i, price_ticks=i, quantity=1, instrument_id=1)
        view1 = cons1.drain(256)
        self.assertEqual(view1.shape[0], 5)
        self.assertEqual(cons1.local_tail, 5)
        self.assertEqual(cons2.local_tail, 0, "cons2 hasn't polled yet -- its cursor must be untouched by cons1's poll")

        for i in range(5, 8):
            prod.broadcast(sequence=i, price_ticks=i, quantity=1, instrument_id=1)
        # cons1 only sees the NEW records (it already drained 0-4); cons2
        # sees everything from its own still-untouched cursor at 0.
        view1b = cons1.drain(256)
        view2 = cons2.drain(256)
        self.assertEqual(view1b.shape[0], 3)
        self.assertEqual(view2.shape[0], 8)
        self.assertEqual(cons1.overrun_count, 0)
        self.assertEqual(cons2.overrun_count, 0)

    def test_slow_consumer_detects_and_accounts_an_overrun(self):
        # Tiny capacity, no draining until well past it -- guarantees an
        # overrun deterministically, in-process, with no timing dependency
        # (unlike tests/test_spmc.cpp's real cross-process throttle).
        prod = self._create(capacity=64, drain_batch_capacity=256)
        cons = self.SpmcConsumerChannel.open(self.name, drain_batch_capacity=256)
        total = 500
        for i in range(total):
            prod.broadcast(sequence=i, price_ticks=i, quantity=1, instrument_id=1)

        view = cons.drain(256)
        received = view.shape[0]
        while view.shape[0] > 0:
            view = cons.drain(256)
            received += view.shape[0]

        self.assertGreater(cons.overrun_count, 0)
        self.assertEqual(received + cons.overrun_count, total)

    def test_last_poll_overran_flag_tracks_only_the_most_recent_call(self):
        prod = self._create(capacity=64, drain_batch_capacity=256)
        cons = self.SpmcConsumerChannel.open(self.name, drain_batch_capacity=256)
        for i in range(500):
            prod.broadcast(sequence=i, price_ticks=i, quantity=1, instrument_id=1)

        cons.drain(256)  # first drain after falling behind -- must overrun
        self.assertTrue(cons.last_poll_overran)
        self.assertGreater(cons.overrun_count, 0)

        overrun_before = cons.overrun_count
        cons.drain(256)  # already caught up to the (now-idle) producer -- no new overrun
        self.assertFalse(cons.last_poll_overran)
        self.assertEqual(cons.overrun_count, overrun_before)

    def test_unlink_from_a_non_owner_raises(self):
        self._create()
        cons = self.SpmcConsumerChannel.open(self.name, drain_batch_capacity=256)
        with self.assertRaises(RuntimeError):
            cons.unlink()

    def test_metadata_matches_execution_event(self):
        from animus._animus_shm_native import WIRE_FORMAT, WIRE_RECORD_SIZE
        prod = self._create()
        self.assertEqual(prod.payload_size, WIRE_RECORD_SIZE)
        self.assertEqual(prod.stride, WIRE_RECORD_SIZE)
        self.assertEqual(prod.wire_format, WIRE_FORMAT)
        self.assertTrue(prod.is_owner)
        self.assertEqual(prod.name, self.name)

    def test_is_producer_alive_true_while_producer_handle_is_held(self):
        prod = self._create()
        cons = self.SpmcConsumerChannel.open(self.name, drain_batch_capacity=256)
        self.assertTrue(cons.is_producer_alive())


if __name__ == "__main__":
    unittest.main()
