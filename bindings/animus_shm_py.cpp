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
// Wire format: WireRecord below is animus::ExecutionEvent
// (include/animus/execution_event.hpp) -- the same 40-byte layout
// benchmarks/harness_benchmark.cpp writes into the ring and
// benchmarks/consumer.py decodes by hand. SharedExecutionChannel stays
// hardcoded to this one type deliberately: it is the primary,
// backward-compatible fast path (animus::schema::Traits<ExecutionEvent>,
// include/animus/schema.hpp), unchanged since before Milestone 1.
//
// Milestone 1 (Dynamic & User-Defined Wire Schemas) adds SharedSchemaChannel
// below it: a second, genuinely schema-agnostic binding over
// animus::sys::ipc::RawSchemaView (shm_ipc.hpp) that attaches to a segment
// without ever naming T at compile time, reading whatever
// payload_size/stride/schema_version_hash/wire_format ShmRing<T>::create()
// stamped into the header instead. Since ShmRing<T>::open() now validates
// that header against the attaching side's own T (a real gap this
// milestone closes -- previously two same-size T's could attach to the
// same segment and silently misread each other's fields, with no
// cross-check possible), SharedSchemaChannel is what lets a Python
// consumer inspect *any* registered custom schema (MarketDepthEvent,
// OrderBookL2, AlphaSignal, ...) and build a matching NumPy structured
// dtype from wire_format() at runtime, without a new compiled extension
// per schema.
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
#include "animus/execution_event.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

using WireRecord = animus::ExecutionEvent;
static_assert(sizeof(WireRecord) == 40, "must stay wire-compatible with animus::ExecutionEvent");
constexpr const char* kWireFormat = animus::kExecutionEventWireFormat;

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
    // for the decoupled/lossy alternative). dispatch_ts_raw is stamped here
    // via animus::read_cycle_counter() (the same RDTSC-or-monotonic-clock
    // helper animus.hpp uses throughout) -- not a serialized/calibrated
    // read the way harness_benchmark.cpp's own sample_clock() is, so
    // latencies computed against a Python-side push() are directly
    // comparable to each other but not bit-for-bit comparable to a run
    // produced by the C++ harness.
    bool push(uint64_t sequence, int64_t price_ticks, int64_t quantity, uint32_t instrument_id) noexcept {
        const WireRecord rec{sequence, animus::read_cycle_counter(), price_ticks, quantity, instrument_id, 0};
        return ring_->try_push(rec);
    }

    // Producer-side. Never blocks and never refuses: if the ring is full,
    // reclaims the oldest unconsumed slot and increments dropped_count()
    // instead -- see ShmRing::push_overwrite's own doc comment
    // (include/animus/shm_ipc.hpp) for the exact concurrency contract.
    void push_overwrite(uint64_t sequence, int64_t price_ticks, int64_t quantity, uint32_t instrument_id) noexcept {
        const WireRecord rec{sequence, animus::read_cycle_counter(), price_ticks, quantity, instrument_id, 0};
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

// Milestone 1: schema-agnostic Python attach. Wraps
// animus::sys::ipc::RawSchemaView (include/animus/shm_ipc.hpp) -- unlike
// SharedExecutionChannel above, this never names a C++ record type, so it
// can attach to a segment created for ExecutionEvent, OrderBookL2
// (animus::schema, include/animus/schema.hpp), or any other struct a
// client registered with ANIMUS_DEFINE_SCHEMA, purely by reading the
// wire descriptor ShmRing<T>::create() stamped into the header. Read-only
// by design (no push/push_overwrite): a Python consumer that also needs
// to *produce* a custom-schema record still needs a compiled binding for
// that concrete T (there is no way to construct an arbitrary trivially-
// copyable C++ struct from untyped Python bytes without one), but reading
// an existing stream and decoding it -- the actual "dynamic unpacker"
// this milestone asks for -- needs no such binding.
class SharedSchemaChannel {
public:
    static SharedSchemaChannel open(const std::string& name) {
        auto view = animus::sys::ipc::RawSchemaView::open(name.c_str());
        if (!view) {
            throw std::runtime_error(
                "RawSchemaView::open('" + name + "') failed -- no such segment, or its header "
                "is not a valid Milestone-1 schema-descriptor ring (too old, or a torn/foreign segment)");
        }
        return SharedSchemaChannel(std::move(view), name);
    }

    uint64_t schema_version_hash() const noexcept { return view_->schema_version_hash(); }
    size_t payload_size() const noexcept { return static_cast<size_t>(view_->payload_size()); }
    size_t stride() const noexcept { return static_cast<size_t>(view_->stride()); }
    std::string wire_format() const { return std::string(view_->wire_format()); }
    size_t capacity() const noexcept { return static_cast<size_t>(view_->capacity()); }
    uint64_t head() const noexcept { return view_->head(); }
    uint64_t tail() const noexcept { return view_->tail(); }
    uint64_t dropped_count() const noexcept { return view_->dropped_count(); }
    const std::string& name() const noexcept { return name_; }

    // Zero-copy raw view of the ENTIRE slot array as a (capacity, stride)
    // uint8 ndarray -- byte-for-byte the same memory layout as the C++
    // side, with no copy and no dependency on knowing T. wire_format()
    // (a struct.calcsize-compatible format string, e.g. "<QQqqII") is
    // what lets a caller reinterpret this as a structured NumPy dtype;
    // see animus/shm.py's schema helpers for that conversion. Only
    // [tail(), head()) mod capacity() is data an active producer has
    // actually published and not yet overwritten -- same reader contract
    // ShmRing<T>'s own try_pop()/pop_spin() have, just left for the
    // caller to apply here instead of being enforced by a pop() call.
    nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> raw_view() {
        return nb::ndarray<uint8_t, nb::memview, nb::ndim<2>>(
            reinterpret_cast<uint8_t*>(view_->slots_base()),
            {view_->capacity(), view_->stride()},
            nb::find(*this)
        );
    }

private:
    SharedSchemaChannel(std::unique_ptr<animus::sys::ipc::RawSchemaView> view, std::string name)
        : view_(std::move(view)), name_(std::move(name)) {
    }

    std::unique_ptr<animus::sys::ipc::RawSchemaView> view_;
    std::string name_;
};

} // namespace

