// Animus Engine -- Zero-Copy Python SDK native extension (nanobind)
//
// Binds animus::eval::SpscRingBuffer<TelemetryFrame> (the same C++20
// single-producer/single-consumer ring animus_bench measures, see
// animus-eval-kit/include/spsc_ring_buffer.hpp and benchmarks/animus_harness.cpp)
// directly to Python. This is the institutional client-facing counterpart
// to bindings/animus_py.cpp (which binds animus.hpp's own
// SpscRingBuffer<TelemetryPayload> for the internal engine's telemetry
// ring) and bindings/animus_shm_py.cpp (cross-process shared memory) --
// distinct from both, this module exists specifically so a client
// evaluating animus_bench's throughput claims can pull TelemetryFrame
// records into Python at native speed, not just watch the C++ binary print
// a number.
//
// Zero-copy contract: poll()/drain() pop frames out of the lock-free ring
// into a scratch buffer this object owns once at construction (one
// memcpy-equivalent per frame -- the same cost the ring's own pop() always
// has), then return a Python buffer-protocol view *over that same scratch
// memory*. No per-frame Python object is constructed, no heap allocation
// happens on this path after construction, and no second copy is made to
// hand the batch to Python -- see animus_sdk/consumer.py's to_numpy() for
// how a caller turns that view into a genuinely zero-copy structured NumPy
// array. The returned view aliases the scratch buffer directly, so it is
// only valid until the next poll()/drain() call on the same object.
//
// GIL discipline (explicit project requirement): start_producer() and
// stop_producer() run under nb::call_guard<nb::gil_scoped_release> so the
// native producer thread's lifecycle management (spawn, join) never blocks
// on Python holding the GIL (e.g. mid garbage collection). poll()'s
// spin-wait loop over the ring is likewise released for the wait itself
// (nb::gil_scoped_release), reacquiring only to construct the returned
// ndarray (a Python-API call -- see the matching comment in
// bindings/animus_shm_py.cpp's SharedExecutionChannel::poll for why that
// reacquire is not optional).

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include "animus/thread_affinity.hpp"
#include "spsc_ring_buffer.hpp"
#include "telemetry_frame.hpp"

namespace nb = nanobind;
using namespace nb::literals;

namespace {

using animus::eval::TelemetryFrame;
using animus::eval::kFlagBurst;
constexpr const char* kWireFormat = "<QQ8sdIB27x"; // must track telemetry_frame.hpp's kTelemetryFrameWireFormat

// One producer (either the calling Python thread via push(), or the
// background native thread start_producer() spawns -- never both at once)
// and one consumer (the calling Python thread, via poll()/drain()). Same
// SPSC contract animus::eval::SpscRingBuffer<T> itself documents and does
// not enforce at runtime -- see that header's own comment.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity, size_t drain_batch_capacity)
        : ring_(capacity),
          scratch_(drain_batch_capacity == 0 ? size_t{1} : drain_batch_capacity) {
    }

    ~RingBuffer() { stop_producer(); }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // Producer-side single-frame push, callable directly from Python for a
    // caller wiring up its own real market-data source rather than the
    // synthetic load generator below. Never blocks; returns false if the
    // ring is momentarily full.
    bool push(uint64_t sequence_id, const std::string& symbol, double price,
              uint32_t volume, uint8_t flags) noexcept {
        TelemetryFrame frame{};
        frame.sequence_id = sequence_id;
        frame.timestamp_ns = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const size_t n = symbol.size() < sizeof(frame.symbol) ? symbol.size() : sizeof(frame.symbol);
        std::memcpy(frame.symbol, symbol.data(), n);
        frame.price = price;
        frame.volume = volume;
        frame.flags = flags;
        return ring_.push(frame);
    }

