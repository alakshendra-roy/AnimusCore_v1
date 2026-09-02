// Animus Engine -- Python Zero-Copy Interop Layer (nanobind)
//
// A native Python extension exposing animus::SpscRingBuffer<TelemetryPayload>
// -- the same in-process, lock-free, single-producer/single-consumer ring
// `animus.hpp` uses on its own hot path (see BENCHMARKS.md Phase 14) --
// directly to Python, with no ctypes call-marshalling tax and no C-ABI
// boundary at all: this translation unit #includes animus.hpp directly
// (header-only since Phase 7) and drives animus::Engine's own primitives
// in-process, the same way a C++ caller with a hard latency budget already
// can (see animus::ExecutionClient).
//
// Built on nanobind rather than pybind11 specifically to avoid pybind11's
// larger per-call dispatch overhead and binary footprint -- see nanobind's
// own published benchmarks; this project's own benchmark culture (measure,
// don't assume) says pick that up as a build-time evaluation before relying
// on it, not as an unverified claim repeated here.
//
// Zero-copy contract, stated precisely rather than left implicit: drain()
// pops events out of the lock-free ring into a scratch buffer this object
// owns (one memcpy-equivalent per event -- the same cost the ring's own
// pop() always has, unavoidable at a producer/consumer handoff) and then
// returns a Python buffer-protocol view *over that same scratch memory* --
// zero-copy specifically across the Python/C++ boundary: no per-event
// Python object is constructed, no heap allocation happens on this path
// after construction, and no second copy is made to hand the batch to
// Python. The returned view aliases the scratch buffer directly, so it is
// only valid until the next drain() call -- see TelemetryStream::drain's
// docstring below.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include "animus.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

// Wire format for the buffer-protocol view drain() returns: identical
// layout to animus::SharedTelemetryRecord / animus.shm's own <QIIQ> record
// (see animus.hpp's SharedTelemetryRecord for why it's tightly packed, not
// cache-line padded like the internal TelemetryPayload ring storage) --
// deliberately reusing that existing wire format rather than inventing a
// third one, so a caller already familiar with the shared-memory IPC layer
// gets the same 24-byte record shape here.
using WireRecord = animus::SharedTelemetryRecord;
static_assert(sizeof(WireRecord) == 24, "must stay wire-compatible with animus::SharedTelemetryRecord");
constexpr const char* kWireFormat = "<QIIQ"; // struct.calcsize/struct.iter_unpack format, matches animus.shm

// One producer thread, one consumer (the calling Python thread) -- the
// same SPSC contract animus::SpscRingBuffer<T> and animus_spsc_* already
// document and do not enforce at runtime (a debug-only check would cost
// real cycles on the exact path this class exists to make fast). This
// class inherits that contract unchanged: never call push() and the
// background producer concurrently, and never call drain() from more
// than one thread at a time.
class TelemetryStream {
public:
    TelemetryStream(size_t capacity, size_t drain_batch_capacity)
        : ring_(capacity),
          scratch_(drain_batch_capacity == 0 ? size_t{1} : drain_batch_capacity) {
    }

    ~TelemetryStream() { stop_producer(); }

    TelemetryStream(const TelemetryStream&) = delete;
    TelemetryStream& operator=(const TelemetryStream&) = delete;

    // Producer-side single-event push, timestamped the same way the native
    // engine stamps every event (animus::read_cycle_counter(), the same
    // RDTSC-or-monotonic-clock helper TelemetryPayload's timestamp_cycles
    // uses throughout animus.hpp). Never blocks; returns false if the ring
    // is momentarily full -- mirrors SpscRingBuffer::push's own contract,
    // not silently dropped without the caller knowing.
    bool push(uint32_t event_id, uint32_t trace_id, uint64_t metric_value) noexcept {
        const animus::TelemetryPayload ev{
            animus::read_cycle_counter(), event_id, trace_id, metric_value
        };
        return ring_.push(ev);
    }

