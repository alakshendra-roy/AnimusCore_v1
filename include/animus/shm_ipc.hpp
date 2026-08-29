#pragma once
// Phase 6: High-throughput shared-memory (SHM) IPC engine.
//
// Cross-platform, header-only, zero-copy single-producer/single-consumer
// ring buffer living entirely inside a named OS shared-memory mapping, so
// two independent OS processes -- not two threads in one process -- can
// exchange records with no serialization step, no syscall on the hot path,
// and no kernel round trip once both sides have mapped the segment. Every
// entry point is noexcept: this module exists to be called from hot,
// latency-sensitive producer/consumer loops, and an exception unwinding
// out of a push/pop is worse for those loops than the call simply
// reporting failure.
//
// Distinct from animus::SharedTelemetryChannel (animus.hpp): that class is
// a fixed-record-layout, wire-format-compatible-with-Python primitive
// (animus.shm's <QQQ> header / <QIIQ> record, deliberately NOT
// cache-line-padded so the byte layout matches the pure-Python side
// exactly). This module has no such wire-compatibility constraint -- it is
// a generic, cache-line-padded ring for any trivially-copyable T, built for
// raw producer/consumer latency and throughput between two native
// processes, not cross-language interop. Reuses animus::cpu_relax() and
// ANIMUS_CACHE_LINE_SIZE from thread_affinity.hpp (that header's own
// dependency-free, noexcept design is exactly what a hot spin-poll loop
// here needs) rather than duplicating them.
//
// Platform coverage, same split as animus.hpp's own SharedMemorySegment:
//   - Windows: CreateFileMappingA/MapViewOfFile/OpenFileMappingA (pagefile-
//     backed, not a real file on disk) with VirtualQuery for size discovery
//     on the attach side, since MapViewOfFile has no "how big is this"
//     return value of its own.
//   - Linux (and other POSIX): shm_open/ftruncate/mmap, with fstat for size
//     discovery on the attach side.

#include "thread_affinity.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace animus {
namespace sys {
namespace ipc {

    // RAII wrapper over one named OS shared-memory mapping. One process
    // calls create() to allocate and own the segment; any other process on
    // the same machine calls open() with the same name to map the
    // identical physical pages -- writes on one side are visible to reads
    // on the other with no copy, no syscall, no kernel round trip once
    // mapped. Mirrors animus.hpp's SharedMemorySegment (same proven
    // approach, verified there against a real create/attach round trip)
    // but kept as an independent copy here so this header has no
    // dependency on animus.hpp's much larger include footprint.
    class SharedMemoryRegion {
    public:
        SharedMemoryRegion() noexcept = default;
        ~SharedMemoryRegion() { close(); }
        SharedMemoryRegion(const SharedMemoryRegion&) = delete;
        SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;

        SharedMemoryRegion(SharedMemoryRegion&& other) noexcept { *this = std::move(other); }
        SharedMemoryRegion& operator=(SharedMemoryRegion&& other) noexcept {
            if (this != &other) {
                close();
                data_ = other.data_;
                size_ = other.size_;
#if defined(_WIN32)
                handle_ = other.handle_;
                other.handle_ = nullptr;
#endif
                other.data_ = nullptr;
                other.size_ = 0;
            }
            return *this;
        }

