"""Python smoke test for Milestone 1 (Dynamic & User-Defined Wire Schemas).

Two tiers, same split tests/test_consumer.py already uses for its own
compiled-extension dependency:

  - WireFormatToDtypeTests exercises animus.dynamic_schema.wire_format_to_dtype()
    against hand-built format strings -- no compiled extension needed at
    all, always runs (skipped only if numpy itself isn't installed, same
    as tests/test_consumer.py's own ToNumpyTests).
  - SharedSchemaChannelIntegrationTests drives a real
    _animus_shm_native.SharedExecutionChannel (bindings/animus_shm_py.cpp)
    end to end, then attaches to that same live segment with the new,
    schema-agnostic SharedSchemaChannel and verifies its metadata
    (payload_size/stride/schema_version_hash/wire_format) and raw_view()
    agree with what the typed channel already knows -- proving the
    "inspect any registered schema without a new compiled extension"
    contract this milestone adds. Skipped, not failed, when the extension
    hasn't been built.

Run with:
    python -m unittest discover -s tests
or:
    python -m pytest tests
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.dynamic_schema import to_structured_array, wire_format_to_dtype  # noqa: E402


def _native_shm_extension_available() -> bool:
    try:
        from animus import _animus_shm_native  # noqa: F401
    except ImportError:
        return False
    return True


def _numpy_available() -> bool:
    try:
        import numpy  # noqa: F401
    except ImportError:
        return False
    return True


@unittest.skipUnless(_numpy_available(), "numpy not installed")
class WireFormatToDtypeTests(unittest.TestCase):
    """No compiled extension involved -- just the format-string parser."""

    def test_execution_event_wire_format_decodes_to_the_right_dtype(self):
        # animus::kExecutionEventWireFormat, include/animus/execution_event.hpp
        dtype = wire_format_to_dtype("<QQqqII")
        self.assertEqual(dtype.itemsize, 40)
        self.assertEqual(list(dtype.names), ["f0", "f1", "f2", "f3", "f4", "f5"])
        self.assertEqual(dtype["f0"].str, "<u8")
        self.assertEqual(dtype["f2"].str, "<i8")
        self.assertEqual(dtype["f4"].str, "<u4")

    def test_orderbook_l2_wire_format_decodes_to_64_bytes(self):
        # animus::schema::OrderBookL2's registered format, include/animus/schema.hpp
        dtype = wire_format_to_dtype("<QQIIqqqqQ")
        self.assertEqual(dtype.itemsize, 64)
        self.assertEqual(len(dtype.names), 9)

    def test_empty_wire_format_raises_value_error(self):
        with self.assertRaises(ValueError):
            wire_format_to_dtype("")

    def test_unsupported_format_character_raises_value_error(self):
        with self.assertRaises(ValueError):
            wire_format_to_dtype("<Qs")  # 's' (char array) isn't a fixed-width scalar type

    def test_default_endianness_is_little(self):
        explicit = wire_format_to_dtype("<QI")
        implicit = wire_format_to_dtype("QI")
        self.assertEqual(explicit, implicit)


@unittest.skipUnless(_native_shm_extension_available(),
                      "no compiled _animus_shm_native extension found; build it via "
                      "`pip install ./bindings` or bindings/CMakeLists.txt's direct-CMake steps first")
class SharedSchemaChannelIntegrationTests(unittest.TestCase):
    """Drives a real SharedExecutionChannel + SharedSchemaChannel pair
    attached to the SAME segment, in-process (both are just handles onto
    the same named OS shared-memory mapping -- attaching twice from one
    process exercises the identical open() code path a second process
    would use). Small capacities/counts throughout -- correctness, not a
    benchmark."""

    def setUp(self):
        from animus._animus_shm_native import SharedExecutionChannel, WIRE_FORMAT, WIRE_RECORD_SIZE
        self.SharedExecutionChannel = SharedExecutionChannel
        self.WIRE_FORMAT = WIRE_FORMAT
        self.WIRE_RECORD_SIZE = WIRE_RECORD_SIZE
        self._segment_counter = getattr(SharedSchemaChannelIntegrationTests, "_counter", 0) + 1
        SharedSchemaChannelIntegrationTests._counter = self._segment_counter
        self.name = f"animus_test_dynamic_schema_{os.getpid()}_{self._segment_counter}"
        self.owner = self.SharedExecutionChannel.create(self.name, capacity=256, drain_batch_capacity=256)
        self.addCleanup(self.owner.unlink)

    def _open_schema_channel(self):
        from animus._animus_shm_native import SharedSchemaChannel
        return SharedSchemaChannel.open(self.name)

    def test_metadata_matches_the_typed_channel(self):
        schema_chan = self._open_schema_channel()
        self.assertEqual(schema_chan.payload_size, self.WIRE_RECORD_SIZE)
        self.assertEqual(schema_chan.stride, self.WIRE_RECORD_SIZE)
        self.assertEqual(schema_chan.wire_format, self.WIRE_FORMAT)
        self.assertEqual(schema_chan.capacity, self.owner.capacity)
        self.assertEqual(schema_chan.name, self.name)
        self.assertNotEqual(schema_chan.schema_version_hash, 0)

    def test_cursors_and_dropped_count_track_the_live_segment(self):
        schema_chan = self._open_schema_channel()
        self.assertEqual(schema_chan.head, 0)
        self.assertEqual(schema_chan.tail, 0)
        self.assertEqual(schema_chan.dropped_count, 0)

        self.assertTrue(self.owner.push(sequence=1, price_ticks=100, quantity=5, instrument_id=7))
        self.assertEqual(schema_chan.head, 1)
        self.owner.poll(max_count=1)
        self.assertEqual(schema_chan.tail, 1)

    @unittest.skipUnless(_numpy_available(), "numpy not installed")
    def test_raw_view_decodes_to_the_exact_pushed_values(self):
        for i in range(10):
            self.assertTrue(self.owner.push(sequence=i, price_ticks=1000 + i, quantity=i + 1, instrument_id=i % 4))

        schema_chan = self._open_schema_channel()
        arr = to_structured_array(schema_chan)
        self.assertEqual(arr.shape[0], schema_chan.capacity)  # raw_view covers the whole ring, not just filled slots

        for i in range(10):
            row = arr[i]
            self.assertEqual(int(row["f0"]), i)              # sequence
            self.assertEqual(int(row["f2"]), 1000 + i)        # price_ticks
            self.assertEqual(int(row["f3"]), i + 1)           # quantity
            self.assertEqual(int(row["f4"]), i % 4)           # instrument_id

    def test_raw_view_is_actually_zero_copy(self):
        # A real memoryview over the live segment, constructed BEFORE any
        # data is pushed -- mutate the source afterward and the SAME view
        # object must see the mutation, proving it aliases shared memory
        # rather than having snapshotted it at raw_view() call time (same
        # style as test_consumer.py's own to_numpy zero-copy test). Row 0
        # is where the very first push lands (head starts at 0).
        stride = self.WIRE_RECORD_SIZE
        schema_chan = self._open_schema_channel()
        buf = schema_chan.raw_view()
        row0_before_push = bytes(buf)[0:stride]
        self.assertEqual(row0_before_push, b"\x00" * stride, "a fresh segment's slots should start zeroed")

        self.assertTrue(self.owner.push(sequence=42, price_ticks=1, quantity=1, instrument_id=1))
        row0_after_push = bytes(buf)[0:stride]  # re-read the SAME buf object, not a fresh raw_view() call
        self.assertNotEqual(row0_after_push, row0_before_push,
                             "raw_view() must alias the live segment, not a snapshot taken at call time")

    def test_open_raises_a_clear_error_for_a_nonexistent_segment(self):
        from animus._animus_shm_native import SharedSchemaChannel
        with self.assertRaises(RuntimeError):
            SharedSchemaChannel.open(f"{self.name}_does_not_exist")


if __name__ == "__main__":
    unittest.main()