    // Spawns one background native thread generating synthetic telemetry
    // load -- for exercising Python-side consumption throughput/latency
    // without needing a second real producer process or thread wired up
    // by the caller. event_count == 0 means "run until stop_producer()".
    // target_events_per_sec <= 0 means unthrottled (push as fast as the
    // ring accepts, matching this repo's other flood-throughput benchmarks
    // -- see benchmarks/telemetry_benchmark.cpp).
    void start_producer(uint64_t event_count, double target_events_per_sec) {
        if (producer_thread_.joinable()) {
            throw std::runtime_error(
                "producer already running -- call stop_producer() first "
                "(SPSC ring: exactly one producer at a time)");
        }
        stop_requested_.store(false, std::memory_order_relaxed);
        pushed_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        producer_thread_ = std::thread([this, event_count, target_events_per_sec] {
            run_producer(event_count, target_events_per_sec);
            running_.store(false, std::memory_order_release);
        });
    }

    void stop_producer() {
        stop_requested_.store(true, std::memory_order_relaxed);
        if (producer_thread_.joinable()) {
            producer_thread_.join();
        }
    }

    // NOTE: deliberately NOT producer_thread_.joinable() -- a std::thread
    // stays joinable() from the moment it's started until join()/detach()
    // is called, regardless of whether the underlying thread function has
    // actually returned. A bounded producer (event_count != 0) finishes
    // run_producer() on its own without anyone calling stop_producer(), so
    // joinable() alone would report "running" forever after it's actually
    // done -- running_ is a real flag the thread sets itself, covering
    // every exit path (bounded completion or stop_requested_).
    bool producer_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    uint64_t pushed_count() const noexcept { return pushed_.load(std::memory_order_relaxed); }
    uint64_t dropped_count() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    size_t capacity() const noexcept { return ring_.capacity(); }
    size_t drain_batch_capacity() const noexcept { return scratch_.size(); }

    // Consumer-side (the calling Python thread) only. Pops up to
    // min(max_count, drain_batch_capacity()) events out of the ring into
    // this object's own scratch buffer, then returns a zero-copy
    // buffer-protocol view over exactly the N events actually popped
    // (N may be less than requested if the ring ran dry -- never blocks).
    //
    // THE RETURNED VIEW ALIASES THIS OBJECT'S SCRATCH BUFFER. It stays
    // valid only until the next drain() call on this same object, which
    // overwrites the same memory -- copy out what you need (bytes(view),
    // struct.unpack_from, numpy.frombuffer(...).copy(), ...) before
    // calling drain() again if the data must outlive that call. This is
    // the standard tradeoff a reused-buffer zero-copy design makes; see
    // animus/consumer.py for the idiomatic way to consume it.
    nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> drain(size_t max_count) {
        const size_t limit = max_count < scratch_.size() ? max_count : scratch_.size();
        size_t n = 0;
        animus::TelemetryPayload ev;
        while (n < limit && ring_.pop(ev)) {
            scratch_[n] = WireRecord{ ev.timestamp_cycles, ev.event_id, ev.trace_id, ev.metric_value };
            ++n;
        }
        // shape (n, sizeof(WireRecord)): one row per record, raw bytes --
        // no numpy dependency required to construct or return this; a
        // caller with numpy gets a zero-copy *structured* view via
        // numpy.frombuffer(view, dtype=...) -- see consumer.py's
        // to_numpy() for the exact dtype construction.
        return nb::ndarray<uint8_t, nb::memview, nb::ndim<2>>(
            reinterpret_cast<uint8_t*>(scratch_.data()),
            { n, sizeof(WireRecord) },
            nb::find(*this)
        );
    }

private:
    void run_producer(uint64_t event_count, double target_events_per_sec) {
        const bool bounded = event_count != 0;
        const bool paced = target_events_per_sec > 0.0;
        const auto interval = paced
            ? std::chrono::duration<double>(1.0 / target_events_per_sec)
            : std::chrono::duration<double>(0.0);
        auto next_due = std::chrono::steady_clock::now();

        uint64_t i = 0;
        while (!stop_requested_.load(std::memory_order_relaxed) && (!bounded || i < event_count)) {
            if (paced) {
                next_due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
                std::this_thread::sleep_until(next_due);
            }
            // Deterministic synthetic payload -- same convention as
            // benchmarks/telemetry_benchmark.cpp's make_synthetic_event:
            // no RNG on the hot path, still varies field-to-field so a
            // consumer has something real to unpack and verify.
            const animus::TelemetryPayload ev{
                animus::read_cycle_counter(),
                static_cast<uint32_t>(i % 256),                  // event_id
                static_cast<uint32_t>((i * 2654435761u) % 4096), // trace_id
                static_cast<uint64_t>(100000 + (i % 4096))       // metric_value
            };
            if (ring_.push(ev)) {
                pushed_.fetch_add(1, std::memory_order_relaxed);
            } else {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            ++i;
        }
    }

    animus::SpscRingBuffer<animus::TelemetryPayload> ring_;
    std::vector<WireRecord> scratch_;
    std::thread producer_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> pushed_{0};
    std::atomic<uint64_t> dropped_{0};
};

} // namespace

