// SPDX-License-Identifier: Apache-2.0
// python_bridge.cpp -- zero-copy nanobind bridge over an AnimusCore telemetry slot buffer.
//
// TelemetryBuffer owns one cache-line-aligned block laid out as animus::sys::ipc::SpscQueue<
// animus::TelemetryPayload> (include/animus/spsc_queue.hpp): a header whose head and tail indices each
// own a cache line, then `capacity` 64-byte slots. slots() hands that slot array to Python as a NumPy
// array that ALIASES the C++ memory: no copy is made, no memory is allocated, and a record the native
// producer publishes is visible through an array obtained earlier without calling slots() again.
//
// The three things that make that safe, and that a copying binding would silently get wrong:
//   * Lifetime. slots() is bound with rv_policy::reference_internal, so nanobind attaches the Python
//     TelemetryBuffer as the owner of the returned array; the block cannot be freed while any view of
//     it (or any NumPy view derived from that view) is alive.
//   * No copy. With the default return policy, nanobind exports an ndarray that has no owner by COPYING
//     it (nb_ndarray.cpp: copy = owner == nullptr && self == nullptr). That is exactly what the policy
//     above prevents; python/test_python_bridge.py proves it by comparing addresses rather than assuming it.
//   * Read-only. The view is `const`, so NumPy marks it non-writeable: Python cannot scribble on
//     the producer's memory.
//
// The consumer contract, which is the engine's own SPSC protocol (spsc_queue.hpp): the native producer
// publishes slot i by a release store of head. Python is the single consumer and may read slot
// (i & (capacity - 1)) for every i in [tail, head), after reading `head` (acquire load, in the
// property below). Those slots are not being written. consume(n) retires n of them with a release
// store of tail, after which the producer may overwrite them AT ANY TIME. The view does not know
// this: slots outside [tail, head), and any slot after it has been consumed, are mid-write or stale.
//
// The slot layout is TelemetryPayload's (animus/ring_buffers.hpp; 64 bytes, 64-byte aligned, little-endian):
//     offset  0  uint64  timestamp_cycles          offset  8  uint32  event_id
//     offset 12  uint32  trace_id                  offset 16  uint64  metric_value      (24..63: padding)
// SLOT_FIELDS exports exactly that, so Python can build a structured dtype without this module
// importing NumPy: np.dtype({'names': [...], 'formats': [...], 'offsets': [...], 'itemsize': 64}).
//
// The buffer is ONE SPSC queue: a single consumer (Python) and a single producer, which is either push()
// or a thread running produce() (a native loop that releases the GIL, so Python can consume
// concurrently), never both at once. It is not meant to be shared across processes.
//
// Verified with nanobind 3.0.1 and NumPy 2.5. NumPy is needed at run time (the views are NumPy arrays),
// nanobind only to build. See CMakeLists.txt and python/test_python_bridge.py. From the package root:
//     cmake -S . -B build/bridge -DCMAKE_BUILD_TYPE=Release && cmake --build build/bridge
//     python python/test_python_bridge.py        (finds the module under build/)
// or: make pybridge
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include "animus/ring_buffers.hpp"
#include "animus/spsc_queue.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    #error "SLOT_FIELDS describes the slot as little-endian; this bridge supports little-endian targets only"
#endif

namespace nb = nanobind;

namespace {

using animus::TelemetryPayload;
using animus::sys::ipc::SpscQueue;
using animus::sys::ipc::SpscQueueHeader;
using Queue = SpscQueue<TelemetryPayload>;

// The layout Python is told about must be the layout the compiler produced.
constexpr std::size_t kSlotWords = sizeof(TelemetryPayload) / sizeof(std::uint64_t);
static_assert(sizeof(TelemetryPayload) == 64 && alignof(TelemetryPayload) == 64, "a slot is one 64-byte cache line");
static_assert(kSlotWords == 8, "SlotView below is declared shape<-1, 8>");
static_assert(std::is_standard_layout_v<TelemetryPayload> && std::is_trivially_copyable_v<TelemetryPayload>,
              "slots are read as raw bytes from another language");
static_assert(offsetof(TelemetryPayload, timestamp_cycles) == 0 && offsetof(TelemetryPayload, event_id) == 8 &&
                  offsetof(TelemetryPayload, trace_id) == 12 && offsetof(TelemetryPayload, metric_value) == 16,
              "SLOT_FIELDS offsets");
static_assert(sizeof(SpscQueueHeader) % alignof(TelemetryPayload) == 0, "slots start on a cache line");

constexpr std::align_val_t kBlockAlign{ ANIMUS_CACHE_LINE_SIZE };

struct BlockDeleter {
    void operator()(unsigned char* p) const noexcept { ::operator delete(p, kBlockAlign); }
};

// (capacity, 8) uint64, read-only, NumPy. `const` is what makes the exported array non-writeable.
using SlotView = nb::ndarray<nb::numpy, const std::uint64_t, nb::shape<-1, 8>>;

class TelemetryBuffer {
public:
    explicit TelemetryBuffer(std::size_t capacity) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw nb::value_error("capacity must be a power of two, at least 2");
        }
        constexpr std::size_t kMaxCapacity =
            (std::numeric_limits<std::size_t>::max() - sizeof(SpscQueueHeader)) / sizeof(TelemetryPayload);
        if (capacity > kMaxCapacity) throw nb::value_error("capacity is too large");

