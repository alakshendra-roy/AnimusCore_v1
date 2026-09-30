#pragma once
// Lossy 1-writer -> N-reader broadcast ring (telemetry, market ticks).
//
// Readers NEVER write shared memory: no tail, no CAS, no reader count. The
// writer's cache line is therefore never invalidated by a reader, and any
// number of independent readers (threads or processes) can attach. Each
// reader keeps its own cursor in its own process memory; a reader that falls
// too far behind detects it deterministically, counts the loss, and snaps
// forward. The writer never waits for, or even knows about, its readers.
//
// Operates on caller-supplied memory (a heap block, or a shm mapping such as
// animus::sys::ipc::SharedMemoryRegion), aligned to ANIMUS_CACHE_LINE_SIZE:
//   init() on the writer's side, attach() on each reader's. No OS calls here.
//
// Protocol (one atomic, no sequence counter):
//   writer  publish(): fence(release); copy into slots[w & mask]; cursor.store(w + 1, release)
//   reader  poll():    c = cursor.load(acquire); copy slots; fence(acquire);
//                      c2 = cursor.load(relaxed); record i is intact iff c2 - i < capacity
// Why the window is capacity - 1, not capacity: write w recycles the slot of
// record w - capacity, and it starts while cursor == w (it is published only
// afterwards). So when cursor - i == capacity, record i's slot may be under
// the writer's hands right now, and nothing visible says so. Requiring
// c2 - i < capacity proves write i + capacity had not started. Consequently
// the newest capacity - 1 records are readable, and a reader more than that
// far behind is snapped to cursor - (capacity - 1).
// The release fence before the slot copy is what makes that proof hold on
// weakly-ordered CPUs: a release *store* of the previous cursor value does
// not stop the next write's slot stores from becoming visible ahead of it.
// It costs nothing on x86 (compiler barrier only).
//
// Known, deliberate formal data race: slots are copied with plain memcpy. A
// reader that has fallen behind can be mid-copy of record i while the writer
// writes record i + capacity -- the window above lets the reader DETECT that
// afterwards and discard the copy, it does not prevent the overlap. Discarded
// reads of a racing location are undefined behaviour in the C++ memory model
// (and ThreadSanitizer will report them), though the fences above make it
// correct on every real compiler/CPU this targets -- the same trade every
// production seqlock makes. The alternative, relaxed-atomic word copies, is
// race-free by the standard but slower on some CPUs (scalar loads vs vector).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

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
    #pragma warning(disable: 4324) // padding from alignas is the point: one writer-owned line
