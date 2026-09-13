// animus_sandbox/nanobind_bridge.cpp
//
// Minimal nanobind module demonstrating zero-copy, zero-per-event-heap-
// allocation consumption of this sandbox's own MPMC ring buffer from
// Python: a background native producer thread feeds the ring
// continuously; TelemetryStream::drain() pops up to `batch` events
// directly into a scratch buffer this object allocates exactly once, at
// construction, and reuses across every call -- never in the hot path --
// then returns an nb::ndarray view over that same memory. No per-event
// Python object is constructed on this path, and drain() itself copies
// nothing beyond the plain-old-data pops the ring buffer already does:
// the returned array aliases the C++ scratch buffer directly, valid
// until the next drain() call overwrites it.
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "ring_buffer.hpp"
#include "sandbox_event.hpp"
#include "tsc_clock.hpp"

namespace nb = nanobind;

namespace {

class TelemetryStream {
public:
    TelemetryStream(std::size_t capacity, std::size_t drain_batch_capacity)
        : queue_(capacity),
          scratch_(drain_batch_capacity),
          drain_batch_capacity_(drain_batch_capacity) {}

    ~TelemetryStream() { stop_producer(); }

    // Starts a background native thread pushing `event_count` synthetic
    // events as fast as the ring allows (unthrottled), so drain() below
    // can measure real concurrent producer/consumer throughput rather
    // than draining a pre-filled snapshot.
    void start_producer(std::uint64_t event_count) {
        stop_producer();
        producer_running_.store(true, std::memory_order_release);
        producer_ = std::thread([this, event_count] {
            for (std::uint64_t i = 0; i < event_count; ++i) {
                sandbox::Event ev{};
                ev.sequence = i;
                ev.dispatch_tsc = sandbox::read_tsc();
                ev.producer_id = 0;
                ev.value = i;
                while (!queue_.enqueue(ev)) {
                    std::this_thread::yield(); // ring momentarily full -- retry
                }
            }
            producer_running_.store(false, std::memory_order_release);
        });
    }

    void stop_producer() {
        if (producer_.joinable()) {
            producer_.join();
        }
    }

    bool producer_running() const { return producer_running_.load(std::memory_order_acquire); }

    // Zero-copy drain: pops up to `batch` events into the pre-allocated
    // scratch buffer and returns a byte view over the popped prefix. No
    // allocation happens in this function -- `scratch_` was sized once,
    // at construction, and never resized.
    nb::ndarray<nb::numpy, uint8_t, nb::ndim<1>> drain(std::size_t batch) {
        const std::size_t n = std::min(batch, drain_batch_capacity_);
        std::size_t popped = 0;
        while (popped < n && queue_.dequeue(scratch_[popped])) {
            ++popped;
        }
        const size_t shape[1] = {popped * sizeof(sandbox::Event)};
        return nb::ndarray<nb::numpy, uint8_t, nb::ndim<1>>(
            reinterpret_cast<uint8_t*>(scratch_.data()), 1, shape, nb::handle());
    }

private:
    sandbox::MpmcBoundedQueue<sandbox::Event> queue_;
    std::vector<sandbox::Event> scratch_;
    std::size_t drain_batch_capacity_;
    std::thread producer_;
    std::atomic<bool> producer_running_{false};
};

} // namespace

NB_MODULE(animus_sandbox_bridge, m) {
    m.doc() = "Animus Core sandbox: zero-copy MPMC ring buffer bridge for institutional evaluation.";
    m.attr("EVENT_SIZE_BYTES") = static_cast<int>(sizeof(sandbox::Event));

    nb::class_<TelemetryStream>(m, "TelemetryStream")
        .def(nb::init<std::size_t, std::size_t>(), nb::arg("capacity"), nb::arg("drain_batch_capacity"))
        .def("start_producer", &TelemetryStream::start_producer, nb::arg("event_count"))
        .def("stop_producer", &TelemetryStream::stop_producer)
        .def("drain", &TelemetryStream::drain, nb::arg("batch"))
        .def_prop_ro("producer_running", &TelemetryStream::producer_running);
}
