# SPDX-License-Identifier: Apache-2.0
"""Verifies src/python_bridge.cpp: the nanobind bridge really is zero-copy, safe, and correct.

What is proved, and how (a view that "looks right" can be a silent copy, so nothing here trusts that):

  * No copy       -- the NumPy array's data pointer equals the C++ block's slot-0 address, a record
                     the native side publishes appears in an array obtained BEFORE the push, and
                     tracemalloc sees no allocation proportional to the 4 MiB array. Negative
                     controls: a real .copy() has a different address and IS seen by tracemalloc, so
                     both detectors can tell the difference.
  * Lifetime      -- the array (and NumPy views derived from it) keeps the C++ buffer alive after
                     every Python reference to the TelemetryBuffer is gone, and releases it after.
  * Read-only     -- the view is non-writeable; Python cannot write into the producer's memory.
  * Protocol      -- head/tail/push/consume follow the SPSC contract, including backpressure and
                     wrap-around onto a physical slot.
  * Concurrency   -- with a native thread producing (GIL released) while Python consumes through the
                     zero-copy view, every record read from [tail, head) is intact and in order.
  * Layout        -- SLOT_FIELDS builds a structured dtype that reads the fields at the right offsets.

Build the module first (`make pybridge` does both steps), then, from the package root:
    python python/test_python_bridge.py
The compiled module is found under build/ (a make build puts it in build/bridge, a Visual Studio build in
build/<dir>/Release); set ANIMUS_BRIDGE_DIR to point somewhere else. Needs NumPy at run time. Run as a
script, a missing module or NumPy is an error (exit 2), not a skip.
"""
import gc
import importlib.machinery
import os
import statistics
import sys
import threading
import time
import tracemalloc
import unittest
import weakref

PACKAGE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_bridge_dir():
    """ANIMUS_BRIDGE_DIR if set, else the directory under build/ holding a compiled module this
    interpreter can import (the suffix must match its ABI, so a stale build for another Python is ignored)."""
    explicit = os.environ.get("ANIMUS_BRIDGE_DIR")
    if explicit:
        return explicit
    suffixes = tuple(importlib.machinery.EXTENSION_SUFFIXES)
    for root, _dirs, files in os.walk(os.path.join(PACKAGE_ROOT, "build")):
        if any(name.startswith("animus_bridge") and name.endswith(suffixes) for name in files):
            return root
    return None


_BRIDGE_DIR = find_bridge_dir()
if _BRIDGE_DIR:
    sys.path.insert(0, _BRIDGE_DIR)

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None
try:
    import animus_bridge as ab
except ImportError:  # pragma: no cover
    ab = None

MISSING = ("NumPy" if np is None else "") + (" and " if np is None and ab is None else "") + \
          ("the animus_bridge module (build it with `make pybridge`, or set ANIMUS_BRIDGE_DIR)" if ab is None else "")


def addr(a):
    """Address of the array's first element."""
    return a.__array_interface__["data"][0]


def slot_dtype():
    """The structured dtype SLOT_FIELDS describes: 64 bytes, named fields at the C++ offsets."""
    names = [f[0] for f in ab.SLOT_FIELDS]
    return np.dtype({"names": names,
                     "formats": [f[1] for f in ab.SLOT_FIELDS],
                     "offsets": [f[2] for f in ab.SLOT_FIELDS],
                     "itemsize": ab.SLOT_BYTES})


