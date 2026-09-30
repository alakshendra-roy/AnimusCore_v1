#pragma once
// Lossless 1-writer -> 1-reader command queue (execution / order path).
//
// Strict backpressure: a full queue refuses the push, it never overwrites.
// Each index has exactly one writer -- `head` only the producer, `tail` only
// the consumer -- each on its own cache line, so the only cross-core traffic
// is the index the other side actually needs. No seqlock, no CAS, no retry
// loop: a record is published by a release store of `head` and retired by a
// release store of `tail`, and the matching acquire loads make every slot
// access ordered, so there is no data race to tolerate.
//
// Operates on caller-supplied memory (a heap block, or a shm mapping such as
// animus::sys::ipc::SharedMemoryRegion), aligned to ANIMUS_CACHE_LINE_SIZE:
//   init() on one side, attach() on the other. No OS calls here.
// Each side uses its OWN view object: a view caches the other side's index
// (process-local, never in shared memory) and only re-reads the shared one
// when the cached value says full/empty, so a steady stream touches the
// peer's cache line once per batch rather than once per record.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#ifndef ANIMUS_CACHE_LINE_SIZE
    #if defined(__aarch64__) || defined(_M_ARM64)
        #define ANIMUS_CACHE_LINE_SIZE 128
    #else
        #define ANIMUS_CACHE_LINE_SIZE 64
    #endif
#endif

namespace animus {
namespace sys {
namespace ipc {

#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable: 4324) // padding from alignas is the point: one owner per cache line
#endif
    struct alignas(ANIMUS_CACHE_LINE_SIZE) SpscQueueHeader {
        // Read-only after init().
        uint64_t magic = 0;
        uint64_t capacity = 0; // power of two; all capacity slots are usable
        uint64_t mask = 0;
        uint64_t payload_size = 0;

        alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> head{ 0 }; // producer-owned: records ever pushed
        alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> tail{ 0 }; // consumer-owned: records ever popped
    };
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
    static_assert(std::is_standard_layout<SpscQueueHeader>::value, "offsetof below requires standard layout");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "lives in shared memory: a non-lock-free atomic could fall back to a mutex that is invalid across processes");
    static_assert(offsetof(SpscQueueHeader, head) == ANIMUS_CACHE_LINE_SIZE &&
                  offsetof(SpscQueueHeader, tail) == 2 * ANIMUS_CACHE_LINE_SIZE,
        "descriptor, head and tail must each own a separate cache line");
    static_assert(sizeof(SpscQueueHeader) == 3 * ANIMUS_CACHE_LINE_SIZE,
        "descriptor + head line + tail line, nothing else: slots must start on a cache line");

    template <typename T>
    class SpscQueue {
    public:
        static_assert(std::is_trivially_copyable<T>::value, "records live in raw shared memory and are copied bytewise");
        static_assert(alignof(T) <= ANIMUS_CACHE_LINE_SIZE, "slots are packed back to back from a cache-line boundary");

        static constexpr uint64_t kMagic = 0x5350534351554555ull; // "SPSCQUEU"

        static constexpr size_t required_bytes(size_t capacity) noexcept {
            return sizeof(SpscQueueHeader) + capacity * sizeof(T);
        }

        // `mem` must be ANIMUS_CACHE_LINE_SIZE-aligned and at least
        // required_bytes(capacity) long; capacity a power of two >= 2. Call
        // once, before the peer attaches.
        static bool init(void* mem, size_t bytes, size_t capacity, SpscQueue& out) noexcept {
            if (!mem || !is_pow2(capacity) || !aligned(mem) || bytes < required_bytes(capacity)) return false;
            auto* h = new (mem) SpscQueueHeader();
            h->capacity = capacity;
            h->mask = capacity - 1;
            h->payload_size = sizeof(T);
            h->magic = kMagic;
            out = SpscQueue(h);
            return true;
        }

        // Validates the header before trusting any field of it.
        static bool attach(void* mem, size_t bytes, SpscQueue& out) noexcept {
            if (!mem || !aligned(mem) || bytes < sizeof(SpscQueueHeader)) return false;
            auto* h = static_cast<SpscQueueHeader*>(mem);
            if (h->magic != kMagic || h->payload_size != sizeof(T) || !is_pow2(h->capacity) ||
                h->mask != h->capacity - 1 || bytes < required_bytes(h->capacity)) return false;
            out = SpscQueue(h);
            return true;
        }

        SpscQueue() noexcept = default;
        bool valid() const noexcept { return h_ != nullptr; }
        size_t capacity() const noexcept { return h_ ? static_cast<size_t>(h_->capacity) : 0; }

        // Producer view only. Returns false at once when full: backpressure, never an overwrite.
        bool try_push(const T& value) noexcept {
            const uint64_t head = h_->head.load(std::memory_order_relaxed); // we are its only writer
            if (head - cached_tail_ >= h_->capacity) {
                cached_tail_ = h_->tail.load(std::memory_order_acquire);    // pairs with the consumer's release: its reads of the slot are done
                if (head - cached_tail_ >= h_->capacity) return false;
            }
            slots_[head & h_->mask] = value;
            h_->head.store(head + 1, std::memory_order_release);            // publishes the slot
            return true;
        }

        // Consumer view only. Returns false at once when empty.
        bool try_pop(T& out) noexcept {
            const uint64_t tail = h_->tail.load(std::memory_order_relaxed); // we are its only writer
            if (tail == cached_head_) {
                cached_head_ = h_->head.load(std::memory_order_acquire);    // pairs with the producer's release: the slot is written
                if (tail == cached_head_) return false;
            }
            out = slots_[tail & h_->mask];
            h_->tail.store(tail + 1, std::memory_order_release);            // retires the slot for reuse
            return true;
        }

    private:
        explicit SpscQueue(SpscQueueHeader* h) noexcept
            : h_(h), slots_(reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(h) + sizeof(SpscQueueHeader))) {}

        static constexpr bool is_pow2(uint64_t v) noexcept { return v >= 2 && (v & (v - 1)) == 0; }
        static bool aligned(const void* p) noexcept { return reinterpret_cast<uintptr_t>(p) % ANIMUS_CACHE_LINE_SIZE == 0; }

        SpscQueueHeader* h_ = nullptr;
        T* slots_ = nullptr;
        uint64_t cached_tail_ = 0; // producer view: last tail observed (<= the real tail)
        uint64_t cached_head_ = 0; // consumer view: last head observed (<= the real head)
    };

} // namespace ipc
} // namespace sys
} // namespace animus