        // Allocates a new named segment of exactly `size` bytes and maps
        // it into this process. Returns false (out left empty) on any
        // failure -- a name collision with an already-existing segment
        // included, since silently reusing a stale segment of the wrong
        // size would be worse than failing loudly.
        static bool create(const char* name, size_t size, SharedMemoryRegion& out) noexcept {
            out.close();
            if (!name || size == 0) return false;
#if defined(_WIN32)
            HANDLE h = CreateFileMappingA(
                INVALID_HANDLE_VALUE, // backed by the system paging file, not a real file on disk
                nullptr,
                PAGE_READWRITE,
                static_cast<DWORD>((static_cast<uint64_t>(size) >> 32) & 0xFFFFFFFFu),
                static_cast<DWORD>(static_cast<uint64_t>(size) & 0xFFFFFFFFu),
                name);
            if (!h) return false;
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                // A mapping with this name already exists -- its size may
                // not match `size`, so refuse rather than silently mapping
                // a differently-sized, possibly in-use segment.
                CloseHandle(h);
                return false;
            }
            void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
            if (!view) {
                CloseHandle(h);
                return false;
            }
            out.handle_ = h;
            out.data_ = view;
            out.size_ = size;
            return true;
#else
            int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
            if (fd < 0) return false; // includes EEXIST -- same "don't silently reuse a stale segment" reasoning as Windows above
            if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
                ::close(fd);
                shm_unlink(name);
                return false;
            }
            void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd); // the mapping keeps the underlying object alive; the fd itself isn't needed after mmap
            if (addr == MAP_FAILED) {
                shm_unlink(name);
                return false;
            }
            out.data_ = addr;
            out.size_ = size;
            return true;
#endif
        }

        // Maps an existing segment created by another process's create()
        // call. Size is discovered from the OS, not supplied by the
        // caller, so both sides always agree on it.
        static bool open(const char* name, SharedMemoryRegion& out) noexcept {
            out.close();
            if (!name) return false;
#if defined(_WIN32)
            HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (!h) return false;
            void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0); // 0 = map the whole committed region
            if (!view) {
                CloseHandle(h);
                return false;
            }
            // MapViewOfFile doesn't hand back how large the mapping is;
            // VirtualQuery on the mapped base address reports the size of
            // the contiguous region sharing its protection/state, which
            // for a single MapViewOfFile call is exactly the whole view.
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0) {
                UnmapViewOfFile(view);
                CloseHandle(h);
                return false;
            }
            out.handle_ = h;
            out.data_ = view;
            out.size_ = mbi.RegionSize;
            return true;
#else
            int fd = ::open(name, O_RDWR);
            if (fd < 0) return false;
            struct stat st{};
            if (fstat(fd, &st) != 0 || st.st_size <= 0) {
                ::close(fd);
                return false;
            }
            size_t size = static_cast<size_t>(st.st_size);
            void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);
            if (addr == MAP_FAILED) return false;
            out.data_ = addr;
            out.size_ = size;
            return true;
#endif
        }

        // Destroys the underlying OS shared-memory object. POSIX:
        // shm_unlink -- call once every attached process has closed its
        // mapping. Windows: named file mappings have no separate "unlink"
        // step -- the OS object is destroyed automatically once the last
        // HANDLE to it (this process's and every attached process's)
        // closes, so this is a documented no-op there, not a
        // silently-missing feature.
        static bool unlink(const char* name) noexcept {
#if defined(_WIN32)
            (void)name;
            return true;
#else
            if (!name) return false;
            return shm_unlink(name) == 0;
#endif
        }

        void* data() const noexcept { return data_; }
        size_t size() const noexcept { return size_; }
        bool valid() const noexcept { return data_ != nullptr; }

        void close() noexcept {
            if (data_) {
#if defined(_WIN32)
                UnmapViewOfFile(data_);
#else
                munmap(data_, size_);
#endif
                data_ = nullptr;
                size_ = 0;
            }
#if defined(_WIN32)
            if (handle_) {
                CloseHandle(handle_);
                handle_ = nullptr;
            }
#endif
        }

    private:
        void* data_ = nullptr;
        size_t size_ = 0;
#if defined(_WIN32)
        HANDLE handle_ = nullptr;
