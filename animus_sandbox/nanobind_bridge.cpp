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
          drain_batch_capacity_(drain_batch_capacity),
          cycles_per_ns_(sandbox::calibrate_cycles_per_ns()) {}

    ~TelemetryStream() { stop_producer(); stop_continuous(); }

    // Starts a background native thread pushing `event_count` synthetic
    // events as fast as the ring allows (unthrottled), so drain() below
    // can measure real concurrent producer/consumer throughput rather
    // than draining a pre-filled snapshot. Never drops: retries until the
    // ring has room, matching the "no telemetry loss" contract used by
    // benchmark_harness.cpp / soak_harness.cpp.
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
                produced_total_.fetch_add(1, std::memory_order_relaxed);
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

    // Runs until stop_continuous() is called, for a live dashboard's
    // indefinite feed. Unlike start_producer() above, each event gets a
    // bounded number of enqueue attempts (spin + yield) before it is
    // counted as dropped and skipped -- a best-effort, non-blocking mode
    // so a consumer that falls behind produces a real, non-zero drop
    // counter instead of only ever back-pressuring the producer.
    void start_continuous(std::uint32_t max_retry_spins) {
        stop_continuous();
        continuous_running_.store(true, std::memory_order_release);
        continuous_ = std::thread([this, max_retry_spins] {
            std::uint64_t seq = 0;
            while (continuous_running_.load(std::memory_order_acquire)) {
                sandbox::Event ev{};
                ev.sequence = seq;
                ev.dispatch_tsc = sandbox::read_tsc();
                ev.producer_id = 1;
                ev.value = seq;
                ++seq;

                bool delivered = false;
                for (std::uint32_t attempt = 0; attempt <= max_retry_spins; ++attempt) {
                    if (queue_.enqueue(ev)) {
                        delivered = true;
                        break;
                    }
                    std::this_thread::yield();
                }
                if (delivered) {
                    produced_total_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    dropped_total_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    void stop_continuous() {
        continuous_running_.store(false, std::memory_order_release);
        if (continuous_.joinable()) {
            continuous_.join();
        }
    }

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
        if (popped > 0) {
            consumed_total_.fetch_add(popped, std::memory_order_relaxed);
        }
        const size_t shape[1] = {popped * sizeof(sandbox::Event)};
        return nb::ndarray<nb::numpy, uint8_t, nb::ndim<1>>(
            reinterpret_cast<uint8_t*>(scratch_.data()), 1, shape, nb::handle());
    }

    std::size_t capacity() const { return queue_.capacity(); }
    std::uint64_t produced_total() const { return produced_total_.load(std::memory_order_relaxed); }
    std::uint64_t consumed_total() const { return consumed_total_.load(std::memory_order_relaxed); }
    std::uint64_t dropped_total() const { return dropped_total_.load(std::memory_order_relaxed); }

    // occupancy is an approximation (produced/consumed counters are
    // updated independently, without a shared snapshot lock) -- adequate
    // for a display metric sampled a few times a second, not for anything
    // that needs an exact instantaneous queue depth.
    std::int64_t occupancy_estimate() const {
        return static_cast<std::int64_t>(produced_total()) -
               static_cast<std::int64_t>(consumed_total());
    }

    double cycles_per_ns() const { return cycles_per_ns_; }
    std::uint64_t tsc_now() const { return sandbox::read_tsc(); }

private:
    sandbox::MpmcBoundedQueue<sandbox::Event> queue_;
    std::vector<sandbox::Event> scratch_;
    std::size_t drain_batch_capacity_;
    std::thread producer_;
    std::atomic<bool> producer_running_{false};
    std::thread continuous_;
    std::atomic<bool> continuous_running_{false};
    std::atomic<std::uint64_t> produced_total_{0};
    std::atomic<std::uint64_t> consumed_total_{0};
    std::atomic<std::uint64_t> dropped_total_{0};
    double cycles_per_ns_;
};

} // namespace

NB_MODULE(animus_sandbox_bridge, m) {
    m.doc() = "Animus Core sandbox: zero-copy MPMC ring buffer bridge for institutional evaluation.";
    m.attr("EVENT_SIZE_BYTES") = static_cast<int>(sizeof(sandbox::Event));

    nb::class_<TelemetryStream>(m, "TelemetryStream")
        .def(nb::init<std::size_t, std::size_t>(), nb::arg("capacity"), nb::arg("drain_batch_capacity"))
        .def("start_producer", &TelemetryStream::start_producer, nb::arg("event_count"))
        .def("stop_producer", &TelemetryStream::stop_producer)
        .def("start_continuous", &TelemetryStream::start_continuous, nb::arg("max_retry_spins") = 64)
        .def("stop_continuous", &TelemetryStream::stop_continuous)
        // reference_internal: without an explicit return-value policy,
        // nanobind's ndarray export copies the buffer whenever it sees no
        // owner and no bound `self` (nb_ndarray.cpp: `copy = th->owner ==
        // nullptr && th->self == nullptr`) -- exactly what was happening
        // here, silently, on every call. reference_internal makes nanobind
        // attach this TelemetryStream instance itself as the ndarray's
        // owner, which (a) satisfies that check so the view is returned
        // as-is instead of copied, and (b) keeps the instance -- and so
        // scratch_ -- alive for as long as the returned array is.
        .def("drain", &TelemetryStream::drain, nb::arg("batch"), nb::rv_policy::reference_internal)
        .def_prop_ro("producer_running", &TelemetryStream::producer_running)
        .def_prop_ro("capacity", &TelemetryStream::capacity)
        .def_prop_ro("produced_total", &TelemetryStream::produced_total)
        .def_prop_ro("consumed_total", &TelemetryStream::consumed_total)
        .def_prop_ro("dropped_total", &TelemetryStream::dropped_total)
        .def_prop_ro("occupancy_estimate", &TelemetryStream::occupancy_estimate)
        .def_prop_ro("cycles_per_ns", &TelemetryStream::cycles_per_ns)
        .def("tsc_now", &TelemetryStream::tsc_now);
}
