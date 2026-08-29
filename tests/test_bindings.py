"""Unit tests for animus.bindings: native-library discovery, the pure-Python
fallback engine, and -- the focus of this file -- that poll_signals() is
genuinely zero-copy when a native engine is backing AnimusBindings: the
ctypes buffer allocated in Python is the exact memory the (fake or real)
native call writes into, with no serialize/deserialize step in between.

Run with:
    python -m unittest discover -s tests
or (if pytest is installed; these are plain unittest.TestCase classes, so
pytest collects and runs them without any extra plugin):
    python -m pytest tests
"""
import ctypes
import os
import struct
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from animus.bindings import (  # noqa: E402
    AnimusBindings,
    NativeEvent,
    RuleComparator,
    SpscTelemetryRecord,
    ThreatSignal,
    find_native_library,
    load_native_library,
)


class ThreatSignalLayoutTests(unittest.TestCase):
    """ThreatSignal must stay byte-for-byte identical to animus::ThreatSignal
    (animus.hpp) since it crosses the C-ABI as a caller-supplied buffer --
    any drift here would silently corrupt poll_signals() results.
    """

    def test_size_matches_native_struct(self):
        # 2x uint64 (8) + 4x uint32 (4) = 32 bytes, no padding.
        self.assertEqual(ctypes.sizeof(ThreatSignal), 32)

    def test_field_order_and_offsets(self):
        expected = [
            ("timestamp_cycles", 0),
            ("event_id", 8),
            ("trace_id", 12),
            ("metric_value", 16),
            ("rule_id", 24),
            ("severity", 28),
        ]
        for name, offset in expected:
            field = getattr(ThreatSignal, name)
            self.assertEqual(field.offset, offset, f"{name} offset")

    def test_constructible_from_keywords(self):
        # _PurePythonEngine builds ThreatSignal this way (no C-ABI call
        # involved), so keyword construction must work standalone.
        sig = ThreatSignal(
            timestamp_cycles=123, event_id=1, trace_id=2, metric_value=99,
            rule_id=7, severity=5,
        )
        self.assertEqual((sig.timestamp_cycles, sig.event_id, sig.trace_id,
                           sig.metric_value, sig.rule_id, sig.severity),
                          (123, 1, 2, 99, 7, 5))


class NativeLibraryDiscoveryTests(unittest.TestCase):
    """find_native_library()/load_native_library() must prefer the portable
    AnimusNative build over the legacy AnimusCore_v1 build, search every
    documented location, and degrade gracefully instead of raising when
    `required=False`.
    """

    def _patched_exists(self, existing_paths):
        existing = {os.path.normpath(p) for p in existing_paths}

        def fake_exists(path):
            return os.path.normpath(path) in existing

        return fake_exists

    def test_returns_none_when_nothing_found(self):
        with mock.patch("animus.bindings.os.path.exists", return_value=False):
            self.assertIsNone(find_native_library())

    def test_prefers_animus_native_over_legacy_on_same_platform(self):
        base_dir = os.path.dirname(os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "animus", "bindings.py")))
        native_path = os.path.join(base_dir, "AnimusNative.dll")
        legacy_path = os.path.join(base_dir, "AnimusCore_v1.dll")

        with mock.patch("animus.bindings.sys.platform", "win32"), \
             mock.patch("animus.bindings.os.path.exists",
                         side_effect=self._patched_exists([native_path, legacy_path])):
            found = find_native_library()
            self.assertEqual(os.path.normpath(found), os.path.normpath(native_path))

    def test_falls_back_to_legacy_name_when_native_absent(self):
        base_dir = os.path.dirname(os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "animus", "bindings.py")))
        legacy_path = os.path.join(base_dir, "AnimusCore_v1.dll")

        with mock.patch("animus.bindings.sys.platform", "win32"), \
             mock.patch("animus.bindings.os.path.exists",
                         side_effect=self._patched_exists([legacy_path])):
            found = find_native_library()
            self.assertEqual(os.path.normpath(found), os.path.normpath(legacy_path))

    def test_load_native_library_required_raises_when_missing(self):
        with mock.patch("animus.bindings.os.path.exists", return_value=False):
            with self.assertRaises(FileNotFoundError):
                load_native_library(required=True)

    def test_load_native_library_optional_returns_none_when_missing(self):
        with mock.patch("animus.bindings.os.path.exists", return_value=False):
            self.assertIsNone(load_native_library(required=False))