        const std::size_t bytes = Queue::required_bytes(capacity);
        block_.reset(static_cast<unsigned char*>(::operator new(bytes, kBlockAlign))); // std::bad_alloc -> MemoryError
        // Zero everything: Python must never read stale heap bytes through the view, and touching every
        // page here keeps page faults out of the producer's path later.
        std::memset(block_.get(), 0, bytes);
        if (!Queue::init(block_.get(), bytes, capacity, producer_)) {
            throw nb::value_error("could not initialise the queue"); // unreachable: the arguments were validated above
        }
        capacity_ = capacity;
    }

    TelemetryBuffer(const TelemetryBuffer&) = delete;
    TelemetryBuffer& operator=(const TelemetryBuffer&) = delete;

    // The native producer, standing in for an engine thread: one release store of head publishes the slot.
    // False when the queue is full (backpressure; nothing is overwritten).
    bool push(std::uint64_t timestamp_cycles, std::uint32_t event_id, std::uint32_t trace_id, std::uint64_t metric_value) noexcept {
        TelemetryPayload p{};
        p.timestamp_cycles = timestamp_cycles;
        p.event_id = event_id;
        p.trace_id = trace_id;
        p.metric_value = metric_value;
        return producer_.try_push(p);
    }

    // A native producer loop. Publishes `count` records on the calling thread with the GIL RELEASED,
    // yielding while the buffer is full, so a Python thread can consume while it runs. Record number
    // n (n = head at entry + k) is
    //     timestamp_cycles = n,  event_id = n & 0xFFFFFFFF,  trace_id = n >> 32,  metric_value = mix(n)
    // with mix(x) = (x * 0x9E3779B97F4A7C15) ^ (x >> 29), modulo 2^64. One producer at a time: do not
    // call push() while this is running.
    // It never spins forever: if the buffer stays full for stall_timeout_ms (nobody is consuming), it
    // stops. Returns how many records it published, which is `count` unless it gave up.
    std::uint64_t produce(std::uint64_t count, std::uint32_t stall_timeout_ms) {
        nb::gil_scoped_release release;
        const std::uint64_t base = header().head.load(std::memory_order_relaxed); // the producer is head's only writer
        const std::chrono::milliseconds limit(stall_timeout_ms);
        for (std::uint64_t k = 0; k < count; ++k) {
            const std::uint64_t n = base + k;
            TelemetryPayload p{};
            p.timestamp_cycles = n;
            p.event_id = static_cast<std::uint32_t>(n);
            p.trace_id = static_cast<std::uint32_t>(n >> 32);
            p.metric_value = (n * 0x9E3779B97F4A7C15ull) ^ (n >> 29);
            if (producer_.try_push(p)) continue;
            const auto stalled_since = std::chrono::steady_clock::now();
            do {
                if (std::chrono::steady_clock::now() - stalled_since > limit) return k;
                std::this_thread::yield();
            } while (!producer_.try_push(p));
        }
        return count;
    }

    // Records ever published / ever consumed. Slots [tail, head) are readable. head is an acquire load:
    // read it BEFORE reading the slots it covers.
    std::uint64_t head() const noexcept { return header().head.load(std::memory_order_acquire); }
    std::uint64_t tail() const noexcept { return header().tail.load(std::memory_order_acquire); }

    // Retires n published slots (release store of tail); the producer may reuse them from then on.
    void consume(std::uint64_t n) {
        SpscQueueHeader& h = header();
        const std::uint64_t t = h.tail.load(std::memory_order_relaxed); // Python is tail's only writer
        const std::uint64_t published = h.head.load(std::memory_order_acquire) - t;
        if (n > published) throw nb::value_error("consume(n): n exceeds the published, unconsumed slots");
        h.tail.store(t + n, std::memory_order_release);
    }

    // The whole slot array as (capacity, 8) uint64 aliasing the C++ memory. Bound with reference_internal.
    SlotView slots() const {
        const std::size_t shape[2] = { capacity_, kSlotWords };
        return SlotView(slot_base(), 2, shape);
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t nbytes() const noexcept { return capacity_ * sizeof(TelemetryPayload); }
    // Address of slot 0: what slots().__array_interface__['data'][0] must equal if nothing was copied.
    std::uintptr_t data_ptr() const noexcept { return reinterpret_cast<std::uintptr_t>(slot_base()); }

private:
    SpscQueueHeader& header() const noexcept {
        return *std::launder(static_cast<SpscQueueHeader*>(static_cast<void*>(block_.get())));
    }
    const void* slot_base() const noexcept { return block_.get() + sizeof(SpscQueueHeader); }

    std::unique_ptr<unsigned char, BlockDeleter> block_;
    Queue producer_; // the producer-side view: caches the consumer's index, process-local
    std::size_t capacity_ = 0;
};

} // namespace

