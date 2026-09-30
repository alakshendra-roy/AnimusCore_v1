// Animus Engine -- Zero-Copy Cross-Process Shared-Memory Interop (nanobind)
//
// Companion to animus_py.cpp, and deliberately distinct from it:
// animus_py.cpp's TelemetryStream binds animus::SpscRingBuffer<T>, an
// *in-process* ring invisible outside the Python interpreter that loaded
// this extension. This file binds rings that live entirely inside a named OS
// shared-memory segment (Windows: CreateFileMapping; POSIX: shm_open/mmap
// under /dev/shm), so a native C++ process and this Python process exchange
// records with no serialization step and no copy across the process boundary
// itself -- only the one memcpy-equivalent per record that any ring read
// already costs.
//
// Two primitives, one per traffic class (see the headers for the protocols):
//
//   BroadcastRing  (include/animus/broadcast_ring.hpp) -- lossy 1-writer ->
//       N-reader market-data/telemetry ring. publish() never blocks and
//       never refuses; every reader keeps its own cursor in its own process
//       memory, and one that falls more than capacity-1 records behind is
//       snapped forward with the loss counted into dropped_count().
//
//   SpscQueue      (include/animus/spsc_queue.hpp) -- lossless 1-producer ->
//       1-consumer execution/order queue. try_push() returns False on a full
//       queue (strict backpressure); nothing is ever overwritten.
//
// Both carry WireRecord below, which is animus::ExecutionEvent
// (include/animus/execution_event.hpp): the same 40-byte layout
// benchmarks/consumer.py decodes by hand. It is exposed to Python as the
// ExecutionEvent class -- SpscQueue.try_push()/try_pop() move one of those
// per call, while BroadcastRing.poll() returns a batch as a zero-copy
// (n, WIRE_RECORD_SIZE) uint8 view instead, so the hot path allocates nothing
// per record.
//
// SharedSchemaChannel at the bottom is unchanged: a schema-agnostic,
// read-only attach (animus::sys::ipc::RawSchemaView, shm_ipc.hpp) to
// segments laid out by the legacy ShmRing<T>, which is still what the C++
// ITCH bridge produces. BroadcastRing/SpscQueue segments carry no wire_format
// descriptor, so it cannot attach to them.
//
// GIL discipline: every call here is non-blocking (a bounded memcpy loop at
// most), so none of them releases the GIL -- releasing and reacquiring it
// would cost more than the work it would unblock.

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "animus/broadcast_ring.hpp"
#include "animus/execution_event.hpp"
#include "animus/shm_ipc.hpp"
#include "animus/spsc_queue.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

using WireRecord = animus::ExecutionEvent;
static_assert(sizeof(WireRecord) == 40, "must stay wire-compatible with animus::ExecutionEvent");
constexpr const char* kWireFormat = animus::kExecutionEventWireFormat;

using Broadcast = animus::sys::ipc::BroadcastRing<WireRecord>;
using Queue = animus::sys::ipc::SpscQueue<WireRecord>;
using Region = animus::sys::ipc::SharedMemoryRegion;
using RecordBatch = nb::ndarray<uint8_t, nb::memview, nb::ndim<2>>;

// One named shared-memory segment plus the lock-free view over it.
// BroadcastRing/SpscQueue operate on caller-supplied memory and make no OS
// calls, so this owns the mapping: exactly one of create()/open() builds an
// instance. create() allocates the segment and makes this side its owner --
// only the owner may unlink(); open() attaches to one another process (or
// this one) already created.
template <typename Ring>
class Segment {
public:
    static Segment create(const std::string& name, size_t requested_capacity) {
        // Power of two, minimum 2 (the rings index with an AND mask), bounded
        // so required_bytes() cannot overflow size_t.
        const size_t max_capacity =
            (std::numeric_limits<size_t>::max() - Ring::required_bytes(0)) / sizeof(WireRecord);
        size_t capacity = 2;
        while (capacity < requested_capacity) {
            if (capacity > max_capacity / 2) {
                throw std::invalid_argument("capacity " + std::to_string(requested_capacity) + " is too large");
            }
            capacity <<= 1;
        }

        Segment seg;
        if (!Region::create(name.c_str(), Ring::required_bytes(capacity), seg.region_)) {
            throw std::runtime_error(
                "create('" + name + "') failed -- a segment with this name "
                "may already exist, or the OS refused the shared-memory allocation");
        }
        if (!Ring::init(seg.region_.data(), seg.region_.size(), capacity, seg.ring_)) {
            Region::unlink(name.c_str()); // don't leave a half-built segment behind
            throw std::runtime_error("create('" + name + "') failed -- could not initialise the ring header");
        }
        seg.name_ = name;
        seg.is_owner_ = true;
        return seg;
    }

