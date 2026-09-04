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
#include "shm_lifecycle.hpp"
#include "schema.hpp"

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
            // Bug fixed here (caught on real Linux CI, never on the Windows
            // branch above or on the pure-Python multiprocessing.shared_memory
            // path, which does its own correct shm_open internally): this
            // must be shm_open(), not the raw ::open() syscall. shm_open()
            // resolves `name` through the shared-memory namespace (glibc:
            // strips a leading '/' if present, then looks under /dev/shm/)
            // the exact same way create()'s shm_open() call above does --
            // ::open() on a bare name like "my_ring" instead performs an
            // ordinary filesystem lookup relative to the current working
            // directory, which is never where create() actually put it.
            int fd = shm_open(name, O_RDWR, 0600);
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

    // Discriminates the two structurally different wire-header layouts
    // this file defines (RingHeader below for ShmRing<T>'s SPSC ring,
    // SpmcRingHeader further down for SpmcRing<T>'s broadcast ring).
    // Their leading wire-descriptor fields (capacity through wire_format)
    // are deliberately laid out identically -- same offsets, same sizes --
    // so schema validation and inspection code can treat either header's
    // prefix uniformly; but everything after that prefix differs (RingHeader
    // has a shared tail cursor and per-consumer pid/heartbeat, SpmcRingHeader
    // has neither -- see that struct's own comment), so a segment's actual
    // ring_kind must still be checked before reading anything past
    // wire_format: attaching the wrong ring kind to a segment would
    // otherwise silently misinterpret the producer-owned line's fields
    // and read the T array from the wrong offset (RingHeader and
    // SpmcRingHeader are different total sizes), with no other check able
    // to catch it -- payload_size/schema_version_hash alone are identical
    // for the same T regardless of which ring kind created the segment.
    enum class RingKind : uint64_t { Spsc = 0, SpmcBroadcast = 1 };

    // Wire descriptor + SPSC cursor header shared by every ShmRing<T>
    // instantiation (Milestone 1) -- factored out of the class template,
    // unlike the original design, because nothing in it actually depends
    // on T. That lets a process which has never heard of T attach to a
    // segment and read its schema metadata directly (see RawSchemaView
    // below): payload size, stride, schema version hash, and a
    // struct.calcsize-compatible wire format string, all written once at
    // create() time. This is exactly what bindings/animus_shm_py.cpp's
    // schema-agnostic Python binding needs to build a matching NumPy dtype
    // at runtime instead of hardcoding one compiled-in record type.
    //
    // Layout is a whole number of cache lines (see the static_assert
    // below), not necessarily the fixed "3 lines" the original
    // {capacity, mask}-only header always was: the read-only wire
    // descriptor now itself spans however many lines its fields need
    // (2 on a 64-byte-line target, 1 on a 128-byte-line target such as
    // ARM64 -- see ANIMUS_CACHE_LINE_SIZE, thread_affinity.hpp) before the
    // producer- and consumer-owned lines that follow it.
    //   - read-only wire descriptor: capacity, mask, ring_kind,
    //     schema_version_hash, payload_size, stride, wire_format[] --
    //     written once at create() and never again.
    //   - producer-owned line: head plus every field only the producer
    //     ever writes. Sharing this line among producer-only writes is
    //     free -- same single writer, so there is no false sharing to
    //     avoid, unlike splitting head from tail (different writers).
    //   - consumer-owned line: tail plus the consumer's own pid/heartbeat.
#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable: 4324) // "structure was padded due to alignment specifier" -- the padding
                                    // IS the point (cache-line separation between producer- and
                                    // consumer-owned fields, this struct's whole reason for existing,
                                    // per its own class-level comment above), not an accident /W4 should
                                    // flag; MSVC has no way to distinguish that from unintentional
                                    // padding, so this is the standard, correct way to acknowledge it.
