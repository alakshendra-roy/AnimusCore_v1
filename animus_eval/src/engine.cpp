// Animus Evaluation Kit -- AnimusEngine implementation.
//
// Everything internal to the engine lives in this translation unit, behind
// the Impl type forward-declared in include/animus/engine.hpp: the ring
// buffer's cursor/storage layout, the SPSC push/pop algorithm, and the
// receive-side timestamping on ingest_packet(). None of it is visible from
// the public header, so a client compiled against engine.hpp never depends
// on this layout.
//
// Ring algorithm: the same single-producer/single-consumer design used
// throughout Animus's hot path -- a power-of-two-sized slot array indexed
// by an AND mask (no modulo), with head_/tail_ cursors each pinned to their
// own cache line via alignas(kCacheLineSize). Without that isolation, the
// producer publishing a new head_ value would invalidate the same line the
// consumer is spin-polling tail_ from (false sharing), forcing a
// cache-coherency round trip on every operation instead of only when a
// cursor genuinely changes ownership.
//
// push()/pop() take only a relaxed load of the caller's own last-published
// cursor, an acquire load of the other side's cursor, and a release store
// to publish -- no compare-exchange retry loop, no lock. This is what makes
// the SPSC contract (documented on AnimusEngine in the public header) worth
// the narrower guarantee than a general MPMC queue.

#include "animus/engine.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

#if defined(__linux__)
    #include <time.h>
#endif

namespace animus::eval {

namespace {

// Receive-side timestamp, nanoseconds, from a monotonic clock immune to
// wall-clock adjustments. CLOCK_MONOTONIC_RAW specifically (not
// CLOCK_MONOTONIC) on Linux: it is not subject to NTP slewing, which
// matters when the value being stamped is a latency measurement input, not
// a wall-clock display value. std::chrono::steady_clock is the portable
// fallback for non-Linux development builds (see README.md's supported-
// platform note -- Linux is the only measurement target).
inline uint64_t now_ns() noexcept {
#if defined(__linux__)
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

uint64_t round_up_pow2(size_t v) noexcept {
    uint64_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

} // namespace

struct AnimusEngine::Impl {
    explicit Impl(size_t requested_capacity)
        : capacity(round_up_pow2(requested_capacity < 2 ? 2 : requested_capacity)),
          mask(capacity - 1),
          slots(capacity) {
    }

    bool push(const MarketDataEntry& value) noexcept {
        const uint64_t head = head_cursor.load(std::memory_order_relaxed);
        const uint64_t tail = tail_cursor.load(std::memory_order_acquire);
        if (head - tail >= capacity) {
            return false; // full
        }
        slots[head & mask] = value;
        head_cursor.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(MarketDataEntry& out) noexcept {
        const uint64_t tail = tail_cursor.load(std::memory_order_relaxed);
        const uint64_t head = head_cursor.load(std::memory_order_acquire);
        if (tail == head) {
            return false; // empty
        }
        out = slots[tail & mask];
        tail_cursor.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t occupancy_approx() const noexcept {
        const uint64_t head = head_cursor.load(std::memory_order_acquire);
        const uint64_t tail = tail_cursor.load(std::memory_order_acquire);
        return static_cast<size_t>(head - tail);
    }

    const uint64_t capacity;
    const uint64_t mask;
    std::vector<MarketDataEntry> slots;

    alignas(kCacheLineSize) std::atomic<uint64_t> head_cursor{0}; // producer-owned
    alignas(kCacheLineSize) std::atomic<uint64_t> tail_cursor{0}; // consumer-owned

    std::atomic<uint64_t> packets_ingested{0};
    std::atomic<uint64_t> packets_dropped{0};
    std::atomic<uint64_t> packets_malformed{0};

    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "head_cursor/tail_cursor must be lock-free -- a non-lock-free std::atomic<uint64_t> "
        "could fall back to a mutex/futex, defeating the point of a lock-free ring");
};

AnimusEngine::AnimusEngine() noexcept = default;
AnimusEngine::~AnimusEngine() = default;
AnimusEngine::AnimusEngine(AnimusEngine&&) noexcept = default;
AnimusEngine& AnimusEngine::operator=(AnimusEngine&&) noexcept = default;

bool AnimusEngine::initialize(uint32_t cpu_core, size_t ring_capacity) noexcept {
    if (ring_capacity == 0) {
        return false;
    }
    if (!affinity::pin_current_thread_to_core(cpu_core)) {
        return false;
    }
    impl_ = std::make_unique<Impl>(ring_capacity);
    return true;
}

bool AnimusEngine::ingest_packet(const uint8_t* buffer, size_t len) noexcept {
    if (!impl_) {
        return false;
    }
    if (buffer == nullptr || len < sizeof(WireTick)) {
        impl_->packets_malformed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    WireTick tick;
    std::memcpy(&tick, buffer, sizeof(WireTick));

    const MarketDataEntry entry{
        tick.send_timestamp_ns,
        now_ns(), // recv_timestamp_ns, stamped here -- this is the "tick-to-ring" boundary replay_bench.cpp measures
        tick.sequence,
        tick.price_fixed,
        tick.symbol_id,
        tick.quantity,
        tick.side,
        tick.msg_type,
        {}, // reserved
    };

    if (!impl_->push(entry)) {
        impl_->packets_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    impl_->packets_ingested.fetch_add(1, std::memory_order_relaxed);
    return true;
}

size_t AnimusEngine::poll(ConsumerCallback cb, void* user_data, size_t max_events) noexcept {
    if (!impl_ || cb == nullptr) {
        return 0;
    }
    size_t delivered = 0;
    MarketDataEntry entry;
    while (delivered < max_events && impl_->pop(entry)) {
        cb(entry, user_data);
        ++delivered;
    }
    return delivered;
}

TelemetryMetrics AnimusEngine::get_telemetry_metrics() const noexcept {
    if (!impl_) {
        return TelemetryMetrics{0, 0, 0, 0, 0};
    }
    return TelemetryMetrics{
        impl_->packets_ingested.load(std::memory_order_relaxed),
        impl_->packets_dropped.load(std::memory_order_relaxed),
        impl_->packets_malformed.load(std::memory_order_relaxed),
        impl_->capacity,
        impl_->occupancy_approx(),
    };
}

} // namespace animus::eval