    static Segment open(const std::string& name) {
        Segment seg;
        if (!Region::open(name.c_str(), seg.region_)) {
            throw std::runtime_error("open('" + name + "') failed -- no such segment");
        }
        if (!Ring::attach(seg.region_.data(), seg.region_.size(), seg.ring_)) {
            throw std::runtime_error(
                "open('" + name + "') failed -- the segment is not a valid ring of this kind "
                "and record type (wrong ring kind, wrong record size, or a torn/foreign segment)");
        }
        seg.name_ = name;
        return seg;
    }

    // Destroys the underlying OS shared-memory object. Owner-only: call only
    // after every attached process, including this one, is done with it.
    // Raises rather than silently no-op'ing on a non-owner, since that call
    // would fail outright (POSIX) or do nothing (Windows) and either way not
    // mean what the caller likely intended.
    void unlink() {
        if (!is_owner_) {
            throw std::runtime_error(
                "unlink() called on a channel opened with open(), not create() -- "
                "only the owning (creating) side should unlink the underlying segment");
        }
        Region::unlink(name_.c_str());
    }

    Ring& ring() noexcept { return ring_; }
    const Ring& ring() const noexcept { return ring_; }
    bool is_owner() const noexcept { return is_owner_; }
    const std::string& name() const noexcept { return name_; }

private:
    Segment() = default;

    Region region_;
    Ring ring_;
    std::string name_;
    bool is_owner_ = false;
};

// Lossy broadcast ring. create() takes the writer role (publish()); open()
// attaches an independent reader (poll()) whose cursor lives only in this
// object -- two opens of the same segment, even in one process, never share
// one. The creating handle can also poll() its own ring.
class PyBroadcastRing {
public:
    static PyBroadcastRing create(const std::string& name, size_t capacity, size_t batch_capacity) {
        return PyBroadcastRing(Segment<Broadcast>::create(name, capacity), batch_capacity);
    }

    static PyBroadcastRing open(const std::string& name, size_t batch_capacity) {
        return PyBroadcastRing(Segment<Broadcast>::open(name), batch_capacity);
    }

    // Writer-side. Never blocks and never refuses: overwrites the oldest
    // record once the ring is full. Exactly one writer per segment.
    void publish(const WireRecord& record) noexcept { seg_.ring().publish(record); }

    // Reader-side. Copies up to min(batch_size, batch_capacity) of the oldest
    // unread records into this object's scratch buffer and returns a
    // zero-copy view of them -- possibly (0, WIRE_RECORD_SIZE). Records the
    // writer recycled before they could be read are skipped and added to
    // dropped_count().
    //
    // THE RETURNED VIEW ALIASES THIS OBJECT'S SCRATCH BUFFER -- valid only
    // until the next poll() call on this same object. Copy or reinterpret it
    // first (see animus/consumer.py's decode()/to_numpy()).
    RecordBatch poll(size_t batch_size) {
        const size_t limit = batch_size < scratch_.size() ? batch_size : scratch_.size();
        const size_t n = poll_some(scratch_.data(), limit);
        return RecordBatch(reinterpret_cast<uint8_t*>(scratch_.data()), {n, sizeof(WireRecord)}, nb::find(*this));
    }

    // Cumulative records THIS reader missed because it fell more than
    // capacity-1 behind the writer.
    uint64_t dropped_count() const noexcept { return seg_.ring().dropped(); }

    void unlink() { seg_.unlink(); }
    size_t capacity() const noexcept { return seg_.ring().capacity(); }
    size_t batch_capacity() const noexcept { return scratch_.size(); }
    bool is_owner() const noexcept { return seg_.is_owner(); }
    const std::string& name() const noexcept { return seg_.name(); }

private:
    PyBroadcastRing(Segment<Broadcast>&& seg, size_t batch_capacity)
        : seg_(std::move(seg)), scratch_(batch_capacity == 0 ? size_t{1} : batch_capacity) {}