    // Spawns one background native thread generating synthetic
    // TelemetryFrame load at up to target_frames_per_sec (<=0 means
    // unthrottled -- push as fast as the ring accepts, matching
    // benchmarks/animus_harness.cpp's own flood-throughput convention).
    // frame_count == 0 runs until stop_producer(). This is what lets
    // bench_python_throughput.py measure sustained Python-side ingestion
    // without wiring up a second real producer process.
    void start_producer(uint64_t frame_count, double target_frames_per_sec) {
        if (producer_thread_.joinable()) {
            throw std::runtime_error(
                "producer already running -- call stop_producer() first "
                "(SPSC ring: exactly one producer at a time)");
        }
        stop_requested_.store(false, std::memory_order_relaxed);
        pushed_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        producer_thread_ = std::thread([this, frame_count, target_frames_per_sec] {
            run_producer(frame_count, target_frames_per_sec);
            running_.store(false, std::memory_order_release);
        });
    }

    void stop_producer() {
        stop_requested_.store(true, std::memory_order_relaxed);
        if (producer_thread_.joinable()) {
            producer_thread_.join();
        }
    }

    // See bindings/animus_py.cpp's TelemetryStream::producer_running for
    // why this is a real atomic flag the thread sets itself, not
    // producer_thread_.joinable() -- a bounded producer (frame_count != 0)
    // finishes on its own without stop_producer() ever being called.
    bool producer_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    uint64_t pushed_count() const noexcept { return pushed_.load(std::memory_order_relaxed); }
    uint64_t dropped_count() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    size_t capacity() const noexcept { return ring_.capacity(); }
    size_t drain_batch_capacity() const noexcept { return scratch_.size(); }

    // Consumer-side (the calling Python thread) only. Spin-waits (GIL
    // released -- see file header) up to max_spins times per frame for the
    // producer to catch up, then returns a zero-copy view of up to
    // min(max_count, drain_batch_capacity()) frames actually popped.
    //
    // THE RETURNED VIEW ALIASES THIS OBJECT'S SCRATCH BUFFER -- valid only
    // until the next poll()/drain() call on this same object. See
    // animus_sdk/consumer.py's AnimusConsumer.to_numpy()/poll() for the
    // idiomatic way to consume it.
    nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> poll(size_t max_count, uint64_t max_spins) {
        const size_t limit = max_count < scratch_.size() ? max_count : scratch_.size();
        size_t n = 0;
        {
            nb::gil_scoped_release release; // hot spin-wait: no Python API touched in here
            TelemetryFrame frame;
            uint64_t spins = 0;
            while (n < limit) {
                if (ring_.pop(frame)) {
                    scratch_[n++] = frame;
                    spins = 0;
                    continue;
                }
                if (++spins >= max_spins) break;
                animus::cpu_relax();
            }
        }
        return nb::ndarray<uint8_t, nb::memview, nb::ndim<2>>(
            reinterpret_cast<uint8_t*>(scratch_.data()),
            {n, sizeof(TelemetryFrame)},
            nb::find(*this)
        );
    }

