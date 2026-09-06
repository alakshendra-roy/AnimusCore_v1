#pragma once
// Animus Evaluation Kit -- Single-Producer/Single-Consumer Ring Buffer
//
// Self-contained on purpose: this header has no dependency on the rest of
// the Animus source tree (no animus.hpp, no build-system include paths) so
// the evaluation kit compiles standalone for benchmarking and independent
// review. It mirrors, algorithm-for-algorithm, the production
// animus::SpscRingBuffer used on the Animus Engine hot path.
//
// Concurrency contract: exactly one producer thread calling push(), and
// exactly one consumer thread calling pop(), concurrently, for the lifetime
// of a given SpscRingBuffer instance. That narrower contract (vs. a general
// MPMC queue) is what buys the latency here -- push()/pop() need only a
// relaxed load of the caller's own last-published cursor, an acquire load
// of the other side's cursor, and a release store to publish. No
// compare-exchange retry loop, no inter-producer contention, no lock.
//
// This is NOT enforced at runtime: a debug-only thread-identity check would
// cost real cycles on the exact path this class exists to make fast. Call
// push() from two threads at once (or pop() from two threads at once) and
// the behavior is undefined.
//
// Cache-line isolation: head_ and tail_ are each pinned to their own
// 64-byte line via alignas(kCacheLineSize). Without that, a producer
// publishing a new head_ value would invalidate the same cache line the
// consumer is spin-polling tail_ from (false sharing), forcing a
// cache-coherency round trip on every single operation instead of only when
// a cursor genuinely changes ownership. Isolating them means the producer's
// writes to head_ and the consumer's writes to tail_ each live on a line
// touched by only one core's write traffic.
//
// T must be trivially copyable -- slots are plain assignment-copied, not
// serialized.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace animus::eval {

// 64 bytes covers essentially all x86_64 and most 32/64-bit ARM cores.
// Apple Silicon and some server-class ARM (Neoverse) parts use a 128-byte
// line -- widen this if specifically targeting one of those, since a
// 64-byte pad on a 128-byte-line part still leaves head_/tail_ sharing a
// line.
inline constexpr std::size_t kCacheLineSize = 64;

template <typename T>
class SpscRingBuffer {
public:
    static_assert(std::is_trivially_copyable_v<T>,
        "SpscRingBuffer<T> stores T by plain assignment, not serialization -- "
        "T must be trivially copyable");

    // Allocates backing storage once, sized to the next power of two at or
    // above `requested_capacity` (minimum 2), so slot indexing is a plain
    // AND mask, not a modulo. No further allocation occurs on push()/pop().
    explicit SpscRingBuffer(std::size_t requested_capacity)
        : capacity_(round_up_pow2(requested_capacity < 2 ? 2 : requested_capacity)),
          mask_(capacity_ - 1),
          slots_(capacity_) {
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Producer-thread-only. Never blocks; returns false if the ring is full.
    bool push(const T& value) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            return false; // full
        }
        slots_[head & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer-thread-only. Never blocks; returns false if the ring is empty.
    bool pop(T& out) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false; // empty
        }
        out = slots_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    // Approximate occupancy -- "approximate" because, read from neither the
    // producer nor the consumer thread, the two loads below are not a
    // single atomic snapshot. Diagnostic use only; never gate push()/pop()
    // correctness on this value.
    std::size_t size_approx() const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(head - tail);
    }

private:
    static uint64_t round_up_pow2(std::size_t v) noexcept {
        uint64_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    const uint64_t capacity_;
    const uint64_t mask_;
    std::vector<T> slots_;

    alignas(kCacheLineSize) std::atomic<uint64_t> head_{ 0 }; // producer-owned cursor
    alignas(kCacheLineSize) std::atomic<uint64_t> tail_{ 0 }; // consumer-owned cursor

    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "head_/tail_ must be lock-free -- a non-lock-free std::atomic<uint64_t> could "
        "fall back to a mutex/futex, defeating the point of a lock-free ring");
};

} // namespace animus::eval