    // BroadcastRing::poll() can return 0 after discarding a batch the writer
    // lapped mid-copy, even though newer records are already readable. To
    // keep "returned nothing" meaning "caught up", re-poll while unread
    // records remain -- a few times at most, so a writer outrunning this
    // reader cannot pin the caller inside this call.
    size_t poll_some(WireRecord* out, size_t limit) noexcept {
        constexpr int kMaxPollAttempts = 4;
        Broadcast& ring = seg_.ring();
        size_t got = 0;
        for (int attempt = 0; limit != 0 && attempt < kMaxPollAttempts && got == 0; ++attempt) {
            got = ring.poll(out, limit);
            if (got == 0 && ring.read_cursor() >= ring.writer_cursor()) break;
        }
        return got;
    }

    Segment<Broadcast> seg_;
    std::vector<WireRecord> scratch_;
};

// Lossless SPSC queue. One side creates, the other opens; either handle
// supports try_push() and try_pop(), but each direction must be driven by one
// thread/process only.
class PySpscQueue {
public:
    static PySpscQueue create(const std::string& name, size_t capacity) {
        return PySpscQueue(Segment<Queue>::create(name, capacity));
    }

    static PySpscQueue open(const std::string& name) {
        return PySpscQueue(Segment<Queue>::open(name));
    }

    // Producer-side. False means the queue was full and the record was
    // refused -- nothing is overwritten.
    bool try_push(const WireRecord& record) noexcept { return seg_.ring().try_push(record); }

    // Consumer-side. None when the queue is empty.
    std::optional<WireRecord> try_pop() noexcept {
        WireRecord record;
        if (!seg_.ring().try_pop(record)) return std::nullopt;
        return record;
    }

    void unlink() { seg_.unlink(); }
    size_t capacity() const noexcept { return seg_.ring().capacity(); }
    bool is_owner() const noexcept { return seg_.is_owner(); }
    const std::string& name() const noexcept { return seg_.name(); }

private:
    explicit PySpscQueue(Segment<Queue>&& seg) : seg_(std::move(seg)) {}

    Segment<Queue> seg_;
};

