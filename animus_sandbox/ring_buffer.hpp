#pragma once
// Bounded, lock-free, multi-producer/multi-consumer (MPMC) ring buffer.
//
// Dmitry Vyukov's classic bounded-MPMC design: every slot carries its own
// sequence number, so a producer/consumer coordinates with the *slot* it
// is about to touch rather than with one shared "is the queue full/empty"
// flag. That is what lets independent producers (and independent
// consumers) make progress concurrently via a single CAS each on their
// own cursor, with no lock and no blocking on unrelated slots.
//
// enqueue_pos_/dequeue_pos_ are each pinned to their own cache line
// (alignas(kCacheLineSize)) so a producer publishing enqueue_pos_ never
// invalidates the line a consumer is polling dequeue_pos_ from, and vice
// versa -- the same false-sharing elimination this sandbox's own
// benchmark_harness.cpp measures directly in its A/B test.
//
// Note this queue's per-slot Cell (an atomic<size_t> sequence plus one T)
// is deliberately NOT sized to one cache line itself -- that is a
// different, orthogonal property from the payload type's own alignment
// (see sandbox_event.hpp's Event, which IS exactly one cache line). This
// queue's throughput comes from the sequence-number coordination scheme
// above, not from slot-to-slot cache isolation.
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sandbox {

inline constexpr std::size_t kCacheLineSize = 64;

template <typename T>
class MpmcBoundedQueue {
public:
    explicit MpmcBoundedQueue(std::size_t capacity)
        : mask_(capacity - 1), buffer_(capacity) {
        assert(capacity >= 2 && (capacity & (capacity - 1)) == 0 &&
               "capacity must be a power of two");
        for (std::size_t i = 0; i < capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    MpmcBoundedQueue(const MpmcBoundedQueue&) = delete;
    MpmcBoundedQueue& operator=(const MpmcBoundedQueue&) = delete;

    // Multi-producer safe. Returns false if the queue is full.
    bool enqueue(const T& item) noexcept {
        Cell* cell;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Multi-consumer safe. Returns false if the queue is empty.
    bool dequeue(T& out) noexcept {
        Cell* cell;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = cell->data;
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Cell {
        std::atomic<std::size_t> sequence;
        T data;
    };

    const std::size_t mask_;
    std::vector<Cell> buffer_;

    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_;
    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_;

    static_assert(std::atomic<std::size_t>::is_always_lock_free,
        "enqueue_pos_/dequeue_pos_ must be lock-free, or a mutex fallback "
        "would defeat the point of a lock-free queue");
};

} // namespace sandbox