#endif
    };

    // Single-producer/single-consumer, fixed-capacity ring living entirely
    // inside a SharedMemoryRegion. Same head/tail algorithm as animus.hpp's
    // in-process SpscRingBuffer (a relaxed load of the caller's own last
    // index, an acquire load of the other side's index, a release store to
    // publish -- no compare-exchange retry loop, since there is only ever
    // one writer and one reader for any given slot), but placement-based
    // (storage is the mapped segment, not an owned std::vector) and with
    // head/tail each pinned to their own cache line via the Header below,
    // since here the two sides are two different processes on two
    // different cores, and false sharing between them costs cross-socket
    // (not just cross-core) cache-coherency traffic -- worth padding for
    // explicitly, unlike animus.hpp's wire-compatible SharedRingHeader,
    // which deliberately does NOT pad (see that struct's own comment).
    //
    // T must be trivially copyable: records live in raw shared memory with
    // no serialization step, so copying T's bytes must be both meaningful
    // and safe (no owned pointers/heap resources a naive memcpy-equivalent
    // copy would corrupt or leak).
    template <typename T>
    class ShmRing {
    public:
        static_assert(std::is_trivially_copyable<T>::value,
            "T must be trivially copyable -- ShmRing<T> places T directly in shared memory "
            "with no serialization step, so a naive byte-copy across the process boundary "
            "must be a valid, complete copy of T");

        ShmRing(const ShmRing&) = delete;
        ShmRing& operator=(const ShmRing&) = delete;

        // Allocates a new named ring sized for at least `requested_capacity`
        // slots (rounded up to a power of two, so slot indexing is a plain
        // AND mask, not a modulo -- consistent with animus.hpp's
        // LockFreeRingBuffer/SpscRingBuffer). Returns nullptr on failure,
        // including a name collision (see SharedMemoryRegion::create).
        static std::unique_ptr<ShmRing> create(const char* name, size_t requested_capacity) noexcept {
            const uint64_t capacity = round_up_pow2(requested_capacity < 2 ? 2 : requested_capacity);
            const size_t total_size = sizeof(Header) + static_cast<size_t>(capacity) * sizeof(T);

            SharedMemoryRegion region;
            if (!SharedMemoryRegion::create(name, total_size, region)) {
                return nullptr;
            }
            // Placement-new: this is raw, freshly-mapped OS memory (zeroed
            // by the OS on both Windows and POSIX), not a live Header
            // object yet -- constructing one in place is what makes the
            // atomics inside it valid to use, not merely "probably zero".
            auto* header = new (region.data()) Header();
            header->capacity = capacity;
            header->mask = capacity - 1;

            std::unique_ptr<ShmRing> ring(new ShmRing());
            ring->region_ = std::move(region);
            ring->header_ = header;
            ring->slots_ = reinterpret_cast<T*>(header + 1);
            return ring;
        }

        // Maps an existing ring created by another process's create() call.
        // Returns nullptr if the segment doesn't exist, is too small to
        // hold a valid header, or the header's own capacity claim doesn't
        // fit the mapped region (a torn/foreign segment).
        static std::unique_ptr<ShmRing> open(const char* name) noexcept {
            SharedMemoryRegion region;
            if (!SharedMemoryRegion::open(name, region)) {
                return nullptr;
            }
            if (region.size() < sizeof(Header)) {
                return nullptr;
            }
            auto* header = reinterpret_cast<Header*>(region.data());
            const uint64_t capacity = header->capacity;
            const bool capacity_is_pow2 = capacity != 0 && (capacity & (capacity - 1)) == 0;
            if (!capacity_is_pow2 || header->mask != capacity - 1) {
                return nullptr;
            }
            if (region.size() < sizeof(Header) + static_cast<size_t>(capacity) * sizeof(T)) {
                return nullptr; // header claims a capacity the mapped region doesn't actually have room for
            }

            std::unique_ptr<ShmRing> ring(new ShmRing());
            ring->region_ = std::move(region);
            ring->header_ = header;
            ring->slots_ = reinterpret_cast<T*>(header + 1);
            return ring;
        }

        static bool unlink(const char* name) noexcept { return SharedMemoryRegion::unlink(name); }

        bool valid() const noexcept { return header_ != nullptr; }
        size_t capacity() const noexcept { return header_ ? static_cast<size_t>(header_->capacity) : 0; }

        // Producer-side only (one process). Never blocks; returns false
        // if the ring is full.
        bool try_push(const T& value) noexcept {
            const uint64_t head = header_->head.load(std::memory_order_relaxed);
            const uint64_t tail = header_->tail.load(std::memory_order_acquire);
            if (head - tail >= header_->capacity) return false; // full
            slots_[head & header_->mask] = value;
            header_->head.store(head + 1, std::memory_order_release);
            return true;
        }

        // Consumer-side only (one process). Never blocks; returns false
        // if the ring is empty.
        bool try_pop(T& out) noexcept {
            const uint64_t tail = header_->tail.load(std::memory_order_relaxed);
            const uint64_t head = header_->head.load(std::memory_order_acquire);
            if (tail == head) return false; // empty
            out = slots_[tail & header_->mask];
            header_->tail.store(tail + 1, std::memory_order_release);
            return true;
        }

        // Spin-polling variants: retry try_push/try_pop with
        // animus::cpu_relax() between attempts (the fallback this module's
        // spec calls for) until it succeeds or `max_spins` attempts have
        // been made. Still never blocks in the OS-scheduler sense -- a
        // failed push_spin/pop_spin just means the other side didn't show
        // up in time, not a lock or syscall wait -- so these remain
        // noexcept and safe to bound tightly on a hot path. A very large
        // default lets these behave like "wait for the other process" in
        // practice for a healthy peer, while still returning eventually
        // (rather than truly hanging forever) if that peer has died.
        bool push_spin(const T& value, uint64_t max_spins = kDefaultMaxSpins) noexcept {
            for (uint64_t i = 0; i < max_spins; ++i) {
                if (try_push(value)) return true;
                cpu_relax();
            }
            return false;
        }

        bool pop_spin(T& out, uint64_t max_spins = kDefaultMaxSpins) noexcept {
            for (uint64_t i = 0; i < max_spins; ++i) {
                if (try_pop(out)) return true;
                cpu_relax();
            }
            return false;
        }

    private:
        ShmRing() noexcept = default;

        static constexpr uint64_t kDefaultMaxSpins = 200'000'000ull;

        // Three cache lines: read-only metadata (capacity/mask, written
        // once at create() and never again), the producer's head cursor,
        // and the consumer's tail cursor -- each on its own line so a
        // producer's head update never invalidates the cache line the
        // consumer is spin-polling tail from, and vice versa. This is the
        // false-sharing elimination the module's spec calls for; it is
        // exactly animus.hpp's LockFreeRingBuffer/SpscRingBuffer's own
        // alignas(64) enqueue_pos_/dequeue_pos_ split, just expressed as
        // struct layout instead of two separate member variables, since
        // this header (unlike those in-process rings) must also be
        // reconstructible by a second process from raw bytes alone.
        struct alignas(ANIMUS_CACHE_LINE_SIZE) Header {
            uint64_t capacity = 0;
            uint64_t mask = 0;
            alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> head{ 0 }; // producer-only writes
            alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> tail{ 0 }; // consumer-only writes
        };
        static_assert(std::atomic<uint64_t>::is_always_lock_free,
            "Header lives in memory shared across process boundaries -- a non-lock-free "
            "std::atomic<uint64_t> could fall back to a mutex/futex that isn't valid there");
        static_assert(sizeof(Header) == 3 * ANIMUS_CACHE_LINE_SIZE,
            "Header must be exactly 3 cache lines: metadata, head, tail -- if this fails, "
            "the alignas(ANIMUS_CACHE_LINE_SIZE) members above aren't landing on distinct lines");

        static uint64_t round_up_pow2(size_t v) noexcept {
            uint64_t p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        SharedMemoryRegion region_;
        Header* header_ = nullptr;
        T* slots_ = nullptr;
    };

} // namespace ipc
} // namespace sys
} // namespace animus