@unittest.skipIf(np is None or ab is None, "needs " + MISSING)
class BridgeTests(unittest.TestCase):

    def test_layout_constants_match_the_cpp_slot(self):
        self.assertEqual(ab.SLOT_BYTES, 64)
        self.assertEqual(ab.SLOT_WORDS, 8)
        self.assertEqual([(n, f, o) for n, f, o in ab.SLOT_FIELDS],
                         [("timestamp_cycles", "<u8", 0), ("event_id", "<u4", 8),
                          ("trace_id", "<u4", 12), ("metric_value", "<u8", 16)])
        self.assertEqual(slot_dtype().itemsize, 64)

    def test_view_aliases_the_cpp_block_and_is_not_a_copy(self):
        buf = ab.TelemetryBuffer(1 << 10)
        v = buf.slots()
        self.assertIsInstance(v, np.ndarray)
        self.assertEqual(v.shape, (1 << 10, 8))
        self.assertEqual(v.dtype, np.dtype("<u8"))
        self.assertEqual(addr(v), buf.data_ptr, "the array must point at the C++ slot memory")
        self.assertEqual(addr(v) % 64, 0, "slot 0 sits on a cache-line boundary")
        self.assertFalse(v.flags.owndata, "the array must not own its data")
        self.assertEqual(v.nbytes, buf.nbytes)
        again = buf.slots()
        self.assertEqual(addr(again), addr(v))
        self.assertTrue(np.shares_memory(v, again))
        # negative control: the same check does flag a real copy
        self.assertNotEqual(addr(v.copy()), buf.data_ptr)

    def test_view_is_read_only(self):
        buf = ab.TelemetryBuffer(8)
        v = buf.slots()
        self.assertFalse(v.flags.writeable)
        with self.assertRaises(ValueError):
            v[0, 0] = 1

    def test_records_published_natively_appear_in_a_view_taken_earlier(self):
        buf = ab.TelemetryBuffer(8)
        v = buf.slots()                       # taken BEFORE the push: no refresh needed
        self.assertEqual(int(v[0, 0]), 0, "slots start zeroed, never stale heap bytes")
        self.assertTrue(buf.push(111, 7, 9, 555))
        self.assertEqual(int(v[0, 0]), 111)                 # timestamp_cycles
        self.assertEqual(int(v[0, 1]) & 0xFFFFFFFF, 7)      # event_id (low half of word 1)
        self.assertEqual(int(v[0, 1]) >> 32, 9)             # trace_id (high half)
        self.assertEqual(int(v[0, 2]), 555)                 # metric_value

    def test_structured_and_column_views_stay_zero_copy(self):
        buf = ab.TelemetryBuffer(8)
        v = buf.slots()
        buf.push(111, 7, 9, 555)
        rec = v.view(slot_dtype()).reshape(-1)
        self.assertEqual(rec.shape, (8,))
        self.assertEqual((int(rec["timestamp_cycles"][0]), int(rec["event_id"][0]),
                          int(rec["trace_id"][0]), int(rec["metric_value"][0])), (111, 7, 9, 555))
        self.assertEqual(addr(rec), buf.data_ptr)
        self.assertTrue(np.shares_memory(rec, v))
        column = rec["metric_value"]
        self.assertTrue(np.shares_memory(column, v))
        self.assertEqual(column.strides, (64,), "a column is a strided view over the slots, not a gathered copy")

    def test_a_view_keeps_the_buffer_alive_and_releases_it(self):
        buf = ab.TelemetryBuffer(16)
        buf.push(1, 2, 3, 4)
        ref = weakref.ref(buf)
        v = buf.slots()
        rec = v.view(slot_dtype()).reshape(-1)
        del buf
        gc.collect()
        self.assertIsNotNone(ref(), "the array must own the buffer it points into")
        self.assertEqual(int(rec["metric_value"][0]), 4, "and the memory must still be valid")
        del v
        gc.collect()
        self.assertIsNotNone(ref(), "a NumPy view derived from the array keeps the chain alive")
        del rec
        gc.collect()
        self.assertIsNone(ref(), "with the last view gone, the buffer is released")

    def test_no_allocation_proportional_to_the_array_size(self):
        buf = ab.TelemetryBuffer(1 << 16)     # 4 MiB of slots
        gc.collect()
        tracemalloc.start()
        try:
            before = tracemalloc.get_traced_memory()[0]
            v = buf.slots()
            after = tracemalloc.get_traced_memory()[0]
        finally:
            tracemalloc.stop()
        self.assertEqual(v.nbytes, 4 << 20)
        self.assertLess(after - before, 64 * 1024,
                        "creating the view allocated %d bytes for a %d-byte array: that is a copy" % (after - before, v.nbytes))
        # negative control: the accounting does see a real copy of the same array
        tracemalloc.start()
        try:
            before = tracemalloc.get_traced_memory()[0]
            c = v.copy()
            after = tracemalloc.get_traced_memory()[0]
        finally:
            tracemalloc.stop()
        self.assertGreaterEqual(after - before, c.nbytes)

    def test_spsc_protocol_backpressure_and_wraparound(self):
        cap = 8
        buf = ab.TelemetryBuffer(cap)
        v = buf.slots()
        self.assertEqual((buf.head, buf.tail), (0, 0))
        for i in range(cap):
            self.assertTrue(buf.push(100 + i, i, 0, 1000 + i))
        self.assertFalse(buf.push(999, 9, 9, 9), "a full buffer refuses the push")
        self.assertEqual(buf.head, cap)
        self.assertEqual(int(v[0, 0]), 100, "the refused push must not have overwritten slot 0")
        buf.consume(3)
        self.assertEqual(buf.tail, 3)
        with self.assertRaises(ValueError):
            buf.consume(cap)                  # only 5 are published and unconsumed
        self.assertEqual(buf.tail, 3, "a rejected consume changes nothing")
        self.assertTrue(buf.push(200, 20, 0, 2000))   # record #8 goes to physical slot 8 & 7 == 0
        self.assertEqual((int(v[0, 0]), int(v[0, 2])), (200, 2000))
        buf.consume(6)
        self.assertEqual((buf.head, buf.tail), (9, 9))
        with self.assertRaises(ValueError):
            buf.consume(1)

    def test_concurrent_native_producer_and_python_consumer(self):
        # The contract in python_bridge.cpp, under real concurrency: a native thread publishes records
        # (GIL released) into a SMALL ring, so it laps the consumer many times, while Python reads
        # [tail, head) through the zero-copy view and retires what it has read. A reader that saw a
        # slot mid-write, a slot beyond head, or a slot the producer had already reused would fail the
        # per-record check below.
        cap, total = 4096, 2000000
        mask = cap - 1
        buf = ab.TelemetryBuffer(cap)
        view = buf.slots()
        published = []
        producer = threading.Thread(target=lambda: published.append(buf.produce(total)), daemon=True)
        producer.start()
        consumed = bad = 0
        deadline = time.monotonic() + 120
        mult, shift = np.uint64(0x9E3779B97F4A7C15), np.uint64(29)
        while consumed < total:
            self.assertLess(time.monotonic(), deadline, "consumer stalled at %d of %d records" % (consumed, total))
            head = buf.head                      # acquire: slots [consumed, head) are published and stable
            n = head - consumed
            if n == 0:
                continue
            start = consumed & mask
            first = min(n, cap - start)
            rows = view[start:start + first] if first == n else np.concatenate((view[start:], view[:n - first]))
            seq = np.arange(consumed, head, dtype=np.uint64)
            ok = ((rows[:, 0] == seq)
                  & ((rows[:, 1] & 0xFFFFFFFF) == (seq & 0xFFFFFFFF))
                  & ((rows[:, 1] >> 32) == (seq >> 32))
                  & (rows[:, 2] == ((seq * mult) ^ (seq >> shift))))
            bad += n - int(np.count_nonzero(ok))
            buf.consume(n)
            consumed = head
        producer.join()
        self.assertEqual(published, [total], "produce() reports how many records it published")
        self.assertEqual(bad, 0, "%d of %d records were torn, stale, or out of order" % (bad, total))
        self.assertEqual((buf.head, buf.tail), (total, total))
        # No separate "did they overlap" assertion is needed: the ring holds 4096 slots and the stream is
        # 2,000,000 records, so the producer can only finish by publishing while the consumer retires.

    def test_produce_gives_up_instead_of_spinning_when_nobody_consumes(self):
        # Without this bound, a consumer that dies leaves the native producer spinning forever on a full
        # buffer, and a non-daemon thread then keeps the interpreter from exiting.
        buf = ab.TelemetryBuffer(8)
        t0 = time.monotonic()
        published = buf.produce(100, 50)      # nobody consumes: only the 8 slots can ever be filled
        elapsed = time.monotonic() - t0
        self.assertEqual(published, 8)
        self.assertEqual(buf.head, 8)
        self.assertGreaterEqual(elapsed, 0.045, "it should have waited out the stall timeout")
        self.assertLess(elapsed, 5.0)
        self.assertEqual(buf.produce(100, 10), 0, "a full buffer: gives up at once, publishes nothing")

    def test_bad_arguments_are_rejected_cleanly(self):
        for bad in (0, 1, 3, 12, 1 << 60):
            with self.assertRaises(ValueError, msg="capacity %d" % bad):
                ab.TelemetryBuffer(bad)
        with self.assertRaises(TypeError):
            ab.TelemetryBuffer(-8)
        buf = ab.TelemetryBuffer(8)
        with self.assertRaises(TypeError):
            buf.push(1, 1 << 32, 0, 0)        # event_id is a uint32
        with self.assertRaises(TypeError):
            buf.consume(-1)

    def test_report_view_creation_cost(self):
        # Informational: the cost of one slots() call from Python (a view of the WHOLE buffer, so it does
        # not scale with capacity). It is a per-call figure, not a per-record or per-event one.
        buf = ab.TelemetryBuffer(1 << 12)
        for _ in range(2000):
            buf.slots()
        samples = []
        for _ in range(5):
            t0 = time.perf_counter_ns()
            for _ in range(20000):
                buf.slots()
            samples.append((time.perf_counter_ns() - t0) / 20000)
        median = statistics.median(samples)
        print("\n  slots() call: median %.0f ns per call (5 x 20000 calls, Python %s, NumPy %s)"
              % (median, sys.version.split()[0], np.__version__), file=sys.stderr)
        self.assertLess(median, 1e6)


if __name__ == "__main__":
    if np is None or ab is None:
        print("cannot run: needs " + MISSING, file=sys.stderr)
        sys.exit(2)
    unittest.main(verbosity=2)