NB_MODULE(animus_bridge, m) {
    m.doc() = "Zero-copy bridge over an AnimusCore telemetry slot buffer (SpscQueue<TelemetryPayload>).";

    m.attr("SLOT_BYTES") = sizeof(TelemetryPayload);
    m.attr("SLOT_WORDS") = kSlotWords;
    // (name, NumPy typestr, byte offset) for every named field of a slot, little-endian.
    m.attr("SLOT_FIELDS") = nb::make_tuple(
        nb::make_tuple("timestamp_cycles", "<u8", offsetof(TelemetryPayload, timestamp_cycles)),
        nb::make_tuple("event_id", "<u4", offsetof(TelemetryPayload, event_id)),
        nb::make_tuple("trace_id", "<u4", offsetof(TelemetryPayload, trace_id)),
        nb::make_tuple("metric_value", "<u8", offsetof(TelemetryPayload, metric_value)));

    nb::class_<TelemetryBuffer>(m, "TelemetryBuffer", nb::is_weak_referenceable(),
                                "A cache-line-aligned SPSC slot buffer whose slots are exposed to NumPy without a copy.")
        .def(nb::init<std::size_t>(), nb::arg("capacity"),
             "capacity: number of 64-byte slots; a power of two, at least 2. The block is zeroed and pre-faulted here.")
        .def("push", &TelemetryBuffer::push, nb::arg("timestamp_cycles"), nb::arg("event_id"), nb::arg("trace_id"),
             nb::arg("metric_value"),
             "Native-side producer stand-in. Publishes one record; returns False (and writes nothing) if the buffer is full.")
        .def("produce", &TelemetryBuffer::produce, nb::arg("count"), nb::arg("stall_timeout_ms") = 5000,
             "Native producer loop with the GIL released: publishes `count` synthetic records, waiting while the "
             "buffer is full, and gives up if it stays full for stall_timeout_ms. Returns the number published. "
             "Run it in a threading.Thread and consume from another; see the header comment for the record formula.")
        .def("consume", &TelemetryBuffer::consume, nb::arg("n"),
             "Retire n published slots so the producer can reuse them. Raises ValueError if n exceeds head - tail.")
        .def("slots", &TelemetryBuffer::slots, nb::rv_policy::reference_internal,
             "Read-only (capacity, 8) uint64 NumPy array ALIASING the slot memory (no copy). Read only slots "
             "[tail, head), by index (i & (capacity - 1)); anything else may be mid-write or stale.")
        .def_prop_ro("capacity", &TelemetryBuffer::capacity)
        .def_prop_ro("head", &TelemetryBuffer::head, "Records ever published (acquire load).")
        .def_prop_ro("tail", &TelemetryBuffer::tail, "Records ever consumed.")
        .def_prop_ro("nbytes", &TelemetryBuffer::nbytes, "Size of the slot array in bytes.")
        .def_prop_ro("data_ptr", &TelemetryBuffer::data_ptr, "Address of slot 0.");
}
