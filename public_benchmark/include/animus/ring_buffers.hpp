#pragma once
// animus/ring_buffers.hpp -- the AnimusCore ring-buffer types this suite exercises.
//
// SPDX-License-Identifier: Apache-2.0 (see LICENSE). The rest of AnimusCore is not part of this
// repository and is licensed separately (see animusinfra.com).
//
// TelemetryPayload (the 64-byte telemetry slot), kCachelineBytes, LockFreeRingBuffer<T> (bounded MPMC,
// Vyukov) and SpscRingBuffer<T> (bounded SPSC), extracted VERBATIM from AnimusCore's single-header engine,
// animus.hpp. That header carries much more, none of which is needed to build, run or verify anything
// here, so none of it is shipped.
//
// Provenance. Source file animus.hpp, SHA-256 1dde92a2abd26e1163986df0a19c256713b12d4e26e9e6c2879ce96803e2e34d
// (of the file with LF line endings, as git stores it: a CRLF checkout hashes differently).
// Included below, byte for byte:
//     lines  58-75   TelemetryPayload and its static_asserts
//     lines 334-534  kCachelineBytes, LockFreeRingBuffer, SpscRingBuffer
// so the suite exercises the code the engine ships, not a lookalike. Comments are verbatim too, so a few
// of them refer to parts of animus.hpp that are not included here. To refresh, re-extract; do not edit.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // padding from alignas(64) is the point: one owner per cache line
#endif

namespace animus {

    // Strictly aligned to 64 bytes to match standard CPU L1/L2 cache line boundaries.
    // Real fields occupy exactly the first 24 bytes with zero internal padding
    // (timestamp_cycles @0-7, event_id @8-11, trace_id @12-15, metric_value
    // @16-23 -- each already lands on its own natural-alignment boundary in
    // declaration order, so the compiler inserts no gaps between them); the
    // alignas(64) requirement is what pads sizeof(...) up to a full 64-byte
    // cache line, not scattered internal padding. The static_asserts below
    // pin both halves of that guarantee at compile time -- if a future field
    // addition ever breaks single-cacheline transfer, the build fails here
    // instead of silently regressing a hot-path property nothing else checks.
    struct alignas(64) TelemetryPayload {
        uint64_t timestamp_cycles;
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
    };
    static_assert(sizeof(TelemetryPayload) == 64, "TelemetryPayload must occupy exactly one 64-byte cache line");
    static_assert(alignof(TelemetryPayload) == 64, "TelemetryPayload must be 64-byte aligned for single-cacheline transfer");

    // Cache-line size used to keep a producer's and a consumer's index
    // atomics (LockFreeRingBuffer's enqueue_pos_/dequeue_pos_, SpscRingBuffer's
    // head_/tail_) on separate lines and avoid cross-core false sharing.
    // Prefer the standard library's own platform value when the toolchain
    // defines the __cpp_lib_hardware_interference_size feature-test macro
    // (std::hardware_destructive_interference_size itself has been in <new>
    // since C++17); whether that macro is actually defined varies by
    // standard-library version and target -- some libstdc++ releases
    // withhold it entirely over an ABI-stability concern (the "right" value
    // can differ across translation units built with different -march
    // flags, which would be a silent ODR violation if two TUs disagreed on
    // it), while others define it and instead warn at each use site
    // (GCC's -Winterference-size, suppressed below with the same
    // documented-and-scoped approach this file already uses for MSVC's
    // C4324 at the top) -- so gate on the feature-test macro, not on
    // __has_include<new>, and fall back to the conservative,
    // universally-safe 64-byte assumption everywhere the macro is absent.
#if defined(__cpp_lib_hardware_interference_size)
    // -Winterference-size exists only in GCC. Clang defines __GNUC__ too, but
    // rejects the unknown group (-Wunknown-warning-option), which -Werror turns
    // into a build failure -- hence the explicit !__clang__.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
    inline constexpr size_t kCachelineBytes = std::hardware_destructive_interference_size;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
    inline constexpr size_t kCachelineBytes = 64;
#endif

    // Bounded lock-free MPMC ring buffer (Vyukov algorithm). Backing storage is
    // allocated once at construction; push()/pop() perform zero heap allocations
    // and never block, making them safe to call from multiple concurrent
    // producer threads (e.g. concurrent ctypes callers) and a single consumer.
    template <typename T>
    class LockFreeRingBuffer {
    public:
        explicit LockFreeRingBuffer(size_t requested_capacity)
            : capacity_(round_up_pow2(requested_capacity)),
            mask_(capacity_ - 1),
            cells_(capacity_) {
            for (size_t i = 0; i < capacity_; ++i) {
                cells_[i].sequence.store(i, std::memory_order_relaxed);
            }
        }

        LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
        LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

        bool push(const T& value) noexcept {
            Cell* cell;
            size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
            for (;;) {
                cell = &cells_[pos & mask_];
                size_t seq = cell->sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
                if (diff == 0) {
                    if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        break;
                    }
                }
                else if (diff < 0) {
                    return false; // buffer full
                }
                else {
                    pos = enqueue_pos_.load(std::memory_order_relaxed);
                }
            }
            cell->data = value;
            cell->sequence.store(pos + 1, std::memory_order_release);
            return true;
        }

        bool pop(T& out) noexcept {
            Cell* cell;
            size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
            for (;;) {
                cell = &cells_[pos & mask_];
                size_t seq = cell->sequence.load(std::memory_order_acquire);
                intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
                if (diff == 0) {
                    if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                        break;
                    }
                }
                else if (diff < 0) {
                    return false; // buffer empty
                }
                else {
                    pos = dequeue_pos_.load(std::memory_order_relaxed);
                }
            }
            out = cell->data;
            cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
            return true;
        }

        size_t capacity() const noexcept { return capacity_; }

    private:
        struct Cell {
            std::atomic<size_t> sequence;
            T data;
        };

        static size_t round_up_pow2(size_t v) noexcept {
            size_t p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        size_t capacity_;
        size_t mask_;
        std::vector<Cell> cells_;

        alignas(kCachelineBytes) std::atomic<size_t> enqueue_pos_{ 0 };
        alignas(kCachelineBytes) std::atomic<size_t> dequeue_pos_{ 0 };
    };

    // Bounded lock-free single-producer/single-consumer ring buffer.
    // Unlike LockFreeRingBuffer above (a general Vyukov MPMC ring, safe for
    // multiple concurrent producer threads -- e.g. concurrent ctypes callers
    // sharing one Engine), this SPSC variant assumes exactly one producer
    // thread and exactly one consumer thread and nothing else, ever. That
    // narrower contract buys real throughput/latency: push()/pop() need only
    // a plain atomic load/store pair (a relaxed load of the caller's own
    // last-written index, an acquire load of the other side's index, a
    // release store to publish) -- no compare-exchange retry loop, so no
    // contention between producer-side CAS attempts under concurrent load
    // the way LockFreeRingBuffer can see with >1 producer. Backing storage
    // is one contiguous, pre-allocated std::vector<T> -- a single memory
    // pool, not per-cell padded like LockFreeRingBuffer's Cell, since SPSC
    // needs no per-cell sequence number (there is only ever one writer and
    // one reader for any given index, so nothing to arbitrate) -- sized to
    // a power of two so index-to-slot wraparound is a single AND, not a
    // modulo.
    //
    // This is the classic Le/Vyukov-style SPSC design. Its correctness
    // depends entirely on the single-producer/single-consumer contract
    // being honored by the caller -- it is NOT enforced at runtime (a
    // debug-only thread-id check would cost real overhead on the exact hot
    // path this class exists to make fast). The C-ABI surface built on top
    // of this (animus_spsc_*, see the bottom of this file) inherits the
    // same contract and documents it the same way: call
    // animus_spsc_record_events_batch from more than one thread
    // concurrently and the behavior is undefined, not merely wrong.
    template <typename T>
    class SpscRingBuffer {
    public:
        explicit SpscRingBuffer(size_t requested_capacity)
            : capacity_(round_up_pow2(requested_capacity)),
            mask_(capacity_ - 1),
            cells_(capacity_) {
        }

        SpscRingBuffer(const SpscRingBuffer&) = delete;
        SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

        // Producer-thread-only. Never blocks; returns false if the ring is full.
        bool push(const T& value) noexcept {
            const size_t head = head_.load(std::memory_order_relaxed);
            const size_t tail = tail_.load(std::memory_order_acquire);
            if (head - tail >= capacity_) {
                return false; // full
            }
            cells_[head & mask_] = value;
            head_.store(head + 1, std::memory_order_release);
            return true;
        }

        // Consumer-thread-only. Never blocks; returns false if the ring is empty.
        bool pop(T& out) noexcept {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t head = head_.load(std::memory_order_acquire);
            if (tail == head) {
                return false; // empty
            }
            out = cells_[tail & mask_];
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        size_t capacity() const noexcept { return capacity_; }

    private:
        static size_t round_up_pow2(size_t v) noexcept {
            size_t p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        size_t capacity_;
        size_t mask_;
        std::vector<T> cells_;

        alignas(kCachelineBytes) std::atomic<size_t> head_{ 0 }; // written only by the producer
        alignas(kCachelineBytes) std::atomic<size_t> tail_{ 0 }; // written only by the consumer
    };

} // namespace animus

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