NB_MODULE(_animus_shm_native, m) {
    m.doc() = "Animus Engine -- nanobind zero-copy interop over animus::sys::ipc::ShmRing<ExecutionEvent> "
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
        .def("push", &SharedExecutionChannel::push,
             "sequence"_a, "price_ticks"_a, "quantity"_a, "instrument_id"_a,
             "Producer-side. Never blocks; returns False if the ring is full.")
        .def("push_overwrite", &SharedExecutionChannel::push_overwrite,
             "sequence"_a, "price_ticks"_a, "quantity"_a, "instrument_id"_a,
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

    nb::class_<SharedSchemaChannel>(m, "SharedSchemaChannel")
        .def_static("open", &SharedSchemaChannel::open, "name"_a,
             "Attach read-only to an existing named OS shared-memory ring, without knowing "
             "its record type at compile time -- reads whatever schema descriptor "
             "ShmRing<T>::create() stamped into the header (payload_size, stride, "
             "schema_version_hash, wire_format). Works for ExecutionEvent, OrderBookL2, or "
             "any other schema registered via ANIMUS_DEFINE_SCHEMA (include/animus/schema.hpp).")
        .def("raw_view", &SharedSchemaChannel::raw_view,
             "Zero-copy (capacity, stride) uint8 view of the entire slot array, matching the "
             "C++ memory layout byte-for-byte. Combine with wire_format to decode as a "
             "structured NumPy dtype, and with head/tail/capacity to bound the valid range.")
        .def_prop_ro("schema_version_hash", &SharedSchemaChannel::schema_version_hash)
        .def_prop_ro("payload_size", &SharedSchemaChannel::payload_size)
        .def_prop_ro("stride", &SharedSchemaChannel::stride)
        .def_prop_ro("wire_format", &SharedSchemaChannel::wire_format)
        .def_prop_ro("capacity", &SharedSchemaChannel::capacity)
        .def_prop_ro("head", &SharedSchemaChannel::head)
        .def_prop_ro("tail", &SharedSchemaChannel::tail)
        .def_prop_ro("dropped_count", &SharedSchemaChannel::dropped_count)
        .def_prop_ro("name", &SharedSchemaChannel::name);
}