NB_MODULE(_animus_native, m) {
    m.doc() = "Animus Engine -- nanobind zero-copy interop layer over animus::SpscRingBuffer<TelemetryPayload>";
    m.attr("WIRE_FORMAT") = kWireFormat;
    m.attr("WIRE_RECORD_SIZE") = sizeof(WireRecord);

    nb::class_<TelemetryStream>(m, "TelemetryStream")
        .def(nb::init<size_t, size_t>(), "capacity"_a, "drain_batch_capacity"_a = 8192,
             "capacity: ring size (rounded up to a power of two). "
             "drain_batch_capacity: scratch buffer size in records -- also the "
             "hard upper bound on a single drain() call's batch size.")
        .def("push", &TelemetryStream::push, "event_id"_a, "trace_id"_a, "metric_value"_a,
             "Push one event from Python. Never blocks; returns False if the ring is full.")
        .def("start_producer", &TelemetryStream::start_producer,
             "event_count"_a = 0, "target_events_per_sec"_a = 0.0,
             nb::call_guard<nb::gil_scoped_release>(),
             "Spawn a background native thread generating synthetic load. "
             "event_count=0 runs until stop_producer(); target_events_per_sec<=0 is unthrottled.")
        .def("stop_producer", &TelemetryStream::stop_producer,
             nb::call_guard<nb::gil_scoped_release>(),
             "Stop and join the background producer thread, if running.")
        .def("producer_running", &TelemetryStream::producer_running)
        .def("drain", &TelemetryStream::drain, "max_count"_a,
             // Deliberately NOT nb::call_guard<gil_scoped_release>: drain()'s
             // body ends by calling nb::find(*this) to build the returned
             // ndarray's owner reference, which is a Python-API call and
             // requires the GIL to be held. Releasing the GIL around the
             // whole function (as an earlier version of this binding did)
             // is a real, reproducible segfault, not a theoretical one --
             // caught by actually running examples/live_stream.py, not
             // just compiling this file. drain()'s own work (popping a
             // bounded batch out of the ring) is short, CPU-only, and
             // non-blocking, so there is no real benefit to releasing the
             // GIL here the way there is for stop_producer()'s join() below.
             "Drain up to max_count events into a zero-copy view -- see the C++ "
             "docstring in bindings/animus_py.cpp for the buffer-lifetime contract.")
        .def_prop_ro("capacity", &TelemetryStream::capacity)
        .def_prop_ro("drain_batch_capacity", &TelemetryStream::drain_batch_capacity)
        .def_prop_ro("pushed_count", &TelemetryStream::pushed_count)
        .def_prop_ro("dropped_count", &TelemetryStream::dropped_count);
}