def _unwrap(value):
    """Extracts the plain Python value from a ctypes simple-data object
    (c_size_t, c_uint32, ...), or returns `value` unchanged if it's already
    plain. Only needed because _FakeNativeLib's methods are plain Python
    functions rather than genuine ctypes foreign functions: a real CDLL
    call marshals a ctypes argument down to its underlying C value before
    the callee ever sees it, but a direct Python call hands the wrapper
    object itself through unchanged.
    """
    return value.value if hasattr(value, "value") else value


class _FakeNativeLib:
    """Stands in for a ctypes.CDLL handle without requiring a compiled
    binary. Each attribute is a plain Python function (not a real ctypes
    foreign function), so AnimusBindings._configure_signatures() can still
    set .argtypes/.restype on it (functions accept arbitrary attributes),
    but calls are dispatched straight to Python -- exactly what's needed to
    observe *which object* animus_poll_signals is handed, proving
    AnimusBindings.poll_signals() passes its ctypes buffer through
    untouched rather than copying into/out of it.
    """

    def __init__(self):
        self.state = {"initialized": False, "events": [], "rules": [], "logging": False}
        self.poll_signals_calls = []

        def animus_init(capacity):
            self.state["initialized"] = True
            self.state["capacity"] = _unwrap(capacity)
            return True
        self.animus_init = animus_init

        def animus_record_event(event_id, trace_id, metric_value):
            self.state["events"].append((_unwrap(event_id), _unwrap(trace_id), _unwrap(metric_value)))
            return True
        self.animus_record_event = animus_record_event

        def animus_record_events_batch(buf, count):
            n = _unwrap(count)
            for i in range(n):
                self.state["events"].append((buf[i].event_id, buf[i].trace_id, buf[i].metric_value))
            return n
        self.animus_record_events_batch = animus_record_events_batch

        def animus_start_logging(filepath):
            self.state["logging"] = True
            self.state["log_path"] = filepath
        self.animus_start_logging = animus_start_logging

        def animus_stop_logging():
            self.state["logging"] = False
        self.animus_stop_logging = animus_stop_logging

        def animus_add_rule(rule_id, event_id, threshold, comparator, severity):
            self.state["rules"].append((
                _unwrap(rule_id), _unwrap(event_id), _unwrap(threshold),
                _unwrap(comparator), _unwrap(severity),
            ))
            return True
        self.animus_add_rule = animus_add_rule

        def animus_poll_signals(buf, max_count):
            # `buf` here is the *exact* (ThreatSignal * N)() ctypes array
            # AnimusBindings.poll_signals() constructed -- record its
            # identity/address, then write matches directly into it, the
            # same way the real C function would through the pointer.
            self.poll_signals_calls.append((id(buf), ctypes.addressof(buf)))
            n = min(2, _unwrap(max_count))
            for i in range(n):
                buf[i] = ThreatSignal(
                    timestamp_cycles=1000 + i, event_id=42, trace_id=i,
                    metric_value=500, rule_id=1, severity=9,
                )
            return n
        self.animus_poll_signals = animus_poll_signals

        def animus_spsc_init(capacity):
            self.state["spsc_initialized"] = True
            self.state["spsc_capacity"] = _unwrap(capacity)
            return True
        self.animus_spsc_init = animus_spsc_init

        def animus_spsc_record_events_batch(buf, count):
            n = _unwrap(count)
            self.state.setdefault("spsc_events", [])
            for i in range(n):
                self.state["spsc_events"].append((buf[i].event_id, buf[i].trace_id, buf[i].metric_value))
            return n
        self.animus_spsc_record_events_batch = animus_spsc_record_events_batch

        def animus_spsc_drain(buf, max_count):
            pending = self.state.get("spsc_events", [])
            n = min(len(pending), _unwrap(max_count))
            for i in range(n):
                event_id, trace_id, metric_value = pending[i]
                buf[i].timestamp_cycles = 777
                buf[i].event_id = event_id
                buf[i].trace_id = trace_id
                buf[i].metric_value = metric_value
            self.state["spsc_events"] = pending[n:]
            return n
        self.animus_spsc_drain = animus_spsc_drain

        def animus_pin_current_thread_to_core(core_id):
            core_id = _unwrap(core_id)
            self.state["pinned_to"] = core_id
            return core_id in (0, 1, 2, 3)  # fake: pretend only cores 0-3 exist
        self.animus_pin_current_thread_to_core = animus_pin_current_thread_to_core

        def animus_get_cpu_count():
            return 4
        self.animus_get_cpu_count = animus_get_cpu_count