// Schema-agnostic Python attach. Wraps
// animus::sys::ipc::RawSchemaView (include/animus/shm_ipc.hpp) -- this never
// names a C++ record type, so it can attach to a legacy ShmRing<T> segment
// created for ExecutionEvent, OrderBookL2 (animus::schema,
// include/animus/schema.hpp), or any other struct a client registered with
// ANIMUS_DEFINE_SCHEMA, purely by reading the wire descriptor
// ShmRing<T>::create() stamped into the header. Read-only by design: there
// is no way to construct an arbitrary trivially-copyable C++ struct from
// untyped Python bytes without a compiled binding for that concrete T, but
// reading an existing stream and decoding it needs no such binding.
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
    m.doc() = "Animus Engine -- nanobind zero-copy interop over named OS shared-memory rings: "
              "BroadcastRing (lossy 1-writer -> N-reader) and SpscQueue (lossless 1 -> 1); "
              "see bindings/animus_shm_py.cpp";
    m.attr("WIRE_FORMAT") = kWireFormat;
    m.attr("WIRE_RECORD_SIZE") = sizeof(WireRecord);

    nb::class_<WireRecord>(m, "ExecutionEvent",
             "One 40-byte wire record (struct format WIRE_FORMAT). Fields are plain read/write "
             "attributes; dispatch_ts_raw is whatever the caller sets -- nothing stamps it.")
        .def("__init__",
             [](WireRecord* self, uint64_t sequence, int64_t price_ticks, int64_t quantity,
                uint32_t instrument_id, uint64_t dispatch_ts_raw, uint32_t flags) {
                 new (self) WireRecord{sequence, dispatch_ts_raw, price_ticks, quantity, instrument_id, flags};
             },
             "sequence"_a = 0, "price_ticks"_a = 0, "quantity"_a = 0, "instrument_id"_a = 0,
             "dispatch_ts_raw"_a = 0, "flags"_a = 0)
        .def_rw("sequence", &WireRecord::sequence)
        .def_rw("dispatch_ts_raw", &WireRecord::dispatch_ts_raw)
        .def_rw("price_ticks", &WireRecord::price_ticks)
        .def_rw("quantity", &WireRecord::quantity)
        .def_rw("instrument_id", &WireRecord::instrument_id)
        .def_rw("flags", &WireRecord::flags);

    nb::class_<PyBroadcastRing>(m, "BroadcastRing",
             "Lossy 1-writer -> N-reader broadcast ring in a named OS shared-memory segment. "
             "The writer never waits; a reader that falls behind skips ahead and counts the loss.")
        .def_static("create", &PyBroadcastRing::create,
             "name"_a, "capacity"_a, "batch_capacity"_a = 8192,
             "Allocate a new named segment (capacity rounded up to a power of two) and take the "
             "writer role and ownership. Raises RuntimeError if the name already exists. Only "
             "capacity - 1 records are ever readable.")
        .def_static("open", &PyBroadcastRing::open,
             "name"_a, "batch_capacity"_a = 8192,
             "Attach an independent reader to an existing segment. Each open() gets its own "
             "cursor, starting at the oldest still-readable record.")
        .def("publish", &PyBroadcastRing::publish, "record"_a,
             "Writer-side. Never blocks and never refuses; overwrites the oldest record when full.")
        .def("poll", &PyBroadcastRing::poll, "batch_size"_a,
             "Reader-side, non-blocking. Returns a zero-copy (n, WIRE_RECORD_SIZE) uint8 view of up "
             "to batch_size of the oldest unread records; n == 0 means caught up. The view aliases "
             "this object's scratch buffer and is valid only until the next poll().")
        .def("unlink", &PyBroadcastRing::unlink,
             "Destroy the underlying OS shared-memory object. Owner-only -- call after every "
             "attached process, including this one, is done with the segment.")
        .def_prop_ro("dropped_count", &PyBroadcastRing::dropped_count,
             "Cumulative records this reader missed because it fell too far behind the writer.")
        .def_prop_ro("capacity", &PyBroadcastRing::capacity)
        .def_prop_ro("batch_capacity", &PyBroadcastRing::batch_capacity)
        .def_prop_ro("is_owner", &PyBroadcastRing::is_owner)
        .def_prop_ro("name", &PyBroadcastRing::name);

    nb::class_<PySpscQueue>(m, "SpscQueue",
             "Lossless 1-producer -> 1-consumer queue in a named OS shared-memory segment. "
             "A full queue refuses the push; nothing is ever overwritten.")
        .def_static("create", &PySpscQueue::create, "name"_a, "capacity"_a,
             "Allocate a new named segment (capacity rounded up to a power of two) and take "
             "ownership. Raises RuntimeError if the name already exists.")
        .def_static("open", &PySpscQueue::open, "name"_a,
             "Attach to an existing segment created by another process's (or this one's) create().")
        .def("try_push", &PySpscQueue::try_push, "record"_a,
             "Producer-side. Returns False if the queue is full (strict backpressure).")
        .def("try_pop", &PySpscQueue::try_pop,
             "Consumer-side. Returns the oldest record, or None if the queue is empty.")
        .def("unlink", &PySpscQueue::unlink,
             "Destroy the underlying OS shared-memory object. Owner-only -- call after every "
             "attached process, including this one, is done with the segment.")
        .def_prop_ro("capacity", &PySpscQueue::capacity)
        .def_prop_ro("is_owner", &PySpscQueue::is_owner)
        .def_prop_ro("name", &PySpscQueue::name);

    nb::class_<SharedSchemaChannel>(m, "SharedSchemaChannel")
        .def_static("open", &SharedSchemaChannel::open, "name"_a,
             "Attach read-only to an existing named OS shared-memory ring, without knowing "
             "its record type at compile time -- reads whatever schema descriptor "
             "ShmRing<T>::create() stamped into the header (payload_size, stride, "
             "schema_version_hash, wire_format). Works for ExecutionEvent, OrderBookL2, or "
             "any other schema registered via ANIMUS_DEFINE_SCHEMA (include/animus/schema.hpp). "
             "Legacy ShmRing<T> segments only -- BroadcastRing/SpscQueue segments carry no descriptor.")
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
