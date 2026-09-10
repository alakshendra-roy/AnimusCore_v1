// Animus Evaluation Kit -- Python Zero-Copy Interop (nanobind)
//
// Binds AnimusEngine (include/animus/engine.hpp) directly -- this
// translation unit includes only the public engine header, the same one
// src/replay_bench.cpp builds against, so the Python surface exercises
// exactly what a C++ client would link against, nothing more.
//
// Zero-copy contract, stated precisely rather than left implicit: poll()
// drains entries out of the engine's ring into a scratch buffer this
// object owns (one plain-assignment copy per entry -- the same cost
// AnimusEngine::poll's callback always pays at a producer/consumer handoff,
// unavoidable there) and then returns a Python buffer-protocol view *over
// that same scratch memory*: zero-copy specifically across the Python/C++
// boundary -- no per-entry Python object is constructed, no allocation
// happens on this path after construction, and no second copy is made to
// hand the batch to Python. The returned view aliases the scratch buffer
// directly, so it is only valid until the next poll() call on this same
// object -- see ReplayStream::poll's docstring below.
//
// Built on nanobind (not pybind11) for its lower per-call dispatch overhead
// and smaller binary footprint on the drain/poll hot path -- consistent
// with this codebase's benchmark-driven build choices elsewhere.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "animus/engine.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

using animus::eval::AnimusEngine;
using animus::eval::MarketDataEntry;
using animus::eval::TelemetryMetrics;
using animus::eval::WireTick;

// struct.calcsize/numpy dtype-compatible wire format for MarketDataEntry --
// keep in exact sync with the field order and sizes in engine.hpp. Little-
// endian ('<'), matching every x86_64 target this kit builds for.
//   Q  send_timestamp_ns   u8   8
//   Q  recv_timestamp_ns   u8   8
//   Q  sequence            u8   8
//   q  price_fixed         i8   8
//   I  symbol_id           u4   4
//   I  quantity            u4   4
//   B  side                u1   1
//   B  msg_type            u1   1
//   22x reserved padding to the 64-byte cache line
constexpr const char* kWireFormat = "<QQQqIIBB22x";

// Consumer callback threaded through AnimusEngine::poll -- appends into the
// ReplayStream's own scratch_ vector via user_data, matching the
// C-function-pointer + opaque-context shape AnimusEngine's public API
// requires (no std::function, no capturing lambda passed across the ABI
// boundary).
void append_to_scratch(const MarketDataEntry& entry, void* user_data) {
    auto* scratch = static_cast<std::vector<MarketDataEntry>*>(user_data);
    scratch->push_back(entry);
}

// Single-producer/single-consumer contract inherited unchanged from
// AnimusEngine: from Python, that means never call ingest() concurrently
// with itself, and never call poll() from more than one thread at a time.
// A typical evaluation script calls ingest() from the main thread in a
// loop (or feeds packets captured from a real feed) and poll() from that
// same thread between batches, which satisfies the contract trivially.
class ReplayStream {
public:
    ReplayStream(uint32_t cpu_core, size_t ring_capacity, size_t drain_batch_capacity) {
        if (!engine_.initialize(cpu_core, ring_capacity)) {
            throw std::runtime_error(
                "AnimusEngine::initialize failed -- check cpu_core is a valid logical "
                "core on this host and ring_capacity is nonzero");
        }
        scratch_.reserve(drain_batch_capacity == 0 ? size_t{1} : drain_batch_capacity);
    }

    // Accepts any object implementing the Python buffer protocol (bytes,
    // bytearray, numpy array of dtype uint8, ...) with at least
    // sizeof(WireTick) bytes; reads it directly off that buffer with no
    // intermediate Python-side copy.
    bool ingest(nb::ndarray<const uint8_t, nb::ndim<1>> packet) {
        return engine_.ingest_packet(packet.data(), packet.shape(0));
    }

    // Pops up to min(max_count, drain_batch_capacity()) entries currently
    // available in the ring into this object's own scratch buffer, then
    // returns a zero-copy buffer-protocol view over exactly the N entries
    // actually popped (N may be less than requested if the ring ran dry --
    // never blocks).
    //
    // THE RETURNED VIEW ALIASES THIS OBJECT'S SCRATCH BUFFER. It stays
    // valid only until the next poll() call on this same object, which
    // overwrites the same memory -- copy out what you need
    // (numpy.frombuffer(view, dtype=...).copy()) before calling poll()
    // again if the data must outlive that call.
    nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> poll(size_t max_count) {
        const size_t limit = std::min(max_count, scratch_.capacity());
        scratch_.clear();
        engine_.poll(&append_to_scratch, &scratch_, limit);
        return nb::ndarray<uint8_t, nb::memview, nb::ndim<2>>(
            reinterpret_cast<uint8_t*>(scratch_.data()),
            {scratch_.size(), sizeof(MarketDataEntry)},
            nb::find(*this));
    }

    nb::dict telemetry() const {
        const TelemetryMetrics m = engine_.get_telemetry_metrics();
        nb::dict d;
        d["packets_ingested"] = m.packets_ingested;
        d["packets_dropped"] = m.packets_dropped;
        d["packets_malformed"] = m.packets_malformed;
        d["ring_capacity"] = m.ring_capacity;
        d["ring_occupancy_approx"] = m.ring_occupancy_approx;
        return d;
    }

    size_t drain_batch_capacity() const noexcept { return scratch_.capacity(); }

private:
    AnimusEngine engine_;
    std::vector<MarketDataEntry> scratch_;
};

} // namespace

NB_MODULE(animus_eval_native, m) {
    m.doc() = "Animus Evaluation Kit -- nanobind zero-copy interop layer over AnimusEngine";
    m.attr("WIRE_FORMAT") = kWireFormat;
    m.attr("WIRE_RECORD_SIZE") = sizeof(MarketDataEntry);
    m.attr("WIRE_TICK_SIZE") = sizeof(WireTick);

    nb::class_<ReplayStream>(m, "ReplayStream")
        .def(nb::init<uint32_t, size_t, size_t>(),
             "cpu_core"_a, "ring_capacity"_a, "drain_batch_capacity"_a = 8192,
             "cpu_core: logical core to pin the calling (producer) thread to. "
             "ring_capacity: ring size, rounded up to a power of two. "
             "drain_batch_capacity: scratch buffer size in records -- also the "
             "hard upper bound on a single poll() call's batch size.")
        .def("ingest", &ReplayStream::ingest, "packet"_a,
             "Push one raw packet (buffer-protocol object, e.g. bytes or a numpy "
             "uint8 array) of at least WIRE_TICK_SIZE bytes. Never blocks; "
             "returns False if the ring is full or the packet is too short.")
        .def("poll", &ReplayStream::poll, "max_count"_a,
             // Deliberately NOT nb::call_guard<gil_scoped_release>: poll()'s
             // body ends by calling nb::find(*this) to build the returned
             // ndarray's owner reference, which requires the GIL. The
             // engine-side work poll() does (popping a bounded batch off
             // the ring) is short and non-blocking, so there is no
             // meaningful benefit to releasing the GIL around it.
             "Drain up to max_count entries into a zero-copy view over "
             "WIRE_FORMAT-shaped records -- see the C++ docstring in "
             "python/animus_py.cpp for the buffer-lifetime contract.")
        .def("telemetry", &ReplayStream::telemetry,
             "Snapshot of ingested/dropped/malformed counters and ring occupancy.")
        .def_prop_ro("drain_batch_capacity", &ReplayStream::drain_batch_capacity);
}