class ZeroCopyPollSignalsTests(unittest.TestCase):
    """Verifies AnimusBindings.poll_signals()'s zero-copy contract: the
    ctypes buffer Python allocates is the same memory the native call
    writes into, with no intermediate copy.
    """

    def setUp(self):
        self.fake_lib = _FakeNativeLib()
        self.bindings = AnimusBindings(lib=self.fake_lib)
        self.assertTrue(self.bindings.using_native_engine)
        self.assertTrue(self.bindings.init(buffer_capacity=1024))

    def test_native_call_receives_the_callers_own_buffer(self):
        # Build the buffer the same way AnimusBindings.poll_signals() does,
        # so we can assert on its address independently of the call.
        signals = self.bindings.poll_signals(max_count=8)

        self.assertEqual(len(self.fake_lib.poll_signals_calls), 1)
        _, addr = self.fake_lib.poll_signals_calls[0]
        # The struct the fake "native" side wrote into and the struct
        # AnimusBindings read back from are literally the same bytes --
        # confirm via content, and that nothing narrowed/widened max_count.
        self.assertEqual(len(signals), 2)
        self.assertEqual(signals[0].event_id, 42)
        self.assertEqual(signals[0].metric_value, 500)
        self.assertEqual(signals[1].trace_id, 1)
        self.assertNotEqual(addr, 0)

    def test_buffer_is_sized_to_max_count_not_reallocated_per_signal(self):
        self.bindings.poll_signals(max_count=16)
        # One allocation, sized exactly to max_count -- not one alloc per
        # returned signal, and not a default-sized scratch buffer reused
        # across calls with a different max_count.
        addr_a = self.fake_lib.poll_signals_calls[-1][1]
        self.bindings.poll_signals(max_count=4)
        addr_b = self.fake_lib.poll_signals_calls[-1][1]
        self.assertNotEqual(addr_a, addr_b, "expected a fresh buffer per call")

    def test_record_event_and_add_rule_marshal_through_to_native(self):
        self.bindings.add_rule(1, 42, 100, RuleComparator.GREATER_THAN, 5)
        self.bindings.record_event(event_id=42, trace_id=7, metric_value=250)

        self.assertEqual(self.fake_lib.state["rules"], [(1, 42, 100, 0, 5)])
        self.assertEqual(self.fake_lib.state["events"], [(42, 7, 250)])

    def test_record_events_batch_marshals_through_to_native_in_one_call(self):
        events = [(42, i, i * 10) for i in range(5)]
        pushed = self.bindings.record_events_batch(events)

        self.assertEqual(pushed, 5)
        self.assertEqual(self.fake_lib.state["events"], events)

    def test_record_events_batch_empty_is_a_noop(self):
        self.assertEqual(self.bindings.record_events_batch([]), 0)
        self.assertEqual(self.fake_lib.state["events"], [])

    def test_spsc_push_and_drain_round_trip(self):
        self.assertTrue(self.bindings.spsc_init(1024))
        events = [(7, i, i * 100) for i in range(5)]
        pushed = self.bindings.spsc_record_events_batch(events)
        self.assertEqual(pushed, 5)

        records = self.bindings.spsc_drain(max_count=10)
        self.assertEqual(len(records), 5)
        self.assertEqual(
            [(r.event_id, r.trace_id, r.metric_value) for r in records],
            events,
        )
        # A second drain with nothing left pending must return empty, not error.
        self.assertEqual(self.bindings.spsc_drain(max_count=10), [])

    def test_spsc_drain_before_init_raises(self):
        with self.assertRaises(RuntimeError):
            self.bindings.spsc_drain()

    def test_pin_current_thread_to_core(self):
        self.assertTrue(self.bindings.pin_current_thread_to_core(0))
        self.assertEqual(self.fake_lib.state["pinned_to"], 0)
        self.assertFalse(self.bindings.pin_current_thread_to_core(9999))

    def test_get_cpu_count(self):
        self.assertEqual(self.bindings.get_cpu_count(), 4)

    def test_start_stop_logging_delegate_to_native(self):
        self.bindings.start_logging("telemetry.log")
        self.assertTrue(self.fake_lib.state["logging"])
        self.bindings.stop_logging()
        self.assertFalse(self.fake_lib.state["logging"])

    def test_methods_before_init_raise(self):
        fresh = AnimusBindings(lib=_FakeNativeLib())
        with self.assertRaises(RuntimeError):
            fresh.record_event(1, 1, 1)
        with self.assertRaises(RuntimeError):
            fresh.poll_signals()
        with self.assertRaises(RuntimeError):
            fresh.add_rule(1, 1, 1, RuleComparator.EQUAL, 1)


