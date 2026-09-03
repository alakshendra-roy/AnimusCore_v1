// Animus Engine -- Zero-Copy Cross-Process Shared-Memory Interop (nanobind)
//
// Companion to animus_py.cpp, and deliberately distinct from it:
// animus_py.cpp's TelemetryStream binds animus::SpscRingBuffer<T>, an
// *in-process* ring invisible outside the Python interpreter that loaded
// this extension. This file binds animus::sys::ipc::ShmRing<T>
// (include/animus/shm_ipc.hpp) instead -- a ring that lives entirely
// inside a named OS shared-memory segment (Windows: CreateFileMapping;
// POSIX: shm_open/mmap under /dev/shm), so a native C++ producer process
// (e.g. benchmarks/harness_benchmark.cpp) and this Python process can
// exchange records with no serialization step and no copy across the
// process boundary itself -- only the same one memcpy-equivalent per
// event that any ring pop() already costs, matching animus_py.cpp's own
// stated zero-copy contract (see that file's header comment) rather than
// promising something stronger.
//
// Wire format: WireRecord below is animus::SharedTelemetryRecord
// (animus.hpp) -- the same 24-byte <QIIQ> layout animus.shm and
// animus_py.cpp already use, reused here rather than inventing a fourth
// record shape, so animus/consumer.py's existing decode()/decode_iter()/
// to_numpy() work unchanged against a poll() result from this module too.
//
// GIL discipline (Milestone 3's explicit requirement): poll()'s spin-wait
// over the shared ring runs entirely inside a nb::gil_scoped_release
// block -- a lagging or momentarily-absent producer in another process
// never blocks Python's other threads. The GIL is reacquired (implicitly,
// when that scope ends) before constructing the returned ndarray, for the
// same reason drain() in animus_py.cpp does not release the GIL for its
// own ndarray construction: nb::find() is a Python-API call. push()/
// push_overwrite() are not spin/blocking calls (ShmRing's try_push/
// push_overwrite never wait), so they run under the GIL like any other
// fast native call -- releasing it around them would only add overhead.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "animus.hpp"
#include "animus/shm_ipc.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

using WireRecord = animus::SharedTelemetryRecord;
static_assert(sizeof(WireRecord) == 24, "must stay wire-compatible with animus::SharedTelemetryRecord");
constexpr const char* kWireFormat = "<QIIQ";

using Ring = animus::sys::ipc::ShmRing<WireRecord>;

// Thin Python-facing wrapper over one attached ShmRing<WireRecord>. Exactly
// one of create()/open() is called per instance (mirroring ShmRing's own
// create-vs-open split): create() allocates and owns the underlying OS
// segment; open() attaches to one another process already created. Only
// the owning side should ever call unlink().
class SharedExecutionChannel {
public:
    static SharedExecutionChannel create(const std::string& name, size_t capacity, size_t drain_batch_capacity) {
        auto ring = Ring::create(name.c_str(), capacity);
        if (!ring) {
            throw std::runtime_error(
                "ShmRing::create('" + name + "') failed -- a segment with this name "
                "may already exist, or the OS refused the shared-memory allocation");
        }
        ring->mark_producer_attached();
        return SharedExecutionChannel(std::move(ring), drain_batch_capacity, /*is_owner=*/true, name);
    }

    static SharedExecutionChannel open(const std::string& name, size_t drain_batch_capacity) {
        auto ring = Ring::open(name.c_str());
        if (!ring) {
            throw std::runtime_error(
                "ShmRing::open('" + name + "') failed -- no such segment, or its header "
                "is not a valid ShmRing<WireRecord> (wrong record type or a torn/foreign segment)");
        }
        ring->mark_consumer_attached();
        return SharedExecutionChannel(std::move(ring), drain_batch_capacity, /*is_owner=*/false, name);
    }

    SharedExecutionChannel(const SharedExecutionChannel&) = delete;
    SharedExecutionChannel& operator=(const SharedExecutionChannel&) = delete;
    SharedExecutionChannel(SharedExecutionChannel&&) noexcept = default;
    SharedExecutionChannel& operator=(SharedExecutionChannel&&) noexcept = default;