#endif
    struct alignas(ANIMUS_CACHE_LINE_SIZE) BroadcastHeader {
        // Read-only after init(): its own line, shared by every reader.
        uint64_t magic = 0;
        uint64_t capacity = 0; // power of two; the newest capacity - 1 records are readable
        uint64_t mask = 0;
        uint64_t payload_size = 0;

        // Written only by the writer. Nothing else shares this line.
        alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> cursor{ 0 }; // records ever published
    };
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
    static_assert(std::is_standard_layout<BroadcastHeader>::value, "offsetof below requires standard layout");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "lives in shared memory: a non-lock-free atomic could fall back to a mutex that is invalid across processes");
    static_assert(offsetof(BroadcastHeader, cursor) == ANIMUS_CACHE_LINE_SIZE,
        "cursor must start the writer's own cache line, apart from the read-only descriptor");
    static_assert(sizeof(BroadcastHeader) == 2 * ANIMUS_CACHE_LINE_SIZE,
        "descriptor line + cursor line, nothing else: slots must start on a cache line");

    template <typename T>
    class BroadcastRing {
    public:
        static_assert(std::is_trivially_copyable_v<T>, "records live in raw shared memory and are copied bytewise");
        static_assert(alignof(T) <= ANIMUS_CACHE_LINE_SIZE, "slots are packed back to back from a cache-line boundary");

        static constexpr uint64_t kMagic = 0x42524F4144434153ull; // "BROADCAS"

        // Bytes of memory a ring of `capacity` slots needs.
        static constexpr size_t required_bytes(size_t capacity) noexcept {
            return sizeof(BroadcastHeader) + capacity * sizeof(T);
        }

        // Writer side. `mem` must be ANIMUS_CACHE_LINE_SIZE-aligned and at least
        // required_bytes(capacity) long; capacity a power of two >= 2. Call once,
        // before any reader attaches.
        static bool init(void* mem, size_t bytes, size_t capacity, BroadcastRing& out) noexcept {
            if (!mem || !is_pow2(capacity) || !aligned(mem) || bytes < required_bytes(capacity)) return false;
            auto* h = new (mem) BroadcastHeader();
            h->capacity = capacity;
            h->mask = capacity - 1;
            h->payload_size = sizeof(T);
            h->magic = kMagic;
            out = BroadcastRing(h);
            return true;
        }

        // Reader side (also usable by the writer's process for a second view).
        // Validates the header before trusting any field of it.
        static bool attach(void* mem, size_t bytes, BroadcastRing& out) noexcept {
            if (!mem || !aligned(mem) || bytes < sizeof(BroadcastHeader)) return false;
            auto* h = static_cast<BroadcastHeader*>(mem);
            if (h->magic != kMagic || h->payload_size != sizeof(T) || !is_pow2(h->capacity) ||
                h->mask != h->capacity - 1 || bytes < required_bytes(h->capacity)) return false;
            out = BroadcastRing(h);
            return true;
        }

        BroadcastRing() noexcept = default;
        bool valid() const noexcept { return h_ != nullptr; }
        size_t capacity() const noexcept { return h_ ? static_cast<size_t>(h_->capacity) : 0; }

        // --- writer (exactly one thread/process) ---------------------------
        void publish(const T& value) noexcept {
            const uint64_t w = h_->cursor.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release); // cursor == w is visible before any byte of write w
            std::memcpy(&slots_[w & h_->mask], &value, sizeof(T));
            h_->cursor.store(w + 1, std::memory_order_release);
        }
        uint64_t writer_cursor() const noexcept { return h_->cursor.load(std::memory_order_acquire); }

        // --- reader (each reader owns its own view; shared memory is read-only) ---
        // Copies up to max_count of the oldest unread records into `out`, in
        // order, and returns how many. Never blocks, never retries: a record
        // the writer recycled is skipped and counted in dropped(). Entries of
        // `out` at index >= the return value hold unspecified bytes.
        size_t poll(T* out, size_t max_count) noexcept {
            const uint64_t cap = h_->capacity;
            const uint64_t c = h_->cursor.load(std::memory_order_acquire);
            if (c - read_ > cap - 1) { // fell out of the readable window: snap, deterministically counted
                dropped_ += (c - (cap - 1)) - read_;
                read_ = c - (cap - 1);
            }
            const uint64_t avail = c - read_;
            const size_t n = avail < max_count ? static_cast<size_t>(avail) : max_count;
            if (n == 0) return 0;
            const uint64_t first = read_;
            for (size_t i = 0; i < n; ++i) std::memcpy(&out[i], &slots_[(first + i) & h_->mask], sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t c2 = h_->cursor.load(std::memory_order_relaxed);
            // Intact iff c2 - i < cap, i.e. i >= c2 - cap + 1. The recycled records
            // are exactly the oldest ones, so they form a prefix of the batch.
            const uint64_t oldest_intact = c2 >= cap ? c2 - cap + 1 : 0;
            size_t lost = 0;
            if (oldest_intact > first) lost = static_cast<size_t>(oldest_intact - first < n ? oldest_intact - first : n);
            read_ = first + n;
            if (lost > 0) {
                dropped_ += lost;
                if (lost < n) std::memmove(out, out + lost, (n - lost) * sizeof(T));
            }
            return n - lost;
        }
        uint64_t read_cursor() const noexcept { return read_; }  // next record this reader will look at
        uint64_t dropped() const noexcept { return dropped_; }   // records this reader missed; delivered + dropped + unread == writer_cursor

    private:
        explicit BroadcastRing(BroadcastHeader* h) noexcept
            : h_(h), slots_(reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(h) + sizeof(BroadcastHeader))) {}

        static constexpr bool is_pow2(uint64_t v) noexcept { return v >= 2 && (v & (v - 1)) == 0; }
        static bool aligned(const void* p) noexcept { return reinterpret_cast<uintptr_t>(p) % ANIMUS_CACHE_LINE_SIZE == 0; }

        BroadcastHeader* h_ = nullptr;
        T* slots_ = nullptr;
        uint64_t read_ = 0;    // reader-local cursor: never in shared memory
        uint64_t dropped_ = 0; // reader-local
    };

} // namespace ipc
} // namespace sys
} // namespace animus