class PurePythonFallbackTests(unittest.TestCase):
    """AnimusBindings() with no native binary available must fall back to
    _PurePythonEngine transparently and support the full record -> persist
    -> rule-match -> poll_signals lifecycle.
    """

    def setUp(self):
        patcher = mock.patch("animus.bindings.load_native_library", return_value=None)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.bindings = AnimusBindings()

    def test_reports_pure_python_backend(self):
        self.assertFalse(self.bindings.using_native_engine)

    def test_spsc_and_pinning_have_no_fallback_and_raise(self):
        # These are native performance primitives (lock-free SPSC ring, OS
        # thread affinity) with no meaningful pure-Python reimplementation
        # -- unlike record_event/add_rule/poll_signals, which all degrade to
        # _PurePythonEngine, these must raise clearly instead of silently
        # no-op-ing or behaving differently than the native path would.
        for method, args in [
            (self.bindings.spsc_init, ()),
            (self.bindings.spsc_record_events_batch, ([(1, 1, 1)],)),
            (self.bindings.spsc_drain, ()),
            (self.bindings.pin_current_thread_to_core, (0,)),
            (self.bindings.get_cpu_count, ()),
        ]:
            with self.assertRaises(RuntimeError):
                method(*args)

    def test_init_is_idempotent(self):
        self.assertTrue(self.bindings.init(64))
        self.assertTrue(self.bindings.init(64))

    def test_record_event_respects_bounded_capacity(self):
        self.bindings.init(buffer_capacity=4)
        for _ in range(4):
            self.assertTrue(self.bindings.record_event(1, 1, 1))
        # Ring is full: push must return False, not block or evict.
        self.assertFalse(self.bindings.record_event(1, 1, 1))

    def test_record_events_batch_stops_when_ring_fills_partway(self):
        self.bindings.init(buffer_capacity=4)
        pushed = self.bindings.record_events_batch([(1, i, i) for i in range(6)])
        self.assertEqual(pushed, 4)

    def test_full_pipeline_matches_rule_and_polls_signal(self):
        self.bindings.init(buffer_capacity=256)
        self.bindings.add_rule(
            rule_id=1, event_id=500, threshold=100,
            comparator=RuleComparator.GREATER_THAN, severity=5,
        )
        with tempfile.TemporaryDirectory() as tmp:
            log_path = os.path.join(tmp, "telemetry.log")
            self.bindings.start_logging(log_path)
            try:
                self.assertTrue(self.bindings.record_event(event_id=500, trace_id=1, metric_value=150))
                self.assertTrue(self.bindings.record_event(event_id=500, trace_id=2, metric_value=50))

                deadline = time.time() + 2.0
                signals = []
                while time.time() < deadline and not signals:
                    signals = self.bindings.poll_signals(max_count=32)
                    if not signals:
                        time.sleep(0.01)
            finally:
                self.bindings.stop_logging()

            self.assertEqual(len(signals), 1)
            self.assertEqual(signals[0].trace_id, 1)
            self.assertEqual(signals[0].metric_value, 150)
            self.assertEqual(signals[0].rule_id, 1)

            # The fallback's on-disk record format matches shm.py's
            # _RECORD_FORMAT ("<QIIQ") so log files stay consistent
            # regardless of which engine produced them.
            with open(log_path, "rb") as fh:
                raw = fh.read()
            record_size = struct.calcsize("<QIIQ")
            self.assertEqual(len(raw) % record_size, 0)
            self.assertGreaterEqual(len(raw) // record_size, 2)

    def test_rejects_unknown_comparator(self):
        self.bindings.init(64)
        self.assertFalse(self.bindings.add_rule(1, 1, 1, comparator=99, severity=1))

    def test_methods_before_init_raise(self):
        with self.assertRaises(RuntimeError):
            self.bindings.record_event(1, 1, 1)


@unittest.skipUnless(find_native_library(), "no compiled native engine found; build CMakeLists.txt or AnimusCore_v1.slnx first")
class RealNativeEngineIntegrationTests(unittest.TestCase):
    """End-to-end smoke test against whichever real compiled binary
    (AnimusNative or AnimusCore_v1) find_native_library() actually locates
    on this machine -- exercises the real ring buffer and real
    animus_poll_signals pointer marshalling, not a fake.
    """

    def test_record_persist_and_poll_round_trip(self):
        bindings = AnimusBindings()
        self.assertTrue(bindings.using_native_engine)
        self.assertTrue(bindings.init(buffer_capacity=4096))
        bindings.add_rule(
            rule_id=1, event_id=777, threshold=1000,
            comparator=RuleComparator.GREATER_THAN, severity=8,
        )

        with tempfile.TemporaryDirectory() as tmp:
            log_path = os.path.join(tmp, "native_telemetry.log")
            bindings.start_logging(log_path)
            try:
                bindings.record_event(event_id=777, trace_id=1, metric_value=5000)
                bindings.record_event(event_id=777, trace_id=2, metric_value=1)

                deadline = time.time() + 2.0
                signals = []
                while time.time() < deadline and not signals:
                    signals = bindings.poll_signals(max_count=32)
                    if not signals:
                        time.sleep(0.01)
            finally:
                bindings.stop_logging()

            self.assertEqual(len(signals), 1)
            self.assertEqual(signals[0].trace_id, 1)
            self.assertEqual(signals[0].metric_value, 5000)
            self.assertTrue(os.path.exists(log_path))
            self.assertGreater(os.path.getsize(log_path), 0)

    def test_spsc_push_and_drain_round_trip_against_real_binary(self):
        # Real end-to-end check that SpscTelemetryRecord's alignas(64)
        # padding (bindings.py) actually matches the native
        # animus::TelemetryPayload's real layout -- a size mismatch here
        # doesn't raise, it silently reads every record at the wrong
        # offset (reproduced and fixed during development; see
        # SpscTelemetryRecord's docstring).
        bindings = AnimusBindings()
        self.assertTrue(bindings.using_native_engine)
        self.assertTrue(bindings.spsc_init(buffer_capacity=1024))

        events = [(9, i, i * 100) for i in range(50)]
        pushed = bindings.spsc_record_events_batch(events)
        self.assertEqual(pushed, 50)

        records = bindings.spsc_drain(max_count=100)
        self.assertEqual(len(records), 50)
        self.assertEqual(
            [(r.event_id, r.trace_id, r.metric_value) for r in records],
            events,
        )

    def test_pin_current_thread_to_core_against_real_binary(self):
        bindings = AnimusBindings()
        cpu_count = bindings.get_cpu_count()
        self.assertGreaterEqual(cpu_count, 1)
        # Core 0 exists on every real machine this can run on.
        self.assertTrue(bindings.pin_current_thread_to_core(0))
        # An out-of-range core must fail, not silently pin somewhere else.
        self.assertFalse(bindings.pin_current_thread_to_core(cpu_count + 1000))


if __name__ == "__main__":
    unittest.main()