    // Producer-side. Never blocks; False means the ring was full and the
    // event was refused (bounded-backpressure contract -- see push_overwrite
    // for the decoupled/lossy alternative).
    bool push(uint32_t event_id, uint32_t trace_id, uint64_t metric_value) noexcept {
        const WireRecord rec{animus::read_cycle_counter(), event_id, trace_id, metric_value};
        return ring_->try_push(rec);
    }

    // Producer-side. Never blocks and never refuses: if the ring is full,
    // reclaims the oldest unconsumed slot and increments dropped_count()
    // instead -- see ShmRing::push_overwrite's own doc comment
    // (include/animus/shm_ipc.hpp) for the exact concurrency contract.
    void push_overwrite(uint32_t event_id, uint32_t trace_id, uint64_t metric_value) noexcept {
        const WireRecord rec{animus::read_cycle_counter(), event_id, trace_id, metric_value};
        ring_->push_overwrite(rec);
    }

    // Consumer-side. Pops up to min(max_count, drain_batch_capacity())
    // events, spin-waiting (with animus::cpu_relax() between attempts, via
    // ShmRing::pop_spin) up to max_spins times per event for the producer
    // to catch up before giving up and returning whatever was collected so
    // far -- possibly an empty (0, record_size)-shaped array if none
    // arrived in time. The entire spin-wait runs with the GIL released
    // (see file header); it is reacquired automatically before the
    // returned ndarray is constructed.
    //
    // THE RETURNED VIEW ALIASES THIS OBJECT'S SCRATCH BUFFER -- valid only
    // until the next poll() call on this same object. Same lifetime
    // contract as animus_py.cpp's TelemetryStream.drain(); see
    // animus/consumer.py's decode()/decode_iter()/to_numpy() for how to
    // copy or reinterpret it before that happens.
    nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> poll(size_t max_count, uint64_t max_spins) {
        const size_t limit = max_count < scratch_.size() ? max_count : scratch_.size();
        size_t n = 0;
        {
            nb::gil_scoped_release release; // hot spin-wait: no Python API touched in here
            WireRecord rec;
            while (n < limit && ring_->pop_spin(rec, max_spins)) {
                scratch_[n++] = rec;
            }
        }
        return nb::ndarray<uint8_t, nb::memview, nb::ndim<2>>(
            reinterpret_cast<uint8_t*>(scratch_.data()),
            {n, sizeof(WireRecord)},
            nb::find(*this)
        );
    }

    // Consumer-side, non-blocking: pops whatever is immediately available
    // (no spin-wait at all), up to drain_batch_capacity(). Equivalent to
    // poll(max_count, max_spins=1) but doesn't pay even one cpu_relax();
    // prefer this in a Python-side loop that wants to interleave other
    // work between polls rather than spin natively.
    nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> drain(size_t max_count) {
        return poll(max_count, /*max_spins=*/1);
    }

    void mark_producer_attached() noexcept { ring_->mark_producer_attached(); }
    void mark_consumer_attached() noexcept { ring_->mark_consumer_attached(); }
    void producer_heartbeat() noexcept { ring_->producer_heartbeat(); }
    void consumer_heartbeat() noexcept { ring_->consumer_heartbeat(); }
    bool is_producer_alive(uint64_t stale_after) const noexcept { return ring_->is_producer_alive(stale_after); }
    bool is_consumer_alive(uint64_t stale_after) const noexcept { return ring_->is_consumer_alive(stale_after); }

    // Destroys the underlying OS shared-memory object. Owner-only (the
    // side that called create(), not open()) -- call only after every
    // attached process, including this one, is done with the segment.
    // Raises rather than silently no-op'ing if called from the non-owning
    // side, since that call would either fail outright (POSIX) or be a
    // confusing no-op (Windows) and either way does not mean what the
    // caller likely intended.
    void unlink() {
        if (!is_owner_) {
            throw std::runtime_error(
                "unlink() called on a channel opened with open(), not create() -- "
                "only the owning (creating) side should unlink the underlying segment");
        }
        Ring::unlink(name_.c_str());
    }