#endif
    struct alignas(ANIMUS_CACHE_LINE_SIZE) RingHeader {
        // 80 bytes is generous headroom for any realistic fixed wire
        // format string while keeping the whole read-only descriptor
        // section an exact multiple of a 64-byte cache line: the 6
        // preceding uint64_t fields (48 bytes) + 80 == 128 == 2 lines.
        static constexpr size_t kWireFormatBufSize = 80;

        uint64_t capacity = 0;
        uint64_t mask = 0;
        uint64_t ring_kind = static_cast<uint64_t>(RingKind::Spsc);
        uint64_t schema_version_hash = 0; // animus::schema::Traits<T>::kVersionHash at create() time
        uint64_t payload_size = 0;        // sizeof(T), in bytes
        uint64_t stride = 0;              // bytes between consecutive slots (== payload_size; no per-slot padding today)
        char wire_format[kWireFormatBufSize] = {}; // animus::schema::Traits<T>::kWireFormat, NUL-terminated, "" if unregistered

        alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> head{ 0 };
        std::atomic<uint64_t> dropped_count{ 0 };
        std::atomic<uint64_t> producer_pid{ 0 };
        std::atomic<uint64_t> producer_heartbeat{ 0 };

        alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> tail{ 0 };
        std::atomic<uint64_t> consumer_pid{ 0 };
        std::atomic<uint64_t> consumer_heartbeat{ 0 };
    };
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "RingHeader lives in memory shared across process boundaries -- a non-lock-free "
        "std::atomic<uint64_t> could fall back to a mutex/futex that isn't valid there");
    static_assert(sizeof(RingHeader) % ANIMUS_CACHE_LINE_SIZE == 0,
        "RingHeader must be a whole number of cache lines -- if this fails, a field was added "
        "that isn't respecting the alignas(ANIMUS_CACHE_LINE_SIZE) boundaries above");

    // Bounded, NUL-terminating copy of a schema's wire format string into
    // a wire-descriptor header's fixed-size buffer -- truncates (rather
    // than overflowing or asserting) an unrealistically long format
    // string, since the numeric fields (payload_size/stride/
    // schema_version_hash) remain the authoritative validation at open()
    // time regardless; wire_format is documented as a best-effort
    // convenience for dynamic NumPy dtype construction on the Python
    // side, not a safety-critical field. Templated on the header type (not
    // just RingHeader) so RingHeader and SpmcRingHeader below can share
    // this one implementation despite having differently-sized
    // wire_format buffers -- both expose the same
    // `char wire_format[kWireFormatBufSize]` shape.
    template <typename Header>
    inline void set_wire_format(Header& header, const char* src) noexcept {
        size_t i = 0;
        for (; src && src[i] != '\0' && i + 1 < Header::kWireFormatBufSize; ++i) {
            header.wire_format[i] = src[i];
        }
        header.wire_format[i] = '\0';
    }

    // Single-producer/single-consumer, fixed-capacity ring living entirely
    // inside a SharedMemoryRegion. Same head/tail algorithm as animus.hpp's
    // in-process SpscRingBuffer (a relaxed load of the caller's own last
    // index, an acquire load of the other side's index, a release store to
    // publish -- no compare-exchange retry loop, since there is only ever
    // one writer and one reader for any given slot), but placement-based
    // (storage is the mapped segment, not an owned std::vector) and with
    // head/tail each pinned to their own cache line via RingHeader above,
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
        static_assert(alignof(T) <= 64,
            "T's alignment must not exceed 64 bytes -- ShmRing<T> packs slots back-to-back with "
            "no per-slot padding beyond T's own size (see RingHeader::stride), so an over-aligned "
            "T could straddle the guarantee every slot after the first is validly aligned for it");

        ShmRing(const ShmRing&) = delete;
        ShmRing& operator=(const ShmRing&) = delete;

        // Allocates a new named ring sized for at least `requested_capacity`
        // slots (rounded up to a power of two, so slot indexing is a plain
        // AND mask, not a modulo -- consistent with animus.hpp's
        // LockFreeRingBuffer/SpscRingBuffer). Returns nullptr on failure,
        // including a name collision (see SharedMemoryRegion::create).
        static std::unique_ptr<ShmRing> create(const char* name, size_t requested_capacity) noexcept {
            const uint64_t capacity = round_up_pow2(requested_capacity < 2 ? 2 : requested_capacity);
            const size_t total_size = sizeof(RingHeader) + static_cast<size_t>(capacity) * sizeof(T);

            SharedMemoryRegion region;
            if (!SharedMemoryRegion::create(name, total_size, region)) {
                return nullptr;
            }
            // Placement-new: this is raw, freshly-mapped OS memory (zeroed
            // by the OS on both Windows and POSIX), not a live Header
            // object yet -- constructing one in place is what makes the
            // atomics inside it valid to use, not merely "probably zero".
            auto* header = new (region.data()) RingHeader();
            header->capacity = capacity;
            header->mask = capacity - 1;
            header->ring_kind = static_cast<uint64_t>(RingKind::Spsc);
            // Milestone 1 wire descriptor: stamps this segment with T's
            // schema identity so a later open() (this process or another)
            // can refuse to attach with the wrong T -- see open() below.
            header->schema_version_hash = schema::Traits<T>::kVersionHash;
            header->payload_size = static_cast<uint64_t>(sizeof(T));
            header->stride = static_cast<uint64_t>(sizeof(T));
            set_wire_format(*header, schema::Traits<T>::kWireFormat);

            std::unique_ptr<ShmRing> ring(new ShmRing());
            ring->region_ = std::move(region);
            ring->header_ = header;
            ring->slots_ = reinterpret_cast<T*>(header + 1);
            return ring;
        }

        // Maps an existing ring created by another process's create() call.
        // Returns nullptr if the segment doesn't exist, is too small to
        // hold a valid header, the header's own capacity claim doesn't fit
        // the mapped region (a torn/foreign segment), or -- Milestone 1 --
        // the segment's stamped schema identity doesn't match this T:
        // wrong payload size, or the same size but a different registered
        // schema::Traits<T> (name/wire format). Before this check, two
        // distinct same-size T's could attach to the same segment and
        // silently misinterpret each other's fields; now that mismatch is
        // rejected here instead, loudly and before a single byte is read.
        static std::unique_ptr<ShmRing> open(const char* name) noexcept {
            SharedMemoryRegion region;
            if (!SharedMemoryRegion::open(name, region)) {
                return nullptr;
            }
            if (region.size() < sizeof(RingHeader)) {
                return nullptr;
            }
            auto* header = reinterpret_cast<RingHeader*>(region.data());
            if (header->ring_kind != static_cast<uint64_t>(RingKind::Spsc)) {
                return nullptr; // this segment is an SpmcRing<T> broadcast ring, not an SPSC ShmRing<T>
            }
            const uint64_t capacity = header->capacity;
            const bool capacity_is_pow2 = capacity != 0 && (capacity & (capacity - 1)) == 0;
            if (!capacity_is_pow2 || header->mask != capacity - 1) {
                return nullptr;
            }
            if (header->payload_size != static_cast<uint64_t>(sizeof(T))) {
                return nullptr; // wrong record size -- a different T (or ABI drift) than create() used
            }
            if (header->schema_version_hash != schema::Traits<T>::kVersionHash) {
                return nullptr; // same size, different registered schema -- refuse rather than misread
            }
            if (region.size() < sizeof(RingHeader) + static_cast<size_t>(capacity) * sizeof(T)) {
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

        // Milestone 1 wire descriptor accessors -- the same fields open()
        // above validates, exposed so a caller (or the Python binding, via
        // its own RawSchemaView-backed channel) can report which schema is
        // actually attached without needing to know T's identity any other
        // way. schema_version_hash()/payload_size()/stride() reflect
        // whatever create() stamped into the header (always == T's own
        // identity for a successfully-open()'d ring, by construction of
        // the check above); schema_name()/kWireFormat() are compile-time
        // constants from animus::schema::Traits<T> and need no header read.
        uint64_t schema_version_hash() const noexcept { return header_ ? header_->schema_version_hash : 0; }
        size_t payload_size() const noexcept { return header_ ? static_cast<size_t>(header_->payload_size) : 0; }
        size_t stride() const noexcept { return header_ ? static_cast<size_t>(header_->stride) : 0; }
        const char* wire_format() const noexcept { return header_ ? header_->wire_format : ""; }
        static constexpr const char* schema_name() noexcept { return schema::Traits<T>::kName; }

        // Producer-side only (one process). Never blocks; returns false
        // if the ring is full. Use this when a full ring should be treated
        // as backpressure the caller reacts to (retry, spill elsewhere,
        // count as a hard drop at the call site).
        bool try_push(const T& value) noexcept {
            const uint64_t head = header_->head.load(std::memory_order_relaxed);
            const uint64_t tail = header_->tail.load(std::memory_order_acquire);
            if (head - tail >= header_->capacity) return false; // full
            slots_[head & header_->mask] = value;
            header_->head.store(head + 1, std::memory_order_release);
            return true;
        }

        // Producer-side only. Decoupled/overwrite mode: never blocks and
        // never refuses -- if the ring is full, forcibly reclaims the
        // oldest not-yet-consumed slot (the one at the current `tail`) by
        // advancing tail past it and incrementing dropped_count(), then
        // writes the new value. This is the "producer must not block the
        // core execution thread" contract: a lagging or dead consumer can
        // never slow the producer down, only cause it to lose visibility
        // into old data it hasn't consumed yet -- deterministically counted,
        // not silently lost.
        //
        // Concurrency note: reclaiming the oldest slot while a consumer may
        // concurrently be mid-try_pop()/pop_spin() on that exact slot is a
        // deliberate, documented race, not an oversight -- the consumer's
        // acquire-load of `head` and relaxed-load of `tail` give it no
        // way to distinguish "genuinely empty" from "just overwritten out
        // from under me" in overwrite mode, so a concurrent reader can
        // observe a torn record at the reclaim boundary. Only pair
        // push_overwrite() with a consumer that tolerates occasional torn
        // reads at that boundary (e.g. best-effort telemetry/sampling) --
        // never with a channel that must never observe a torn record. A
        // consumer that must see clean records with a lagging producer
        // should use try_push()/push_spin() (bounded backpressure) instead.
        void push_overwrite(const T& value) noexcept {
            const uint64_t head = header_->head.load(std::memory_order_relaxed);
            const uint64_t tail = header_->tail.load(std::memory_order_acquire);
            if (head - tail >= header_->capacity) {
                // Full: reclaim the oldest slot ourselves. fetch_add (not a
                // plain store) so this stays correct even if a concurrent
                // try_pop() on the consumer side has already moved tail
                // past this exact value -- either way, tail ends up at
                // least one slot further along than it started here.
                header_->tail.fetch_add(1, std::memory_order_acq_rel);
                header_->dropped_count.fetch_add(1, std::memory_order_relaxed);
            }
            slots_[head & header_->mask] = value;
            header_->head.store(head + 1, std::memory_order_release);
        }

        // Count of records reclaimed by push_overwrite() because the ring
        // was full when they were pushed -- the deterministic loss-tracking
        // half of the decoupled overwrite contract. Always 0 for a ring
        // whose producer only ever calls try_push()/push_spin().
        uint64_t dropped_count() const noexcept {
            return header_ ? header_->dropped_count.load(std::memory_order_relaxed) : 0;
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

        // --- Lifecycle / zombie-state prevention ---------------------------
        //
        // Publishing a side's OS pid plus a monotonically-incrementing
        // heartbeat into the header lets the *other* side detect an abrupt
        // termination (kill -9, a segfault the crashed process never got to
        // handle) without that detection depending on any atomic wait state
        // that could itself deadlock: try_push/try_pop/push_spin/pop_spin
        // already never block indefinitely (push_spin/pop_spin are bounded
        // by max_spins and return false, not hang), so what a caller
        // actually needs here is a way to tell "the peer is gone, stop
        // retrying" apart from "the peer is just momentarily behind" --
        // that's what is_producer_alive()/is_consumer_alive() answer.
        //
        // Call mark_producer_attached()/mark_consumer_attached() once, right
        // after create()/open(), from whichever process is playing that
        // role. Call the matching heartbeat() periodically (e.g. once per
        // push()/pop() call, or on a timer) from the same process. A dead
        // process's pid becomes instantly unambiguous to the OS (POSIX:
        // kill(pid, 0) fails ESRCH; Windows: OpenProcess fails or the
        // handle's exit code is no longer STILL_ACTIVE) even under kill -9,
        // which gives no chance to run any cleanup of its own -- the
        // heartbeat is a secondary, coarser signal for a peer that is alive
        // but wedged (e.g. stopped under a debugger, deadlocked elsewhere).
        void mark_producer_attached() noexcept {
            header_->producer_pid.store(sys::lifecycle::current_process_id(), std::memory_order_release);
            header_->producer_heartbeat.store(1, std::memory_order_release);
        }
        void mark_consumer_attached() noexcept {
            header_->consumer_pid.store(sys::lifecycle::current_process_id(), std::memory_order_release);
            header_->consumer_heartbeat.store(1, std::memory_order_release);
        }
        void producer_heartbeat() noexcept {
            header_->producer_heartbeat.fetch_add(1, std::memory_order_relaxed);
        }
        void consumer_heartbeat() noexcept {
            header_->consumer_heartbeat.fetch_add(1, std::memory_order_relaxed);
        }

        // True unless the producer has attached and then either exited (its
        // pid no longer refers to a live process) or gone silent for
        // `stale_after` consecutive heartbeat() calls observed with no
        // change -- a producer that never called mark_producer_attached()
        // (pid still 0) is reported alive, since "no producer has shown up
        // yet" is a different condition from "the producer died" and this
        // function only answers the latter.
        bool is_producer_alive(uint64_t stale_after = 0) const noexcept {
            return peer_alive(header_->producer_pid, header_->producer_heartbeat,
                               last_seen_producer_heartbeat_, producer_stall_count_, stale_after);
        }
        bool is_consumer_alive(uint64_t stale_after = 0) const noexcept {
            return peer_alive(header_->consumer_pid, header_->consumer_heartbeat,
                               last_seen_consumer_heartbeat_, consumer_stall_count_, stale_after);
        }

    private:
        // Shared implementation for is_producer_alive/is_consumer_alive.
        // `last_seen`/`stall_count` are this ShmRing instance's own mutable
        // cache fields (not shared state) -- staleness is tracked
        // per-caller, across successive is_*_alive() polls, deliberately
        // not derived from wall-clock time so this stays free of any clock
        // dependency and remains exact under a paused/single-stepped peer.
        static bool peer_alive(const std::atomic<uint64_t>& pid_field,
                               const std::atomic<uint64_t>& heartbeat_field,
                               uint64_t& last_seen, uint64_t& stall_count, uint64_t stale_after) noexcept {
            const uint64_t pid = pid_field.load(std::memory_order_acquire);
            if (pid == 0) return true; // peer hasn't attached yet -- not a death
            if (!sys::lifecycle::is_process_alive(pid)) return false;
            if (stale_after == 0) return true; // pid-liveness only, no heartbeat staleness check requested
            const uint64_t hb = heartbeat_field.load(std::memory_order_acquire);
            if (hb != last_seen) {
                last_seen = hb;
                stall_count = 0;
                return true;
            }
            return ++stall_count < stale_after;
        }

        mutable uint64_t last_seen_producer_heartbeat_ = 0;
        mutable uint64_t last_seen_consumer_heartbeat_ = 0;
        mutable uint64_t producer_stall_count_ = 0;
        mutable uint64_t consumer_stall_count_ = 0;

    private:
        ShmRing() noexcept = default;

        static constexpr uint64_t kDefaultMaxSpins = 200'000'000ull;

        static uint64_t round_up_pow2(size_t v) noexcept {
            uint64_t p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        SharedMemoryRegion region_;
        RingHeader* header_ = nullptr;
        T* slots_ = nullptr;
    };

    // Schema-agnostic, read-only attach: opens a named ShmRing<T> segment
    // (any T, as long as it was created via ShmRing<T>::create(), which
    // always writes a RingHeader) and exposes its wire descriptor --
    // payload size, stride, schema version hash, wire format string --
    // plus the raw producer/consumer cursors and a raw pointer to the slot
    // array, all without the caller ever naming T. This is what lets
    // bindings/animus_shm_py.cpp's schema-agnostic Python channel build a
    // NumPy dtype at runtime from wire_format() instead of hardcoding one
    // compiled-in record type; it is not a re-implementation of
    // ShmRing<T>::open()'s validation logic, just a view that stops one
    // step earlier, before the "does this T match" check that requires
    // knowing T at compile time.
    class RawSchemaView {
    public:
        RawSchemaView() noexcept = default;
        RawSchemaView(const RawSchemaView&) = delete;
        RawSchemaView& operator=(const RawSchemaView&) = delete;
        RawSchemaView(RawSchemaView&&) noexcept = default;
        RawSchemaView& operator=(RawSchemaView&&) noexcept = default;

        // Same structural checks as ShmRing<T>::open() minus the T-specific
        // ones (payload size / schema version hash), since this view never
        // knows T -- callers that need that guarantee should compare
        // schema_version_hash()/payload_size() against their own expected
        // schema::Traits<T> themselves, or use ShmRing<T>::open() directly.
        static std::unique_ptr<RawSchemaView> open(const char* name) noexcept {
            SharedMemoryRegion region;
            if (!SharedMemoryRegion::open(name, region)) {
                return nullptr;
            }
            if (region.size() < sizeof(RingHeader)) {
                return nullptr;
            }
            auto* header = reinterpret_cast<RingHeader*>(region.data());
            if (header->ring_kind != static_cast<uint64_t>(RingKind::Spsc)) {
                return nullptr; // this segment is an SpmcRing<T> broadcast ring, not a RingHeader-shaped SPSC one
            }
            const uint64_t capacity = header->capacity;
            const bool capacity_is_pow2 = capacity != 0 && (capacity & (capacity - 1)) == 0;
            if (!capacity_is_pow2 || header->mask != capacity - 1) {
                return nullptr;
            }
            if (header->payload_size == 0 || header->stride == 0) {
                return nullptr; // not a schema-descriptor header this milestone's create() ever wrote
            }
            if (region.size() < sizeof(RingHeader) + static_cast<size_t>(capacity) * static_cast<size_t>(header->stride)) {
                return nullptr; // header claims a capacity/stride the mapped region doesn't have room for
            }

            std::unique_ptr<RawSchemaView> view(new RawSchemaView());
            view->region_ = std::move(region);
            view->header_ = header;
            return view;
        }

        bool valid() const noexcept { return header_ != nullptr; }
        uint64_t capacity() const noexcept { return header_ ? header_->capacity : 0; }
        uint64_t schema_version_hash() const noexcept { return header_ ? header_->schema_version_hash : 0; }
        uint64_t payload_size() const noexcept { return header_ ? header_->payload_size : 0; }
        uint64_t stride() const noexcept { return header_ ? header_->stride : 0; }
        const char* wire_format() const noexcept { return header_ ? header_->wire_format : ""; }
        uint64_t head() const noexcept { return header_ ? header_->head.load(std::memory_order_acquire) : 0; }
        uint64_t tail() const noexcept { return header_ ? header_->tail.load(std::memory_order_acquire) : 0; }
        uint64_t dropped_count() const noexcept { return header_ ? header_->dropped_count.load(std::memory_order_relaxed) : 0; }

        // Raw pointer to slot 0 of the underlying array -- the caller must
        // treat this as capacity()*stride() bytes, mask-indexed exactly
        // like ShmRing<T>::try_pop does internally (slot i lives at
        // slots_base() + (i & (capacity()-1)) * stride()). Aliases the
        // live mapped segment: valid only as long as this RawSchemaView is
        // alive, and only [tail(), head()) (mod capacity()) is data an
        // active producer has actually published and not yet overwritten
        // -- same reader contract as the typed ShmRing<T> API.
        void* slots_base() const noexcept {
            return header_ ? reinterpret_cast<uint8_t*>(header_) + sizeof(RingHeader) : nullptr;
        }

    private:
        SharedMemoryRegion region_;
        RingHeader* header_ = nullptr;
    };

    // Wire descriptor + producer-only cursor header for SpmcRing<T>
    // (Milestone 2). Same leading wire-descriptor fields as RingHeader
    // above (capacity, mask, ring_kind, schema_version_hash, payload_size,
    // stride, wire_format[]) at identical offsets -- see RingKind's own
    // comment for why that matters -- but everything after that prefix is
    // deliberately smaller: broadcast mode has no shared tail at all.
    // Every consumer tracks its own read position in its own process
    // memory (SpmcRing<T>::local_tail_), which is exactly what lets N
    // independent consumers poll concurrently with no cross-process cache
    // contention over a shared cursor -- there is nothing here for them to
    // contend over, only one line (the producer's own) that only the
    // single producer ever writes.
#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable: 4324) // intentional cache-line padding -- see RingHeader's identical pragma above
#endif
    struct alignas(ANIMUS_CACHE_LINE_SIZE) SpmcRingHeader {
        static constexpr size_t kWireFormatBufSize = RingHeader::kWireFormatBufSize; // same 80 bytes, same reasoning

        uint64_t capacity = 0;
        uint64_t mask = 0;
        uint64_t ring_kind = static_cast<uint64_t>(RingKind::SpmcBroadcast);
        uint64_t schema_version_hash = 0;
        uint64_t payload_size = 0;
        uint64_t stride = 0;
        char wire_format[kWireFormatBufSize] = {};

        // The only other line: head plus the producer's own pid/heartbeat.
        // No dropped_count here either -- "dropped" is a per-consumer,
        // local concept in broadcast mode (SpmcRing<T>::overrun_count_),
        // since the producer never checks any consumer's position and so
        // has no way to know how many there are or how far behind each one
        // has fallen.
        alignas(ANIMUS_CACHE_LINE_SIZE) std::atomic<uint64_t> head{ 0 };
        std::atomic<uint64_t> producer_pid{ 0 };
        std::atomic<uint64_t> producer_heartbeat{ 0 };
    };
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "SpmcRingHeader lives in memory shared across process boundaries -- a non-lock-free "
        "std::atomic<uint64_t> could fall back to a mutex/futex that isn't valid there");
    static_assert(sizeof(SpmcRingHeader) % ANIMUS_CACHE_LINE_SIZE == 0,
        "SpmcRingHeader must be a whole number of cache lines -- if this fails, a field was added "
        "that isn't respecting the alignas(ANIMUS_CACHE_LINE_SIZE) boundaries above");

    // Single-producer/multi-consumer broadcast ring (Milestone 2). One
    // process creates and exclusively owns the shared `head` cursor
    // (broadcast() below); any number of independent processes attach via
    // open() and each poll at their own pace through a read cursor that
    // lives only in that process's own memory, never in shared memory --
    // the defining difference from ShmRing<T>'s SPSC ring, which has
    // exactly one shared tail because it has exactly one consumer.
    //
    // Broadcast, not queue: broadcast() unconditionally overwrites the
    // next slot and publishes it -- there is no "ring full" backpressure
    // concept here at all, since with an unbounded number of possible
    // consumers there is no single tail position that would even define
    // "full". A consumer that cannot keep up simply loses visibility into
    // records the producer has since overwritten; poll()/poll_spin()
    // detect exactly that condition (this consumer has fallen more than
    // capacity() behind head) and correct for it by jumping this
    // consumer's cursor forward to head-capacity(), accounting the
    // skipped span into overrun_count() -- the "detect the overrun,
    // calculate dropped ticks, and jump to head - capacity" contract this
    // ring exists to implement. A consumer that never falls behind (a
    // tight poll_spin() loop against a producer within this consumer's
    // own scheduling budget) never pays that correction and sees every
    // record broadcast, in order, exactly once.
    //
    // T must be trivially copyable, same reasoning as ShmRing<T>.
    template <typename T>
    class SpmcRing {
    public:
        static_assert(std::is_trivially_copyable<T>::value,
            "T must be trivially copyable -- SpmcRing<T> places T directly in shared memory "
            "with no serialization step, so a naive byte-copy across the process boundary "
            "must be a valid, complete copy of T");
        static_assert(alignof(T) <= 64,
            "T's alignment must not exceed 64 bytes -- SpmcRing<T> packs slots back-to-back with "
            "no per-slot padding beyond T's own size (see SpmcRingHeader::stride), so an "
            "over-aligned T could straddle the guarantee every slot after the first is validly "
            "aligned for it");

        SpmcRing(const SpmcRing&) = delete;
        SpmcRing& operator=(const SpmcRing&) = delete;

        // Producer-side: allocates a new named ring sized for at least
        // `requested_capacity` slots (rounded up to a power of two, same
        // masking rationale as ShmRing<T>::create()). Returns nullptr on
        // failure, including a name collision.
        static std::unique_ptr<SpmcRing> create(const char* name, size_t requested_capacity) noexcept {
            const uint64_t capacity = round_up_pow2(requested_capacity < 2 ? 2 : requested_capacity);
            const size_t total_size = sizeof(SpmcRingHeader) + static_cast<size_t>(capacity) * sizeof(T);

            SharedMemoryRegion region;
            if (!SharedMemoryRegion::create(name, total_size, region)) {
                return nullptr;
            }
            auto* header = new (region.data()) SpmcRingHeader();
            header->capacity = capacity;
            header->mask = capacity - 1;
            header->ring_kind = static_cast<uint64_t>(RingKind::SpmcBroadcast);
            header->schema_version_hash = schema::Traits<T>::kVersionHash;
            header->payload_size = static_cast<uint64_t>(sizeof(T));
            header->stride = static_cast<uint64_t>(sizeof(T));
            set_wire_format(*header, schema::Traits<T>::kWireFormat);

            std::unique_ptr<SpmcRing> ring(new SpmcRing());
            ring->region_ = std::move(region);
            ring->header_ = header;
            ring->slots_ = reinterpret_cast<T*>(header + 1);
            return ring;
        }

        // Consumer-side (call once per independent consumer -- each call
        // returns its OWN SpmcRing instance with its own local_tail_
        // starting at 0, i.e. this consumer will see the full backlog
        // still resident in the ring, self-correcting via the overrun
        // logic in poll() if the producer has already advanced past
        // capacity() by the time this consumer attaches). Also usable by
        // the producer's own process to attach a second, independent
        // read-only view of its own ring if ever needed. Returns nullptr
        // if the segment doesn't exist, is too small, is a torn/foreign
        // segment, is an SPSC ShmRing<T> segment rather than an SpmcRing<T>
        // one, or was created for a different/mismatched schema (same
        // checks as ShmRing<T>::open(), see that method's own comment).
        static std::unique_ptr<SpmcRing> open(const char* name) noexcept {
            SharedMemoryRegion region;
            if (!SharedMemoryRegion::open(name, region)) {
                return nullptr;
            }
            if (region.size() < sizeof(SpmcRingHeader)) {
                return nullptr;
            }
            auto* header = reinterpret_cast<SpmcRingHeader*>(region.data());
            if (header->ring_kind != static_cast<uint64_t>(RingKind::SpmcBroadcast)) {
                return nullptr; // this segment is an SPSC ShmRing<T>, not an SpmcRing<T> broadcast ring
            }
            const uint64_t capacity = header->capacity;
            const bool capacity_is_pow2 = capacity != 0 && (capacity & (capacity - 1)) == 0;
            if (!capacity_is_pow2 || header->mask != capacity - 1) {
                return nullptr;
            }
            if (header->payload_size != static_cast<uint64_t>(sizeof(T))) {
                return nullptr; // wrong record size -- a different T (or ABI drift) than create() used
            }
            if (header->schema_version_hash != schema::Traits<T>::kVersionHash) {
                return nullptr; // same size, different registered schema -- refuse rather than misread
            }
            if (region.size() < sizeof(SpmcRingHeader) + static_cast<size_t>(capacity) * sizeof(T)) {
                return nullptr;
            }

            std::unique_ptr<SpmcRing> ring(new SpmcRing());
            ring->region_ = std::move(region);
            ring->header_ = header;
            ring->slots_ = reinterpret_cast<T*>(header + 1);
            return ring;
        }

        static bool unlink(const char* name) noexcept { return SharedMemoryRegion::unlink(name); }

        bool valid() const noexcept { return header_ != nullptr; }
        size_t capacity() const noexcept { return header_ ? static_cast<size_t>(header_->capacity) : 0; }
        uint64_t schema_version_hash() const noexcept { return header_ ? header_->schema_version_hash : 0; }
        size_t payload_size() const noexcept { return header_ ? static_cast<size_t>(header_->payload_size) : 0; }
        size_t stride() const noexcept { return header_ ? static_cast<size_t>(header_->stride) : 0; }
        const char* wire_format() const noexcept { return header_ ? header_->wire_format : ""; }
        static constexpr const char* schema_name() noexcept { return schema::Traits<T>::kName; }

        // Producer-side only (one process). Never blocks and never
        // refuses: unconditionally writes into the next slot and
        // publishes it via a release-store to the shared head, then
        // advances this producer's own locally-tracked write position.
        // producer_head_ is a plain (non-atomic) member because the
        // producer is the sole writer of it -- there is no reason to pay
        // an atomic load on every call just to read back a value only
        // this same call sequence ever wrote; header_->head itself is
        // still a proper atomic release-store, which is what every
        // consumer's acquire-load actually synchronizes against.
        void broadcast(const T& value) noexcept {
            slots_[producer_head_ & header_->mask] = value;
            header_->head.store(producer_head_ + 1, std::memory_order_release);
            ++producer_head_;
        }

        void mark_producer_attached() noexcept {
            header_->producer_pid.store(sys::lifecycle::current_process_id(), std::memory_order_release);
            header_->producer_heartbeat.store(1, std::memory_order_release);
        }
        void producer_heartbeat() noexcept {
            header_->producer_heartbeat.fetch_add(1, std::memory_order_relaxed);
        }
        // Same semantics as ShmRing<T>::is_producer_alive -- see that
        // method's own comment. There is no is_consumer_alive() here: with
        // broadcast fan-out to an arbitrary number of anonymous consumers,
        // there is no single shared consumer identity for the producer to
        // publish or for any one consumer to check on another's behalf.
        bool is_producer_alive(uint64_t stale_after = 0) const noexcept {
            return peer_alive(header_->producer_pid, header_->producer_heartbeat,
                               last_seen_producer_heartbeat_, producer_stall_count_, stale_after);
        }

        // Consumer-side. Copies up to max_count records into `out`
        // (caller-owned buffer of at least max_count T's), starting from
        // THIS SpmcRing instance's own local_tail_ -- never shared with
        // any other consumer or the producer. Returns the number of
        // records actually copied (0 if this consumer has already caught
        // up to the producer's current head; never blocks).
        //
        // Overrun handling: if head has advanced more than capacity()
        // past local_tail_, the producer has already overwritten every
        // slot this consumer hasn't read yet -- rather than reading stale
        // or torn data, this jumps local_tail_ forward to head-capacity()
        // and accounts the skipped span (new_tail - old local_tail_) into
        // overrun_count(). A consumer that never falls behind never
        // triggers this branch and pays only the one comparison's cost.
        //
        // Torn-read note, same honest tradeoff ShmRing<T>::push_overwrite
        // already documents for its own overwrite mode: the overrun check
        // above uses one head snapshot taken at the start of this call: if
        // an extremely fast producer advances head by another full
        // capacity() or more DURING this call's copy loop (not just before
        // it), a slot already validated as "within capacity() of head" at
        // the top of this function could still be overwritten out from
        // under the copy that follows. This is bounded by how much a
        // single poll()/poll_spin() call can fall behind within its own
        // duration, not by how far this consumer has fallen behind overall
        // -- and is the same class of race push_overwrite() already
        // accepts, not a new one. Pair poll() with a consumer that
        // tolerates an occasional torn record at that boundary; a
        // consumer that must never observe one needs a bounded-
        // backpressure primitive (ShmRing<T>::try_push/push_spin) instead,
        // which broadcast fan-out to multiple readers cannot offer by
        // construction.
        size_t poll(T* out, size_t max_count) noexcept {
            const uint64_t head = header_->head.load(std::memory_order_acquire);
            last_poll_overran_ = false;
            if (head - local_tail_ > header_->capacity) {
                const uint64_t new_tail = head - header_->capacity;
                overrun_count_ += (new_tail - local_tail_);
                local_tail_ = new_tail;
                last_poll_overran_ = true;
            }
            size_t n = 0;
            while (n < max_count && local_tail_ < head) {
                out[n++] = slots_[local_tail_ & header_->mask];
                ++local_tail_;
            }
            return n;
        }

        // Spin-polling variant: retries poll() with animus::cpu_relax()
        // between empty attempts (same fallback convention as
        // ShmRing<T>::pop_spin) until at least one record is copied or
        // max_spins attempts have been made. Still never blocks in the
        // OS-scheduler sense.
        size_t poll_spin(T* out, size_t max_count, uint64_t max_spins = kDefaultMaxSpins) noexcept {
            for (uint64_t i = 0; i < max_spins; ++i) {
                const size_t n = poll(out, max_count);
                if (n > 0) return n;
                cpu_relax();
            }
            return 0;
        }

        // Cumulative count of records THIS consumer has been forced to
        // skip because it fell more than capacity() behind head -- the
        // "dropped ticks" broadcast lossy mode calls for. Always 0 for a
        // consumer that never falls behind. Purely local to this SpmcRing
        // instance, like local_tail_ itself -- never visible to the
        // producer or to any other consumer.
        uint64_t overrun_count() const noexcept { return overrun_count_; }

        // True if the MOST RECENT poll()/poll_spin() call detected and
        // corrected an overrun -- the transient "overrun flag" companion
        // to the cumulative overrun_count() above, for a caller that wants
        // to react to a fresh overrun specifically (e.g. log it once)
        // rather than only monitor the running total.
        bool last_poll_overran() const noexcept { return last_poll_overran_; }

        // This consumer's own current read position -- exposed mainly for
        // diagnostics/tests; not meaningful to compare across different
        // SpmcRing instances (including two opened by the same process),
        // since each tracks its own independently.
        uint64_t local_tail() const noexcept { return local_tail_; }

        // The producer's current published position -- a fresh acquire-load
        // each call, exposed mainly for diagnostics/tests.
        uint64_t head() const noexcept { return header_->head.load(std::memory_order_acquire); }

    private:
        // Identical helper to ShmRing<T>::peer_alive -- kept as its own
        // copy rather than shared, consistent with SharedMemoryRegion
        // being the only piece of implementation this file shares between
        // the SPSC and SPMC ring kinds; the two headers' layouts are
        // different enough (see SpmcRingHeader's own comment) that little
        // else safely generalizes across both without a template
        // parameter for the header type, which isn't worth it for one
        // small function.
        static bool peer_alive(const std::atomic<uint64_t>& pid_field,
                               const std::atomic<uint64_t>& heartbeat_field,
                               uint64_t& last_seen, uint64_t& stall_count, uint64_t stale_after) noexcept {
            const uint64_t pid = pid_field.load(std::memory_order_acquire);
            if (pid == 0) return true; // peer hasn't attached yet -- not a death
            if (!sys::lifecycle::is_process_alive(pid)) return false;
            if (stale_after == 0) return true;
            const uint64_t hb = heartbeat_field.load(std::memory_order_acquire);
            if (hb != last_seen) {
                last_seen = hb;
                stall_count = 0;
                return true;
            }
            return ++stall_count < stale_after;
        }

        mutable uint64_t last_seen_producer_heartbeat_ = 0;
        mutable uint64_t producer_stall_count_ = 0;

        SpmcRing() noexcept = default;

        static constexpr uint64_t kDefaultMaxSpins = 200'000'000ull;

        static uint64_t round_up_pow2(size_t v) noexcept {
            uint64_t p = 1;
            while (p < v) p <<= 1;
            return p;
        }

        SharedMemoryRegion region_;
        SpmcRingHeader* header_ = nullptr;
        T* slots_ = nullptr;
        uint64_t producer_head_ = 0; // producer-side-only local cache of its own head; unused by a consumer instance
        uint64_t local_tail_ = 0;    // THIS consumer's own read cursor -- never written to shared memory
        uint64_t overrun_count_ = 0;
        bool last_poll_overran_ = false;
    };

} // namespace ipc
} // namespace sys
} // namespace animus