    // Consumer-side, non-blocking: pop whatever is immediately available
    // (no spin-wait), up to drain_batch_capacity(). This -- not poll() -- is
    // the hot ingestion-loop primitive bench_python_throughput.py drives in
    // a tight Python while-loop, so a single failed pop attempt costs
    // nothing beyond the one comparison.
    nb::ndarray<uint8_t, nb::memview, nb::ndim<2>> drain(size_t max_count) {
        return poll(max_count, /*max_spins=*/1);
    }

private:
    void run_producer(uint64_t frame_count, double target_frames_per_sec) {
        const bool bounded = frame_count != 0;
        const bool paced = target_frames_per_sec > 0.0;
        const auto interval = paced
            ? std::chrono::duration<double>(1.0 / target_frames_per_sec)
            : std::chrono::duration<double>(0.0);
        auto next_due = std::chrono::steady_clock::now();

        static constexpr const char* kSymbols[] = {"AAPL", "MSFT", "GOOG", "AMZN", "TSLA", "NVDA", "META", "BRKB"};
        constexpr int kNumSymbols = 8;

        uint64_t i = 0;
        uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
        double price = 100.0;
        while (!stop_requested_.load(std::memory_order_relaxed) && (!bounded || i < frame_count)) {
            if (paced) {
                next_due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
                std::this_thread::sleep_until(next_due);
            }
            rng_state ^= rng_state << 13;
            rng_state ^= rng_state >> 7;
            rng_state ^= rng_state << 17;

            TelemetryFrame frame{};
            frame.sequence_id = i;
            const char* sym = kSymbols[rng_state % kNumSymbols];
            std::memcpy(frame.symbol, sym, std::strlen(sym) < sizeof(frame.symbol) ? std::strlen(sym) : sizeof(frame.symbol));
            const double drift = (static_cast<double>((rng_state >> 32) % 2001) - 1000.0) * 0.0001;
            price += drift;
            if (price < 1.0) price = 1.0;
            frame.price = price;
            frame.volume = static_cast<uint32_t>(100 + (rng_state % 9900));
            frame.flags = 0x00;
            frame.timestamp_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

            if (ring_.push(frame)) {
                pushed_.fetch_add(1, std::memory_order_relaxed);
            } else {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            ++i;
        }
    }

    animus::eval::SpscRingBuffer<TelemetryFrame> ring_;
    std::vector<TelemetryFrame> scratch_;
    std::thread producer_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> pushed_{0};
    std::atomic<uint64_t> dropped_{0};
};

} // namespace

NB_MODULE(_animus_sdk_native, m) {
    m.doc() = "Animus Engine -- institutional Python SDK native extension (nanobind), "
              "zero-copy interop over animus::eval::SpscRingBuffer<TelemetryFrame>";
    m.attr("WIRE_FORMAT") = kWireFormat;
    m.attr("WIRE_RECORD_SIZE") = sizeof(TelemetryFrame);
    m.attr("FLAG_BURST") = static_cast<int>(kFlagBurst);

    nb::class_<RingBuffer>(m, "RingBuffer")
        .def(nb::init<size_t, size_t>(), "capacity"_a, "drain_batch_capacity"_a = 8192,
             "capacity: ring size in frames (rounded up to a power of two). "
             "drain_batch_capacity: scratch buffer size in frames -- also the "
             "hard upper bound on a single poll()/drain() call's batch size.")
        .def("push", &RingBuffer::push,
             "sequence_id"_a, "symbol"_a, "price"_a, "volume"_a, "flags"_a = 0,
             "Push one frame from Python. Never blocks; returns False if the ring is full.")
        .def("start_producer", &RingBuffer::start_producer,
             "frame_count"_a = 0, "target_frames_per_sec"_a = 0.0,
             nb::call_guard<nb::gil_scoped_release>(),
             "Spawn a background native thread generating synthetic TelemetryFrame load. "
             "frame_count=0 runs until stop_producer(); target_frames_per_sec<=0 is unthrottled.")
        .def("stop_producer", &RingBuffer::stop_producer,
             nb::call_guard<nb::gil_scoped_release>(),
             "Stop and join the background producer thread, if running.")
        .def("producer_running", &RingBuffer::producer_running)
        .def("poll", &RingBuffer::poll, "max_count"_a, "max_spins"_a = 200000,
             "Consumer-side. Spin-waits (GIL released) up to max_spins times per frame for "
             "the producer to catch up, then returns a zero-copy view of whatever arrived.")
        .def("drain", &RingBuffer::drain, "max_count"_a,
             "Consumer-side, non-blocking: pop only what's immediately available. "
             "The primary ingestion-loop call for the throughput benchmark.")
        .def_prop_ro("capacity", &RingBuffer::capacity)
        .def_prop_ro("drain_batch_capacity", &RingBuffer::drain_batch_capacity)
        .def_prop_ro("pushed_count", &RingBuffer::pushed_count)
        .def_prop_ro("dropped_count", &RingBuffer::dropped_count);
}