    uint64_t dropped_count() const noexcept { return ring_->dropped_count(); }
    size_t capacity() const noexcept { return ring_->capacity(); }
    size_t drain_batch_capacity() const noexcept { return scratch_.size(); }
    bool is_owner() const noexcept { return is_owner_; }
    const std::string& name() const noexcept { return name_; }

private:
    SharedExecutionChannel(std::unique_ptr<Ring> ring, size_t drain_batch_capacity, bool is_owner, std::string name)
        : ring_(std::move(ring)),
          scratch_(drain_batch_capacity == 0 ? size_t{1} : drain_batch_capacity),
          is_owner_(is_owner),
          name_(std::move(name)) {
    }

    std::unique_ptr<Ring> ring_;
    std::vector<WireRecord> scratch_;
    bool is_owner_;
    std::string name_;
};

} // namespace

NB_MODULE(_animus_shm_native, m) {
    m.doc() = "Animus Engine -- nanobind zero-copy interop over animus::sys::ipc::ShmRing<WireRecord> "
               "(cross-process, OS shared-memory backed; see bindings/animus_shm_py.cpp)";
    m.attr("WIRE_FORMAT") = kWireFormat;
    m.attr("WIRE_RECORD_SIZE") = sizeof(WireRecord);

    nb::class_<SharedExecutionChannel>(m, "SharedExecutionChannel")
        .def_static("create", &SharedExecutionChannel::create,
             "name"_a, "capacity"_a, "drain_batch_capacity"_a = 8192,
             "Allocate a new named OS shared-memory ring and take ownership of it. "
             "Fails (raises RuntimeError) if a segment with this name already exists.")
        .def_static("open", &SharedExecutionChannel::open,
             "name"_a, "drain_batch_capacity"_a = 8192,
             "Attach to an existing named OS shared-memory ring created by another "
             "process's create() call (C++ or Python -- same wire format either way).")
        .def("push", &SharedExecutionChannel::push, "event_id"_a, "trace_id"_a, "metric_value"_a,
             "Producer-side. Never blocks; returns False if the ring is full.")
        .def("push_overwrite", &SharedExecutionChannel::push_overwrite,
             "event_id"_a, "trace_id"_a, "metric_value"_a,
             "Producer-side, decoupled/overwrite mode: never blocks and never refuses -- "
             "reclaims the oldest unconsumed slot on a full ring instead. See dropped_count.")
        .def("poll", &SharedExecutionChannel::poll, "max_count"_a, "max_spins"_a = 200000,
             "Consumer-side. Spin-waits (GIL released) up to max_spins times per event for "
             "the producer to catch up, then returns a zero-copy view of whatever arrived -- "
             "see the C++ docstring in bindings/animus_shm_py.cpp for the buffer-lifetime contract.")
        .def("drain", &SharedExecutionChannel::drain, "max_count"_a,
             "Consumer-side, non-blocking: pop() only what's immediately available.")
        .def("mark_producer_attached", &SharedExecutionChannel::mark_producer_attached)
        .def("mark_consumer_attached", &SharedExecutionChannel::mark_consumer_attached)
        .def("producer_heartbeat", &SharedExecutionChannel::producer_heartbeat)
        .def("consumer_heartbeat", &SharedExecutionChannel::consumer_heartbeat)
        .def("is_producer_alive", &SharedExecutionChannel::is_producer_alive, "stale_after"_a = 0)
        .def("is_consumer_alive", &SharedExecutionChannel::is_consumer_alive, "stale_after"_a = 0)
        .def("unlink", &SharedExecutionChannel::unlink,
             "Destroy the underlying OS shared-memory object. Owner-only -- call after "
             "every attached process, including this one, is done with the segment.")
        .def_prop_ro("dropped_count", &SharedExecutionChannel::dropped_count)
        .def_prop_ro("capacity", &SharedExecutionChannel::capacity)
        .def_prop_ro("drain_batch_capacity", &SharedExecutionChannel::drain_batch_capacity)
        .def_prop_ro("is_owner", &SharedExecutionChannel::is_owner)
        .def_prop_ro("name", &SharedExecutionChannel::name);
}
