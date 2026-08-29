#pragma once
// =========================================================================
// animus_release.hpp -- Animus Core v1.0 single-header release
//
// GENERATED FILE -- do not edit directly. Produced by amalgamate.py from
// the four source headers below; re-run `python amalgamate.py` after any
// change to those originals and commit the regenerated output alongside.
// Generated: 2026-08-30
//
// Sections:
//   - animus.hpp (portable) -- Phase 1-7: Core Engine, Ring Buffer, Rule Engine, Broker/Execution Interop
//   - animus_security.hpp (portable) -- Phase 8: RBAC + Multi-Tenant Isolation
//   - animus_transport.hpp (Windows-only) -- Phase 8: mTLS / TLS 1.3 Transport (Schannel)
//   - animus_cluster.hpp (Windows-only) -- Phase 9: Distributed Cloud Orchestration & Clustering (Raft-lite over mTLS)
//
// The Windows-only sections (mTLS transport, Raft-lite clustering) are
// wrapped in `#if defined(_WIN32)` below, so a non-Windows translation
// unit can still #include this single file and use the portable core
// engine + RBAC/multi-tenancy layer; animus::transport / animus::cluster
// are simply unavailable there, matching what the standalone headers'
// own `#error` guards already document.
// =========================================================================


// -------------------------------------------------------------------------
// animus.hpp -- Phase 1-7: Core Engine, Ring Buffer, Rule Engine, Broker/Execution Interop
// -------------------------------------------------------------------------
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <atomic>
#include <vector>
#include <deque>
#include <chrono>
#include <thread>
#include <fstream>
#include <mutex>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

// Cross-platform named shared-memory mapping, used by SharedMemorySegment
// below. Unlike animus_transport.hpp's Schannel includes (a large,
// Windows-only dependency chain deliberately kept out of this portable
// core header), shared memory is both genuinely cross-platform and a
// small enough API surface (four-ish functions per platform) that it
// belongs directly in animus.hpp alongside SpscRingBuffer -- the same
// "core, portable, header-only" bar the rest of this file holds itself
// to, unlike animus_pin_current_thread_to_core (see animus_engine.cpp),
// which stayed DLL-only specifically because windows.h's much larger
// footprint wasn't worth it for two OS calls.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace animus {

    // Strictly aligned to 64 bytes to match standard CPU L1/L2 cache line boundaries
    struct alignas(64) TelemetryPayload {
        uint64_t timestamp_cycles;
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
    };

    // Emitted when a registered rule matches an ingested telemetry event.
    // Deliberately NOT cache-line padded like TelemetryPayload: this struct
    // is copied directly across the C-ABI via poll_signals(ThreatSignal*, ...),
    // so its layout must be exactly what a caller's own struct definition
    // (e.g. a ctypes Structure) naturally produces -- 32 bytes, 8-byte
    // aligned. Padding it to 64 bytes here without the caller mirroring that
    // padding would silently corrupt the caller's buffer.
    struct ThreatSignal {
        uint64_t timestamp_cycles;
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
        uint32_t rule_id;
        uint32_t severity;
    };

    // Plain input record for batched ingestion (animus_record_events_batch):
    // just the caller-supplied fields, no timestamp -- record_batch() stamps
    // each one with read_cycle_counter() itself, same as record(). 16 bytes,
    // naturally aligned (no padding), so it mirrors a ctypes Structure with
    // matching field order directly.
    struct RawEvent {
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
    };

    // Comparator applied between an event's metric_value and a rule's threshold.
    enum class RuleComparator : uint8_t {
        GreaterThan = 0,
        LessThan = 1,
        Equal = 2,
    };

    // Declarative threshold rule: matches telemetry events carrying a given
    // event_id whose metric_value satisfies `comparator` against `threshold`.
    // Covers both threat-detection thresholds (e.g. anomalous rate spikes)
    // and trading-signal thresholds (e.g. price/volume breakouts) uniformly.
    struct RuleThreshold {
        uint32_t rule_id;
        uint32_t event_id;
        uint64_t threshold;
        RuleComparator comparator;
        uint32_t severity;
    };

    inline bool compare_value(uint64_t lhs, uint64_t rhs, RuleComparator comparator) noexcept {
        switch (comparator) {
        case RuleComparator::GreaterThan: return lhs > rhs;
        case RuleComparator::LessThan:    return lhs < rhs;
        case RuleComparator::Equal:       return lhs == rhs;
        }
        return false;
    }

    // ---- Complex Event Processing (CEP): sliding-window aggregation -------
    // A RuleThreshold (above) evaluates one event in isolation. A CEP rule
    // instead maintains a rolling window of recent metric_values for a given
    // event_id and evaluates an *aggregate* (SUM/AVG/MIN/MAX) of that window
    // against a threshold on every new matching event -- "average order size
    // over the last 500ms exceeds X", not just "this one order's size does".

    // Count windows hold the last N matching events; time windows hold
    // events whose timestamp is within the last window_size milliseconds of
    // whenever the rule was last fed an event.
    enum class WindowType : uint8_t {
        Count = 0,
        Time = 1,
    };

    enum class AggregationFunction : uint8_t {
        Sum = 0,
        Avg = 1,
        Min = 2,
        Max = 3,
    };

    // One value in a CEP rule's window: the value itself, plus everything
    // needed to know when it should be evicted -- `sequence` for count
    // windows (compared against the rule's own running event counter, not a
    // deque size, since MIN/MAX's monotonic deques below hold only a subset
    // of raw entries) and `timestamp_ms` for time windows.
    struct CepWindowEntry {
        uint64_t sequence;
        uint64_t timestamp_ms;
        uint64_t value;
    };

    // Sliding-window aggregate + threshold check for one CEP rule, fed one
    // event at a time via on_event(). Not thread-safe by design: this class
    // holds mutable per-rule state (the window contents) that only the
    // single persistence-worker thread ever touches (see
    // EngineImpl::evaluate_cep_rules) -- concurrent producer threads only
    // ever read the *set* of registered rules (copy-on-write, like
    // RuleThreshold's rules_), never a given rule's window state directly.
    //
    // SUM/AVG maintain a plain FIFO of every value currently in the window
    // plus a running total, updated in O(1) amortized per event (subtract
    // the evicted value on eviction, rather than resumming the window).
    // AVG's threshold check cross-multiplies (`sum COMPARATOR threshold *
    // count`) instead of dividing, so the comparison stays exact integer
    // arithmetic with no floating point anywhere in the hot path -- the
    // reported aggregated value (avg = sum / count) still uses integer
    // division, i.e. floored, consistent with every other uint64_t field
    // this engine reports.
    //
    // MIN/MAX each maintain a monotonic deque of *candidate* values (a
    // subset of the raw window, not every entry) in increasing (MIN) or
    // decreasing (MAX) order, so the window's minimum/maximum is always
    // the front element -- O(1) amortized per event, the standard
    // sliding-window-minimum/maximum algorithm. Verified against a naive
    // full-rescan reference over 200 randomized trials (both window types,
    // all four aggregations) before this was wired into the engine proper.
    class CepRuleState {
    public:
        CepRuleState(uint32_t rule_id, uint32_t event_id, WindowType window_type, uint64_t window_size,
            AggregationFunction agg, RuleComparator comparator, uint64_t threshold, uint32_t severity) noexcept
            : rule_id_(rule_id), event_id_(event_id), window_type_(window_type),
            window_size_(window_size == 0 ? 1 : window_size), agg_(agg),
            comparator_(comparator), threshold_(threshold), severity_(severity) {
        }

        uint32_t rule_id() const noexcept { return rule_id_; }
        uint32_t event_id() const noexcept { return event_id_; }
        uint32_t severity() const noexcept { return severity_; }

        // Feeds one matching event's value into this rule's window, evicts
        // anything that has fallen out of it, and returns {matched,
        // aggregated_value}. `now_ms` is supplied by the caller (read once
        // per persistence-worker batch, not once per event -- see
        // process_persistence_queue) rather than read internally here, both
        // to avoid a clock read on every single event and to keep this
        // class deterministically testable with synthetic timestamps.
        std::pair<bool, uint64_t> on_event(uint64_t value, uint64_t now_ms) noexcept {
            ++sequence_counter_;
            CepWindowEntry entry{ sequence_counter_, now_ms, value };

            uint64_t aggregated = 0;
            bool matched = false;

            switch (agg_) {
            case AggregationFunction::Sum:
            case AggregationFunction::Avg: {
                sum_window_.push_back(entry);
                running_sum_ += entry.value;
                evict_fifo(now_ms);
                uint64_t count = sum_window_.size();
                if (agg_ == AggregationFunction::Sum) {
                    aggregated = running_sum_;
                    matched = compare_value(running_sum_, threshold_, comparator_);
                }
                else {
                    aggregated = count ? running_sum_ / count : 0;
                    matched = compare_value(running_sum_, threshold_ * count, comparator_);
                }
                break;
            }
            case AggregationFunction::Max: {
                while (!max_monotonic_.empty() && max_monotonic_.back().value <= entry.value) {
                    max_monotonic_.pop_back();
                }
                max_monotonic_.push_back(entry);
                evict_monotonic(max_monotonic_, now_ms);
                aggregated = max_monotonic_.front().value;
                matched = compare_value(aggregated, threshold_, comparator_);
                break;
            }
            case AggregationFunction::Min: {
                while (!min_monotonic_.empty() && min_monotonic_.back().value >= entry.value) {
                    min_monotonic_.pop_back();
                }
                min_monotonic_.push_back(entry);
                evict_monotonic(min_monotonic_, now_ms);
                aggregated = min_monotonic_.front().value;
                matched = compare_value(aggregated, threshold_, comparator_);
                break;
            }
            }
            return { matched, aggregated };
        }

    private:
        void evict_fifo(uint64_t now_ms) noexcept {
            if (window_type_ == WindowType::Count) {
                while (sum_window_.size() > window_size_) {
                    running_sum_ -= sum_window_.front().value;
                    sum_window_.pop_front();
                }
            }
            else {
                while (!sum_window_.empty() && sum_window_.front().timestamp_ms + window_size_ <= now_ms) {
                    running_sum_ -= sum_window_.front().value;
                    sum_window_.pop_front();
                }
            }
        }

        void evict_monotonic(std::deque<CepWindowEntry>& q, uint64_t now_ms) const noexcept {
            if (window_type_ == WindowType::Count) {
                while (!q.empty() && q.front().sequence + window_size_ <= sequence_counter_) {
                    q.pop_front();
                }
            }
            else {
                while (!q.empty() && q.front().timestamp_ms + window_size_ <= now_ms) {
                    q.pop_front();
                }
            }
        }

        uint32_t rule_id_;
        uint32_t event_id_;
        WindowType window_type_;
        uint64_t window_size_; // event count, or milliseconds -- never 0 (see constructor)
        AggregationFunction agg_;
        RuleComparator comparator_;
        uint64_t threshold_;
        uint32_t severity_;

        uint64_t sequence_counter_ = 0;
        std::deque<CepWindowEntry> sum_window_;   // used when agg_ is Sum or Avg
        uint64_t running_sum_ = 0;
        std::deque<CepWindowEntry> min_monotonic_; // used when agg_ is Min
        std::deque<CepWindowEntry> max_monotonic_; // used when agg_ is Max
    };

    // Low-overhead cycle counter for hot-path timestamping. Falls back to a
    // monotonic clock on platforms without an invariant TSC intrinsic.
    inline uint64_t read_cycle_counter() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__)
        return __builtin_ia32_rdtsc();
#else
        return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

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

        alignas(64) std::atomic<size_t> enqueue_pos_{ 0 };
        alignas(64) std::atomic<size_t> dequeue_pos_{ 0 };
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

        alignas(64) std::atomic<size_t> head_{ 0 }; // written only by the producer
        alignas(64) std::atomic<size_t> tail_{ 0 }; // written only by the consumer
    };

    // ---- IPC shared-memory transport ---------------------------------
    // A separate producer process and consumer process exchanging
    // telemetry with no serialization step and no DLL-static-state
    // barrier between them: ctypes gives each process its own copy of
    // AnimusCore_v1.dll's g_engine, so animus_record_event() in one
    // process is invisible to another -- these classes exist to give two
    // *different* processes a channel that genuinely is the same memory
    // on both sides.
    //
    // RAII wrapper over a named OS shared-memory mapping: Windows
    // CreateFileMappingA/MapViewOfFile, POSIX shm_open/mmap. One process
    // calls create() to allocate and own the segment; any other process
    // on the same machine calls attach() with the same name to map the
    // identical physical pages -- writes on one side are visible to
    // reads on the other with no copy, no syscall, no kernel round trip
    // once mapped.
    class SharedMemorySegment {
    public:
        SharedMemorySegment() noexcept = default;
        ~SharedMemorySegment() { close(); }
        SharedMemorySegment(const SharedMemorySegment&) = delete;
        SharedMemorySegment& operator=(const SharedMemorySegment&) = delete;

        SharedMemorySegment(SharedMemorySegment&& other) noexcept { *this = std::move(other); }
        SharedMemorySegment& operator=(SharedMemorySegment&& other) noexcept {
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
        static bool create(const char* name, size_t size, SharedMemorySegment& out) noexcept {
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
        static bool attach(const char* name, SharedMemorySegment& out) noexcept {
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
        // mapping, same contract as Python's SharedMemory.unlink()
        // (animus/shm.py). Windows: named file mappings have no separate
        // "unlink" step -- the OS object is destroyed automatically once
        // the last HANDLE to it (this process's and every attached
        // process's) closes, so this is a documented no-op there, not a
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

    // Wire-compatible with animus.shm.SharedTelemetryRing's Python-only
    // implementation (_RECORD_FORMAT = "<QIIQ" there) -- deliberately, so
    // a pure-Python producer/consumer and a native one can interoperate
    // on the very same shared-memory segment. 24 bytes, naturally
    // aligned, no padding.
    struct SharedTelemetryRecord {
        uint64_t timestamp_cycles;
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
    };
    static_assert(sizeof(SharedTelemetryRecord) == 24,
        "must match animus.shm's <QIIQ> wire record exactly for cross-language interop");

    // Wire-compatible with animus.shm's header (_HEADER_FORMAT = "<QQQ">):
    // capacity, head, tail, all uint64, tightly packed -- deliberately
    // NOT cache-line-padded between head/tail the way SpscRingBuffer's
    // in-process head_/tail_ are, since padding would break byte-for-byte
    // compatibility with the existing Python wire format for a false-
    // sharing optimization that only matters at extreme sustained
    // throughput, not for correctness. std::atomic<uint64_t> only
    // requires 8-byte alignment to be lock-free, which three consecutive
    // 8-byte fields already provide.
    struct SharedRingHeader {
        uint64_t capacity;
        std::atomic<uint64_t> head; // producer-only writes
        std::atomic<uint64_t> tail; // consumer-only writes
    };
    static_assert(sizeof(SharedRingHeader) == 24,
        "must match animus.shm's <QQQ> wire header exactly for cross-language interop");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "SharedRingHeader is placed in memory shared across processes -- a non-lock-free "
        "uint64_t atomic could fall back to a mutex/futex that isn't valid across process boundaries");

    // Single-producer/single-consumer telemetry ring living entirely
    // inside a SharedMemorySegment -- same head/tail algorithm as
    // SpscRingBuffer above, but placement-based (the ring's storage is
    // the mapped segment, not an owned std::vector) since two different
    // OS processes, not two threads in one process, are what push()/
    // pop() need to stay safe across here. Only ever constructed via
    // create()/attach(); the private default constructor plus these
    // named factories mirrors AnimusCore_v1's other native primitives
    // (Engine::Create, AnimusBindings.spsc_init).
    class SharedTelemetryChannel {
    public:
        SharedTelemetryChannel(const SharedTelemetryChannel&) = delete;
        SharedTelemetryChannel& operator=(const SharedTelemetryChannel&) = delete;

        // Allocates a new named segment sized for exactly `capacity`
        // records -- NOT rounded up to a power of two, unlike
        // SpscRingBuffer/LockFreeRingBuffer above. Those use `& (capacity
        // - 1)` for slot indexing, which requires a power-of-two capacity;
        // this uses plain `%` instead (see push()/pop()) specifically so
        // capacity can be any value animus.shm.SharedTelemetryRing.create()
        // was given, since the wire-compatibility this class is built for
        // only holds if both sides agree on where record N actually lives
        // -- a bitmask would silently index the wrong slot against a
        // segment the Python side created with a non-power-of-two
        // capacity. Returns nullptr on failure (including a name
        // collision -- see SharedMemorySegment::create).
        static std::unique_ptr<SharedTelemetryChannel> create(const char* name, uint64_t capacity) noexcept {
            if (capacity == 0) capacity = 1;

            SharedMemorySegment segment;
            size_t total_size = sizeof(SharedRingHeader) + static_cast<size_t>(capacity) * sizeof(SharedTelemetryRecord);
            if (!SharedMemorySegment::create(name, total_size, segment)) {
                return nullptr;
            }
            auto* header = new (segment.data()) SharedRingHeader{}; // placement-new: zero-init head/tail
            header->capacity = capacity;

            std::unique_ptr<SharedTelemetryChannel> channel(new SharedTelemetryChannel());
            channel->segment_ = std::move(segment);
            channel->header_ = header;
            channel->records_ = reinterpret_cast<SharedTelemetryRecord*>(header + 1);
            return channel;
        }

        // Maps an existing segment created by another process's create()
        // call. Returns nullptr if the segment doesn't exist or is too
        // small to hold a valid header.
        static std::unique_ptr<SharedTelemetryChannel> attach(const char* name) noexcept {
            SharedMemorySegment segment;
            if (!SharedMemorySegment::attach(name, segment)) {
                return nullptr;
            }
            if (segment.size() < sizeof(SharedRingHeader)) {
                return nullptr;
            }
            auto* header = reinterpret_cast<SharedRingHeader*>(segment.data());
            if (header->capacity == 0 ||
                segment.size() < sizeof(SharedRingHeader) + static_cast<size_t>(header->capacity) * sizeof(SharedTelemetryRecord)) {
                return nullptr; // header claims a capacity the mapped region doesn't actually have room for
            }

            std::unique_ptr<SharedTelemetryChannel> channel(new SharedTelemetryChannel());
            channel->segment_ = std::move(segment);
            channel->header_ = header;
            channel->records_ = reinterpret_cast<SharedTelemetryRecord*>(header + 1);
            return channel;
        }

        static bool unlink(const char* name) noexcept { return SharedMemorySegment::unlink(name); }

        bool valid() const noexcept { return segment_.valid(); }
        uint64_t capacity() const noexcept { return header_ ? header_->capacity : 0; }

        // Producer-side only (one process/thread). Never blocks; returns
        // false if the ring is full. Stamps timestamp_cycles itself via
        // read_cycle_counter(), same as Engine::record() -- unlike
        // animus.shm.SharedTelemetryRing.push(), there is no caller-
        // supplied timestamp override; this is a native hot-path primitive,
        // not a general-purpose testing shim.
        bool push(uint32_t event_id, uint32_t trace_id, uint64_t metric_value) noexcept {
            if (!header_) return false;
            const uint64_t head = header_->head.load(std::memory_order_relaxed);
            const uint64_t tail = header_->tail.load(std::memory_order_acquire);
            if (head - tail >= header_->capacity) return false;
            records_[head % header_->capacity] = SharedTelemetryRecord{
                read_cycle_counter(), event_id, trace_id, metric_value };
            header_->head.store(head + 1, std::memory_order_release);
            return true;
        }

        // Consumer-side only (one process/thread). Never blocks; returns
        // false if the ring is empty.
        bool pop(SharedTelemetryRecord& out) noexcept {
            if (!header_) return false;
            const uint64_t tail = header_->tail.load(std::memory_order_relaxed);
            const uint64_t head = header_->head.load(std::memory_order_acquire);
            if (tail == head) return false;
            out = records_[tail % header_->capacity];
            header_->tail.store(tail + 1, std::memory_order_release);
            return true;
        }

        void close() noexcept {
            segment_.close();
            header_ = nullptr;
            records_ = nullptr;
        }

    private:
        SharedTelemetryChannel() noexcept = default;

        SharedMemorySegment segment_;
        SharedRingHeader* header_ = nullptr;
        SharedTelemetryRecord* records_ = nullptr;
    };

    // ---- Offline license verification (proprietary-edition only) --------
    // Validates this machine against an RSA-2048-signed license file at
    // startup, entirely offline (no network call, ever), and gates
    // animus_pin_current_thread_to_core / animus_spsc_init to the
    // license's entitled core count -- see animus_verify_license and the
    // two gated functions in animus_engine.cpp, the only place any of
    // this is actually implemented (Windows-only: BCrypt/CNG for RSA
    // verification and SHA-256, the registry for a stable per-install
    // machine identifier, iphlpapi for a MAC address -- same "heavy
    // platform API stays in the .cpp shim, not this portable header"
    // split animus_pin_current_thread_to_core already established).
    //
    // "CPU GUID": modern x86 CPUs do not expose a true unique-per-chip
    // serial number via CPUID for privacy reasons (Intel deprecated the
    // Pentium III PSN two decades ago). The honestly-labeled stand-in
    // every real license-locking tool on Windows actually uses instead
    // -- and what this implementation reads -- is the OS-assigned
    // MachineGuid (HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid), a
    // GUID generated once at Windows setup and stable for the life of
    // that install. Combined with a real network adapter's burned-in MAC
    // address (see animus_engine.cpp for why "first enumerated adapter"
    // is not a safe way to pick one -- multiple legitimate MACs can
    // exist on one machine) and SHA-256 hashed, this is the fingerprint
    // a license is issued against.
    //
    // 64 bytes on disk: this struct's natural layout (60 real bytes) plus
    // 4 bytes of trailing alignment padding the uint64_t members force,
    // read directly off the license file -- confirmed via a real
    // sizeof()/offsetof() build (60 logical bytes -> 64 actual) before
    // this was relied on anywhere, not assumed from the field list.
    // On disk: [this 64-byte struct][256-byte RSA-2048/PKCS1/SHA-256
    // signature over exactly those 64 bytes] = 320 bytes total.
    struct LicensePayload {
        uint32_t magic;              // kLicenseMagic -- sanity-checks the file before touching the signature
        uint32_t version;
        uint64_t issued_at_unix;     // informational only, not enforced
        uint64_t expires_at_unix;    // 0 = no expiry
        uint32_t max_cores;          // entitled core count
        uint8_t  fingerprint_sha256[32]; // SHA-256(MachineGuid UTF-8 bytes || 6 raw MAC address bytes)
    };
    static_assert(sizeof(LicensePayload) == 64, "LicensePayload's on-disk size changed -- update license_tools/sign_license.ps1 to match");

    constexpr uint32_t kLicenseMagic = 0x434C4E41; // 'ANLC', matches sign_license.ps1's $magic
    constexpr size_t kLicenseSignatureSize = 256;  // RSA-2048 signature size in bytes
    constexpr size_t kLicenseFileSize = sizeof(LicensePayload) + kLicenseSignatureSize; // 320

    class Engine {
    public:
        virtual ~Engine() = default;
        virtual bool record(uint32_t event_id, uint32_t trace_id, uint64_t value) const noexcept = 0;

        // Pushes a contiguous batch of events in one call, amortizing the
        // per-call ctypes/C-ABI marshalling cost across the whole batch
        // instead of paying it per event. Stops at the first push that
        // fails (ring buffer full, since there is no concurrent consumer
        // freeing space mid-call) rather than skipping ahead, so the
        // return value also tells the caller exactly how many of `events`,
        // in order, were actually ingested. Returns the number pushed.
        virtual size_t record_batch(const RawEvent* events, size_t count) const noexcept = 0;

        virtual bool is_guard_active() const noexcept = 0;
        virtual void start_persistence(const std::string& log_filepath) = 0;
        virtual void stop_persistence() = 0;

        // Registers a threshold rule evaluated in-memory against every
        // ingested event (see EngineImpl::evaluate_rules). Returns false for
        // an invalid comparator value or if rule storage could not be grown.
        virtual bool add_rule(uint32_t rule_id, uint32_t event_id, uint64_t threshold, uint8_t comparator, uint32_t severity) noexcept = 0;

        // Registers a CEP sliding-window rule (see CepRuleState above):
        // aggregates matching events' metric_value over a count- or
        // time-based window (window_type: 0=Count, window_size=event count;
        // 1=Time, window_size=milliseconds) via aggregation (0=Sum, 1=Avg,
        // 2=Min, 3=Max), and evaluates the aggregate against threshold on
        // every matching event, same comparator encoding as add_rule.
        // Evaluated on the same persistence-worker thread and delivered
        // through the same poll_signals() queue as plain RuleThreshold
        // matches -- callers distinguish which rule fired via rule_id, not
        // a separate queue. Returns false for an invalid window_type,
        // aggregation, or comparator value.
        virtual bool add_cep_rule(uint32_t rule_id, uint32_t event_id, uint8_t window_type, uint64_t window_size,
            uint8_t aggregation, uint8_t comparator, uint64_t threshold, uint32_t severity) noexcept = 0;

        // Drains up to max_count pending signals into the caller-owned `out`
        // buffer (zero-copy for the caller: no allocation, direct struct
        // copy). Returns the number of signals actually written.
        virtual size_t poll_signals(ThreatSignal* out, size_t max_count) noexcept = 0;

        static std::unique_ptr<Engine> Create(size_t buffer_capacity = 65536);
    };

    // -----------------------------------------------------------------------
    // Header-only engine implementation
    // -----------------------------------------------------------------------
    // Defined directly in this header, with every method `inline`, so that
    // animus.hpp is a genuine zero-dependency, single-header C++ library:
    // any C++17 translation unit can #include "animus.hpp" and call
    // animus::Engine::Create() to get a fully working in-process engine,
    // with no separate .cpp to compile and no AnimusCore_v1.dll to build or
    // link against. `inline` on a class's member functions defined outside
    // the class (and on Engine::Create's out-of-line definition below) is
    // what makes this safe to include from multiple translation units in
    // the same program without violating the One Definition Rule -- the
    // linker collapses the duplicate definitions rather than erroring.
    //
    // The DLL build used by the Python SDK (animus_engine.cpp, compiled
    // with ANIMUS_EXPORTS) does nothing more than #include this same header
    // and re-expose Engine::Create()'s functionality across the C-ABI below
    // -- it is a thin shim over this implementation, not a separate one.
    // A C++ caller that needs to avoid the ctypes/C-ABI marshalling cost
    // entirely (e.g. an execution path with a hard latency budget) can
    // skip the DLL altogether and drive animus::Engine directly.
    class EngineImpl final : public Engine {
    private:
        // Lock-free MPMC ring buffer: safe for concurrent producer threads
        // paired with the single background persistence consumer below.
        mutable LockFreeRingBuffer<TelemetryPayload> ring_;

        // Lock-free output queue for rule matches. Written only by the
        // persistence worker thread (single producer), drained by whatever
        // thread calls poll_signals(); push() never blocks, so a slow/absent
        // signal consumer cannot stall ingestion or persistence.
        mutable LockFreeRingBuffer<ThreatSignal> signal_ring_;

        // Asynchronous Disk Worker Controls
        std::atomic<bool> running_{ false };
        std::thread worker_thread_;
        std::string log_file_path_;

        // Rule set is read once per batch (not per event) by the persistence
        // worker, so a plain mutex guarding an immutable-snapshot swap is
        // simpler than a lock-free structure and adds negligible overhead --
        // add_rule() is a cold path relative to the ~1024-event batch cadence.
        mutable std::mutex rules_mutex_;
        std::shared_ptr<const std::vector<RuleThreshold>> rules_;

        // Zero-copy rule evaluation: takes the telemetry payload and the
        // current rule snapshot by reference (no copy of the event stream or
        // rule set), and pushes a compact ThreatSignal for every match. Runs
        // inline on the persistence worker's already-materialized batch, so
        // no additional buffering or thread hand-off is needed.
        void evaluate_rules(const TelemetryPayload& payload, const std::vector<RuleThreshold>& rules) noexcept {
            for (const RuleThreshold& rule : rules) {
                if (rule.event_id != payload.event_id) continue;

                bool matched = compare_value(payload.metric_value, rule.threshold, rule.comparator);

                if (matched) {
                    ThreatSignal signal{
                        payload.timestamp_cycles,
                        payload.event_id,
                        payload.trace_id,
                        payload.metric_value,
                        rule.rule_id,
                        rule.severity
                    };
                    signal_ring_.push(signal); // never blocks; drops if the signal ring is saturated
                }
            }
        }

        // CEP rule set: same copy-on-write immutable-snapshot pattern as
        // rules_ above (add_cep_rule() is a cold path; the persistence
        // worker reads one snapshot per batch, not per event) -- but unlike
        // RuleThreshold, each CepRuleState carries mutable window state
        // that only the persistence worker thread ever touches after
        // construction. The vector's *membership* is copy-on-write
        // (concurrent add_cep_rule calls never race the worker's reads of
        // it); a given rule's *window contents* are not, because nothing
        // but the worker thread ever writes them -- shared_ptr<CepRuleState>
        // (not shared_ptr<const ...>) is deliberate here for exactly that
        // reason.
        mutable std::mutex cep_rules_mutex_;
        std::shared_ptr<const std::vector<std::shared_ptr<CepRuleState>>> cep_rules_;

        void evaluate_cep_rules(const TelemetryPayload& payload,
            const std::vector<std::shared_ptr<CepRuleState>>& cep_rules, uint64_t now_ms) noexcept {
            for (const auto& rule_ptr : cep_rules) {
                CepRuleState& rule = *rule_ptr;
                if (rule.event_id() != payload.event_id) continue;

                auto [matched, aggregated_value] = rule.on_event(payload.metric_value, now_ms);
                if (matched) {
                    ThreatSignal signal{
                        payload.timestamp_cycles,
                        payload.event_id,
                        payload.trace_id,
                        aggregated_value, // the window's aggregate, not the raw event's metric_value
                        rule.rule_id(),
                        rule.severity()
                    };
                    signal_ring_.push(signal);
                }
            }
        }

        // Spool loop: pop() and push() are both lock-free/non-blocking, so this
        // thread never contends with producer threads for a lock. If the sink
        // file can't be opened (bad path, permissions), events are left in the
        // ring buffer rather than silently discarded -- the buffer will simply
        // fill and record() will start returning false until the sink recovers
        // or shutdown is requested.
        void process_persistence_queue() {
            constexpr size_t BATCH_SIZE = 1024;
            constexpr auto OPEN_RETRY_INTERVAL = std::chrono::milliseconds(200);
            std::vector<TelemetryPayload> batch;
            batch.reserve(BATCH_SIZE);
            TelemetryPayload item;

            std::ofstream log_file;
            auto try_open = [&]() {
                log_file.open(log_file_path_, std::ios::out | std::ios::app | std::ios::binary);
                return log_file.is_open();
            };

            bool sink_ready = try_open();
            auto last_open_attempt = std::chrono::steady_clock::now();

            for (;;) {
                if (!sink_ready) {
                    if (!running_.load(std::memory_order_acquire)) {
                        break; // stop requested before a sink was ever available
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_open_attempt >= OPEN_RETRY_INTERVAL) {
                        sink_ready = try_open();
                        last_open_attempt = now;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    continue;
                }

                while (batch.size() < BATCH_SIZE && ring_.pop(item)) {
                    batch.push_back(item);
                }

                if (!batch.empty()) {
                    std::shared_ptr<const std::vector<RuleThreshold>> rules_snapshot;
                    {
                        std::lock_guard<std::mutex> lock(rules_mutex_);
                        rules_snapshot = rules_; // one snapshot per batch, not per event
                    }
                    std::shared_ptr<const std::vector<std::shared_ptr<CepRuleState>>> cep_rules_snapshot;
                    {
                        std::lock_guard<std::mutex> lock(cep_rules_mutex_);
                        cep_rules_snapshot = cep_rules_; // one snapshot per batch, not per event
                    }
                    // Read once per batch, not once per event, for the same reason the
                    // rule snapshots above are -- a steady_clock read is cheap but not
                    // free, and CEP time-window eviction only needs millisecond
                    // resolution, not per-event precision.
                    uint64_t now_ms = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());

                    for (const TelemetryPayload& payload : batch) {
                        evaluate_rules(payload, *rules_snapshot);
                        if (!cep_rules_snapshot->empty()) {
                            evaluate_cep_rules(payload, *cep_rules_snapshot, now_ms);
                        }
                    }

                    log_file.write(reinterpret_cast<const char*>(batch.data()), batch.size() * sizeof(TelemetryPayload));
                    log_file.flush(); // bound data loss to the in-flight batch on a hard crash
                    if (!log_file.good()) {
                        sink_ready = false; // sink failed mid-run (e.g. disk full); fall back to retrying
                        log_file.close();
                    }
                    batch.clear();
                    continue;
                }

                if (!running_.load(std::memory_order_acquire)) {
                    break; // stop requested and ring buffer fully drained
                }

                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (log_file.is_open()) {
                log_file.flush();
                log_file.close();
            }
        }

    public:
        explicit EngineImpl(size_t capacity)
            : ring_(capacity),
            signal_ring_(capacity),
            rules_(std::make_shared<const std::vector<RuleThreshold>>()),
            cep_rules_(std::make_shared<const std::vector<std::shared_ptr<CepRuleState>>>()) {
        }

        ~EngineImpl() override {
            stop_persistence();
        }

        bool record(uint32_t event_id, uint32_t trace_id, uint64_t value) const noexcept override {
            TelemetryPayload payload{
                read_cycle_counter(),
                event_id,
                trace_id,
                value
            };
            return ring_.push(payload);
        }

        size_t record_batch(const RawEvent* events, size_t count) const noexcept override {
            if (!events) return 0;
            size_t pushed = 0;
            for (size_t i = 0; i < count; ++i) {
                TelemetryPayload payload{
                    read_cycle_counter(),
                    events[i].event_id,
                    events[i].trace_id,
                    events[i].metric_value
                };
                if (!ring_.push(payload)) {
                    break; // full; no concurrent consumer will free space within this call
                }
                ++pushed;
            }
            return pushed;
        }

        bool is_guard_active() const noexcept override {
            return true;
        }

        void start_persistence(const std::string& log_filepath) override {
            if (running_.load()) return;
            log_file_path_ = log_filepath;
            running_.store(true);
            worker_thread_ = std::thread(&EngineImpl::process_persistence_queue, this);
        }

        void stop_persistence() override {
            if (!running_.load()) return;
            running_.store(false);
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }

        bool add_rule(uint32_t rule_id, uint32_t event_id, uint64_t threshold, uint8_t comparator, uint32_t severity) noexcept override {
            if (comparator > static_cast<uint8_t>(RuleComparator::Equal)) {
                return false; // unrecognized comparator value
            }
            try {
                auto updated = std::make_shared<std::vector<RuleThreshold>>();
                {
                    std::lock_guard<std::mutex> lock(rules_mutex_);
                    *updated = *rules_; // copy current snapshot, append, swap -- readers keep using the old snapshot until this store
                    updated->push_back(RuleThreshold{ rule_id, event_id, threshold, static_cast<RuleComparator>(comparator), severity });
                    rules_ = std::move(updated);
                }
                return true;
            }
            catch (...) {
                return false; // e.g. allocation failure -- existing rules remain valid and in effect
            }
        }

        bool add_cep_rule(uint32_t rule_id, uint32_t event_id, uint8_t window_type, uint64_t window_size,
            uint8_t aggregation, uint8_t comparator, uint64_t threshold, uint32_t severity) noexcept override {
            if (window_type > static_cast<uint8_t>(WindowType::Time)) {
                return false; // unrecognized window type
            }
            if (aggregation > static_cast<uint8_t>(AggregationFunction::Max)) {
                return false; // unrecognized aggregation function
            }
            if (comparator > static_cast<uint8_t>(RuleComparator::Equal)) {
                return false; // unrecognized comparator value
            }
            try {
                auto new_rule = std::make_shared<CepRuleState>(
                    rule_id, event_id, static_cast<WindowType>(window_type), window_size,
                    static_cast<AggregationFunction>(aggregation), static_cast<RuleComparator>(comparator),
                    threshold, severity);
                auto updated = std::make_shared<std::vector<std::shared_ptr<CepRuleState>>>();
                {
                    std::lock_guard<std::mutex> lock(cep_rules_mutex_);
                    *updated = *cep_rules_; // copy current snapshot, append, swap -- readers keep using the old snapshot until this store
                    updated->push_back(std::move(new_rule));
                    cep_rules_ = std::move(updated);
                }
                return true;
            }
            catch (...) {
                return false; // e.g. allocation failure -- existing rules remain valid and in effect
            }
        }

        size_t poll_signals(ThreatSignal* out, size_t max_count) noexcept override {
            if (!out) return 0;
            size_t count = 0;
            while (count < max_count && signal_ring_.pop(out[count])) {
                ++count;
            }
            return count;
        }
    };

    inline std::unique_ptr<Engine> Engine::Create(size_t buffer_capacity) {
        return std::make_unique<EngineImpl>(buffer_capacity);
    }

    // -----------------------------------------------------------------------
    // Broker / Execution API Interop
    // -----------------------------------------------------------------------
    // Direct, in-process wrappers for routing orders to a broker/exchange
    // execution API and instrumenting the round trip through the same
    // header-only Engine used for telemetry ingestion. Registering a
    // RuleThreshold on kExecutionLatencyEventId (via Engine::add_rule) turns
    // "flag any fill slower than N nanoseconds" into a normal SOAR signal
    // drained by the existing poll_signals() loop -- no separate risk
    // pipeline needed. Everything here is pure C++17 with no allocation on
    // the submit() hot path beyond whatever the concrete IBrokerGateway
    // implementation itself performs.

    enum class OrderSide : uint8_t { Buy = 0, Sell = 1 };
    enum class OrderType : uint8_t { Market = 0, Limit = 1 };
    enum class ExecStatus : uint8_t { Accepted = 0, Rejected = 1, Filled = 2, PartiallyFilled = 3 };

    // Fixed-point price/quantity (integer ticks, not float): keeps order
    // representation deterministic and bit-exact across languages/brokers,
    // matching TelemetryPayload/ThreatSignal's existing preference for
    // integer fields over floating point.
    struct OrderRequest {
        uint64_t client_order_id;
        uint32_t instrument_id;
        OrderSide side;
        OrderType type;
        uint64_t price_ticks;
        uint64_t quantity;
    };

    struct ExecutionReport {
        uint64_t client_order_id;
        uint32_t instrument_id;
        uint64_t filled_quantity;
        uint64_t avg_price_ticks;
        ExecStatus status;
    };

    // Telemetry event_id under which ExecutionClient records one round-trip
    // latency sample (nanoseconds, as metric_value) per submit() call.
    inline constexpr uint32_t kExecutionLatencyEventId = 0xE0000001u;

    // Implemented by a concrete broker/exchange adapter (a FIX session, a
    // REST-to-exchange bridge, a matching-engine simulator, ...). submit_order
    // must be noexcept and should avoid blocking where possible on the hot
    // path -- ExecutionClient times the call wall-to-wall and folds the
    // result directly into the shared telemetry ring, so a blocking adapter
    // directly inflates the recorded latency.
    class IBrokerGateway {
    public:
        virtual ~IBrokerGateway() = default;
        virtual bool submit_order(const OrderRequest& request, ExecutionReport& out) noexcept = 0;
        virtual const char* name() const noexcept = 0;
    };

    // Deterministic in-process fill simulator: every order is instantly and
    // fully filled at its requested price. Has no network/IO dependency, so
    // it doubles as a live demo of ExecutionClient without a real broker
    // connection and as a fixed baseline for measuring ExecutionClient's own
    // overhead in isolation from any real gateway's latency.
    class LoopbackBrokerGateway final : public IBrokerGateway {
    public:
        bool submit_order(const OrderRequest& request, ExecutionReport& out) noexcept override {
            out.client_order_id = request.client_order_id;
            out.instrument_id = request.instrument_id;
            out.filled_quantity = request.quantity;
            out.avg_price_ticks = request.price_ticks;
            out.status = ExecStatus::Filled;
            return true;
        }
        const char* name() const noexcept override { return "LoopbackBrokerGateway"; }
    };

    // Wraps a broker gateway with automatic per-order latency instrumentation
    // against a header-only Engine. Construction takes non-owning references:
    // both the engine and the gateway are expected to outlive the client,
    // consistent with this being a thin, allocation-free hot-path wrapper
    // rather than an owning facade.
    class ExecutionClient {
    public:
        ExecutionClient(Engine& engine, IBrokerGateway& gateway) noexcept
            : engine_(engine), gateway_(gateway) {
        }

        // Routes one order to the wrapped gateway and records the round-trip
        // wall-clock latency (nanoseconds) as one telemetry event under
        // kExecutionLatencyEventId, trace_id = the low 32 bits of the order's
        // client_order_id. Returns the gateway's own success/failure result;
        // a full telemetry ring only drops the latency sample, never the
        // order itself -- the gateway call always happens.
        bool submit(const OrderRequest& request, ExecutionReport& out) noexcept {
            auto start = std::chrono::steady_clock::now();
            bool accepted = gateway_.submit_order(request, out);
            auto end = std::chrono::steady_clock::now();

            uint64_t latency_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            engine_.record(
                kExecutionLatencyEventId,
                static_cast<uint32_t>(request.client_order_id & 0xFFFFFFFFu),
                latency_ns);

            return accepted;
        }

        const char* gateway_name() const noexcept { return gateway_.name(); }

    private:
        Engine& engine_;
        IBrokerGateway& gateway_;
    };

    // -----------------------------------------------------------------------
    // Market Data Feed Adapters (L2/L3 order book updates + trade ticks)
    // -----------------------------------------------------------------------
    // Low-latency ingestion for a live market-data feed handler, built on
    // LockFreeRingBuffer above rather than a new ring algorithm: one ring
    // for order-book updates, one for trade execution ticks, both genuinely
    // thread-safe for concurrent producers AND concurrent consumers (Vyukov
    // MPMC, the same ring EngineImpl's own telemetry ring uses) -- unlike
    // SpscRingBuffer elsewhere in this header, which is deliberately
    // narrower (exactly one producer, one consumer) for extra throughput.
    // That MPMC property is the point here: a real feed handler often has
    // more than one producer thread pushing into the same book (e.g. one
    // thread per venue connection, or one per instrument shard funneling
    // into a shared consumer), and this lets them share one MarketDataFeed
    // instance without any external locking.

    enum class BookSide : uint8_t { Bid = 0, Ask = 1 };

    // New: a fresh price level entered the book at `level`. Update: that
    // level's quantity changed. Delete: the level was removed (quantity is
    // not meaningful for a Delete and should be ignored by the consumer).
    enum class BookUpdateAction : uint8_t { New = 0, Update = 1, Delete = 2 };

    enum class TradeAggressor : uint8_t { Buyer = 0, Seller = 1, Unknown = 2 };

    // One L2 price-level change. `level` is a venue-relative depth index
    // (0 = best bid/ask), not a raw price -- consumers reconstruct book
    // state by keyed (instrument_id, side, level) replacement, not by
    // summing deltas, matching how incremental L2 feeds (ITCH, ArcaBook,
    // ...) actually publish updates. `sequence_number` is the venue's own
    // monotonic feed sequence number; this struct does not itself detect
    // gaps -- a consumer compares successive sequence_numbers per
    // instrument_id and decides how to react (resync, drop, ...) since
    // that policy is venue- and strategy-specific. `exchange_timestamp_ns`
    // is whatever timestamp the feed carried (typically exchange gateway
    // time); `timestamp_cycles` is stamped locally at ingestion
    // (read_cycle_counter(), same convention as TelemetryPayload), so the
    // gap between the two can be measured as feed-to-ingestion latency.
    //
    // 56 bytes on disk/wire: 50 logical bytes (5 uint64_t fields = 40, two
    // uint32_t fields = 8, two uint8_t fields = 2) rounded up to 56 -- the
    // next multiple of 8 -- for the uint64_t members' alignment, confirmed
    // via a real sizeof() build before this was relied on anywhere, not
    // assumed from the field list.
    struct L2Update {
        uint64_t timestamp_cycles;
        uint64_t exchange_timestamp_ns;
        uint64_t sequence_number;
        uint64_t price_ticks;
        uint64_t quantity;
        uint32_t instrument_id;
        uint32_t level;
        BookSide side;
        BookUpdateAction action;
        // 6 bytes of trailing alignment padding follow -- left implicit,
        // not manually named, consistent with LicensePayload's convention
        // elsewhere in this header.
    };
    static_assert(sizeof(L2Update) == 56, "L2Update's on-disk size changed -- update animus/bindings.py's L2Update ctypes.Structure to match");

    // One executed trade print. `trade_id` is the venue's own trade/print
    // identifier (for de-duplication against retransmitted messages);
    // `aggressor_side` identifies which side initiated the trade (the
    // resting side is the opposite of whichever this reports), per the
    // same convention most exchange trade feeds use. Same timestamp
    // convention as L2Update above.
    struct TradeTick {
        uint64_t timestamp_cycles;
        uint64_t exchange_timestamp_ns;
        uint64_t sequence_number;
        uint64_t trade_id;
        uint64_t price_ticks;
        uint64_t quantity;
        uint32_t instrument_id;
        TradeAggressor aggressor_side;
        // 3 bytes of trailing alignment padding follow -- left implicit,
        // same convention as L2Update above.
    };
    static_assert(sizeof(TradeTick) == 56, "TradeTick's on-disk size changed -- update animus/bindings.py's TradeTick ctypes.Structure to match");

    // Owns two independent LockFreeRingBuffer instances (order-book
    // updates, trade ticks), sized independently at construction. Fully
    // in-process -- unlike SharedTelemetryChannel above, this does not
    // live in OS shared memory, so it cannot fan data out to a second
    // process; a cross-process market-data feed would need its own
    // segment-backed variant, not provided here. Only ever constructed via
    // create(); the private constructor plus a named factory mirrors this
    // header's other native primitives (Engine::Create,
    // SharedTelemetryChannel::create).
    class MarketDataFeed {
    public:
        MarketDataFeed(const MarketDataFeed&) = delete;
        MarketDataFeed& operator=(const MarketDataFeed&) = delete;

        static std::unique_ptr<MarketDataFeed> create(size_t l2_capacity, size_t trade_capacity) {
            if (l2_capacity == 0) l2_capacity = 1;
            if (trade_capacity == 0) trade_capacity = 1;
            return std::unique_ptr<MarketDataFeed>(new MarketDataFeed(l2_capacity, trade_capacity));
        }

        size_t l2_capacity() const noexcept { return l2_ring_.capacity(); }
        size_t trade_capacity() const noexcept { return trade_ring_.capacity(); }

        // Producer-side: any number of concurrent feed-handler threads may
        // call this on the same MarketDataFeed instance at once (see the
        // class-level comment above for why that's safe). Stamps
        // timestamp_cycles itself at the moment of the call;
        // exchange_timestamp_ns is caller-supplied, since only the feed
        // itself knows the venue's own wire timestamp. Never blocks;
        // returns false if the ring is full.
        bool push_l2_update(uint32_t instrument_id, uint8_t side, uint8_t action, uint32_t level,
            uint64_t price_ticks, uint64_t quantity, uint64_t sequence_number, uint64_t exchange_timestamp_ns) noexcept {
            L2Update update{
                read_cycle_counter(),
                exchange_timestamp_ns,
                sequence_number,
                price_ticks,
                quantity,
                instrument_id,
                level,
                static_cast<BookSide>(side),
                static_cast<BookUpdateAction>(action)
            };
            return l2_ring_.push(update);
        }

        // Producer-side, same concurrency guarantee as push_l2_update.
        bool push_trade(uint32_t instrument_id, uint64_t trade_id, uint8_t aggressor_side,
            uint64_t price_ticks, uint64_t quantity, uint64_t sequence_number, uint64_t exchange_timestamp_ns) noexcept {
            TradeTick tick{
                read_cycle_counter(),
                exchange_timestamp_ns,
                sequence_number,
                trade_id,
                price_ticks,
                quantity,
                instrument_id,
                static_cast<TradeAggressor>(aggressor_side)
            };
            return trade_ring_.push(tick);
        }

        // Consumer-side: any number of concurrent consumer threads may
        // call this on the same instance at once, same MPMC guarantee as
        // the producer side. Zero-copy for the caller (direct struct copy
        // into caller-owned `out`, no allocation). Returns the number of
        // updates actually written.
        size_t poll_l2_updates(L2Update* out, size_t max_count) noexcept {
            if (!out) return 0;
            size_t count = 0;
            while (count < max_count && l2_ring_.pop(out[count])) ++count;
            return count;
        }

        // Consumer-side, same concurrency guarantee as poll_l2_updates.
        size_t poll_trades(TradeTick* out, size_t max_count) noexcept {
            if (!out) return 0;
            size_t count = 0;
            while (count < max_count && trade_ring_.pop(out[count])) ++count;
            return count;
        }

    private:
        MarketDataFeed(size_t l2_capacity, size_t trade_capacity)
            : l2_ring_(l2_capacity), trade_ring_(trade_capacity) {
        }

        LockFreeRingBuffer<L2Update> l2_ring_;
        LockFreeRingBuffer<TradeTick> trade_ring_;
    };

} // namespace animus
// Portable C-ABI export/import macro: MSVC (and MinGW) use __declspec, while
// GCC/Clang on Linux/macOS use the "default" visibility attribute -- plain
// __declspec is not recognized by non-Windows GCC/Clang, so this must branch
// on _WIN32 rather than the compiler alone to support both the MSVC vcxproj
// build (AnimusCore_v1.dll) and the portable CMake build (AnimusNative.dll /
// libanimus_native.so) from the same header.
#ifndef ANIMUS_API
#if defined(_WIN32)
#ifdef ANIMUS_EXPORTS
#define ANIMUS_API __declspec(dllexport)
#else
#define ANIMUS_API __declspec(dllimport)
#endif
#else
#ifdef ANIMUS_EXPORTS
#define ANIMUS_API __attribute__((visibility("default")))
#else
#define ANIMUS_API
#endif
#endif
#endif

// Add C-ABI Exports at the bottom of animus.hpp (below namespace animus)
extern "C" {
    ANIMUS_API bool animus_init(size_t buffer_capacity);
    ANIMUS_API bool animus_record_event(uint32_t event_id, uint32_t trace_id, uint64_t metric_value);
    ANIMUS_API size_t animus_record_events_batch(const animus::RawEvent* events, size_t count);
    ANIMUS_API void animus_start_logging(const char* filepath);
    ANIMUS_API void animus_stop_logging();
    ANIMUS_API bool animus_add_rule(uint32_t rule_id, uint32_t event_id, uint64_t threshold, uint8_t comparator, uint32_t severity);

    // Registers a CEP sliding-window rule (see animus::CepRuleState).
    // window_type: 0=Count (window_size = event count), 1=Time (window_size
    // = milliseconds). aggregation: 0=Sum, 1=Avg, 2=Min, 3=Max. comparator:
    // same encoding as animus_add_rule (0=GreaterThan, 1=LessThan,
    // 2=Equal). Matches are delivered through the same animus_poll_signals
    // queue as animus_add_rule matches, with the window's aggregated value
    // (not the triggering event's raw metric_value) in the ThreatSignal.
    ANIMUS_API bool animus_add_cep_rule(uint32_t rule_id, uint32_t event_id, uint8_t window_type, uint64_t window_size,
        uint8_t aggregation, uint8_t comparator, uint64_t threshold, uint32_t severity);

    ANIMUS_API size_t animus_poll_signals(animus::ThreatSignal* out, size_t max_count);

    // Standalone SPSC ingestion channel (see animus::SpscRingBuffer above),
    // fully independent of animus_init()'s Engine singleton -- its own
    // buffer, its own lifetime, not touched by animus_start_logging/
    // animus_add_rule/animus_poll_signals. Single-producer, single-consumer
    // only: animus_spsc_record_events_batch must never be called from more
    // than one thread concurrently, and neither must animus_spsc_drain
    // (independently of each other -- one producer thread and one, possibly
    // different, consumer thread is fine; two of either is not).
    ANIMUS_API bool animus_spsc_init(size_t capacity);
    ANIMUS_API size_t animus_spsc_record_events_batch(const animus::RawEvent* events, size_t count);
    ANIMUS_API size_t animus_spsc_drain(animus::TelemetryPayload* out, size_t max_count);

    // Pins the calling OS thread to logical CPU `core_id` (0-based). Returns
    // false if core_id is out of range or the platform has no supported
    // hard-pinning API (see animus_engine.cpp for the per-platform
    // implementation and exactly which platforms that covers). Affects only
    // the thread that calls it -- pin a producer thread before it starts
    // pushing to reduce OS-scheduler-induced tail latency on that thread's
    // calls, not process-wide affinity.
    ANIMUS_API bool animus_pin_current_thread_to_core(int core_id);

    // Raises the calling OS thread (and, on Windows, this process) to the
    // highest realtime/time-critical scheduling tier the host will grant,
    // falling back a tier if it won't (see animus::sys::set_thread_high_priority,
    // include/animus/thread_affinity.hpp, for exactly what that means per
    // platform). Same license gate as animus_pin_current_thread_to_core --
    // fails closed (silently returns without changing scheduling) with no
    // verified license. Never fails loudly and never throws: call it once,
    // right after pinning, on a thread about to enter its hot loop.
    ANIMUS_API void animus_set_thread_high_priority(void);

    // Logical CPU count on this machine, for sanity-checking a core_id
    // before calling animus_pin_current_thread_to_core.
    ANIMUS_API unsigned animus_get_cpu_count(void);

    // Cross-process shared-memory telemetry channel (animus::SharedTelemetryChannel).
    // Handle-based, not a singleton like g_engine: animus_shm_create/attach
    // return an opaque pointer to hand back into every other animus_shm_*
    // call and finally into animus_shm_close, which destroys it (unmapping
    // the segment on this process's side -- other attached processes keep
    // their own mapping until they close theirs too). Returns nullptr on
    // failure from create/attach (bad name, no such segment, a size/name
    // collision) rather than a handle that then fails every subsequent call.
    ANIMUS_API void* animus_shm_create(const char* name, uint64_t capacity);
    ANIMUS_API void* animus_shm_attach(const char* name);
    ANIMUS_API void animus_shm_close(void* channel);

    // Destroys the underlying OS shared-memory object (POSIX: shm_unlink;
    // Windows: a documented no-op -- see SharedMemorySegment::unlink).
    // Call once every attached process has closed its handle, same
    // contract as animus.shm.SharedTelemetryRing.unlink().
    ANIMUS_API bool animus_shm_unlink(const char* name);

    ANIMUS_API uint64_t animus_shm_capacity(void* channel);
    ANIMUS_API bool animus_shm_push(void* channel, uint32_t event_id, uint32_t trace_id, uint64_t metric_value);
    ANIMUS_API bool animus_shm_pop(void* channel, animus::SharedTelemetryRecord* out);

    // Verifies license_path (see animus::LicensePayload above) against
    // the public key baked into this build and this machine's hardware
    // fingerprint. Entirely offline -- no network call is made or ever
    // will be. On success, sets the process-wide entitlement state that
    // animus_pin_current_thread_to_core / animus_spsc_init check before
    // allowing use; there is no grace period and no way to use either
    // without a verified license in this build. Returns false for: file
    // not found or wrong size, bad magic, signature mismatch (tampered
    // file or wrong signing key), fingerprint mismatch (license issued
    // for a different machine), or an expired license. Windows-only for
    // now -- see animus_engine.cpp's definition for exactly why, and why
    // returning false rather than faking success on other platforms was
    // the deliberate choice here, same as animus_pin_current_thread_to_core.
    ANIMUS_API bool animus_verify_license(const char* license_path);

    // True once animus_verify_license has succeeded in this process.
    ANIMUS_API bool animus_is_licensed(void);

    // The verified license's entitled core count, or 0 if unlicensed.
    ANIMUS_API uint32_t animus_licensed_max_cores(void);

    // Market data feed adapters (animus::MarketDataFeed). Handle-based,
    // same pattern as animus_shm_create/attach/close above -- not a
    // singleton like g_engine, so a caller can create as many independent
    // feeds as it needs (e.g. one per venue). Returns nullptr from
    // animus_feed_create on allocation failure (out of memory); every
    // other call is a no-op / returns 0/false on a null or already-closed
    // handle rather than crashing.
    ANIMUS_API void* animus_feed_create(size_t l2_capacity, size_t trade_capacity);
    ANIMUS_API void animus_feed_close(void* feed);
    ANIMUS_API size_t animus_feed_l2_capacity(void* feed);
    ANIMUS_API size_t animus_feed_trade_capacity(void* feed);

    // Pushes one order-book price-level update. Thread-safe for any number
    // of concurrent producer threads sharing one `feed` handle (see
    // animus::MarketDataFeed's class comment). side: 0=Bid, 1=Ask. action:
    // 0=New, 1=Update, 2=Delete. Never blocks; returns false if the ring
    // is full or `feed` is null.
    ANIMUS_API bool animus_feed_push_l2_update(void* feed, uint32_t instrument_id, uint8_t side, uint8_t action,
        uint32_t level, uint64_t price_ticks, uint64_t quantity, uint64_t sequence_number, uint64_t exchange_timestamp_ns);

    // Pushes one trade execution tick. Same thread-safety as
    // animus_feed_push_l2_update. aggressor_side: 0=Buyer, 1=Seller,
    // 2=Unknown.
    ANIMUS_API bool animus_feed_push_trade(void* feed, uint32_t instrument_id, uint64_t trade_id, uint8_t aggressor_side,
        uint64_t price_ticks, uint64_t quantity, uint64_t sequence_number, uint64_t exchange_timestamp_ns);

    // Drains up to max_count pending order-book updates into the
    // caller-owned `out` buffer (zero-copy: direct struct copy, no
    // allocation). Thread-safe for any number of concurrent consumer
    // threads sharing one `feed` handle. Returns the number actually
    // written.
    ANIMUS_API size_t animus_feed_poll_l2_updates(void* feed, animus::L2Update* out, size_t max_count);

    // Drains up to max_count pending trade ticks, same contract as
    // animus_feed_poll_l2_updates.
    ANIMUS_API size_t animus_feed_poll_trades(void* feed, animus::TradeTick* out, size_t max_count);

    // animus::sys::ipc::ShmRing<RawEvent> (include/animus/shm_ipc.hpp), the
    // generic cross-process ring, instantiated here for animus::RawEvent
    // specifically -- the same 16-byte record animus_record_events_batch
    // already takes, chosen so a batch popped off this ring is exactly the
    // array record_batch() wants, with no translation step. A different
    // C++ caller (or a future C-ABI export) is free to instantiate
    // ShmRing<T> for some other T directly; this is the one concrete
    // instantiation exposed across the ctypes boundary, not a claim that
    // it's the only one that exists.
    //
    // Handle-based, same pattern as animus_shm_create/animus_feed_create --
    // not a singleton, so a caller can open as many independent rings as
    // it needs. Deliberately NOT license-gated: like SharedMemorySegment
    // and MarketDataFeed above (and unlike animus_pin_current_thread_to_core/
    // animus_spsc_init), this is a data-transport primitive, not a
    // hardware-entitlement one.
    //
    // No spin-blocking variant is exposed here on purpose. ShmRing<T>'s own
    // push_spin()/pop_spin() can legitimately block for low seconds waiting
    // for a peer (bounded by max_spins, not infinite, but still long for a
    // single call) -- fine for a native C++ caller, but a ctypes call that
    // blocks that long holds the GIL released for its whole duration and
    // cannot be interrupted with Ctrl+C from Python. A caller that wants
    // "wait for the next item" implements a bounded retry loop in Python
    // instead (same shape as ingest_engine.py's own signal-poller loop),
    // which stays interruptible at every iteration.
    ANIMUS_API void* animus_shm_ring_create(const char* name, size_t requested_capacity);
    ANIMUS_API void* animus_shm_ring_open(const char* name);
    ANIMUS_API void animus_shm_ring_close(void* ring);
    ANIMUS_API bool animus_shm_ring_unlink(const char* name);
    ANIMUS_API size_t animus_shm_ring_capacity(void* ring);

    // Never blocks; false if the ring is full/empty or `ring` is null.
    ANIMUS_API bool animus_shm_ring_try_push(void* ring, const animus::RawEvent* event);
    ANIMUS_API bool animus_shm_ring_try_pop(void* ring, animus::RawEvent* out);

    // Same "stop at the first push that fails, return how many actually
    // transferred" contract as animus_record_events_batch/
    // animus_spsc_record_events_batch -- never blocks, never partially
    // corrupts state, just tells the caller how far it got.
    ANIMUS_API size_t animus_shm_ring_push_batch(void* ring, const animus::RawEvent* events, size_t count);
    ANIMUS_API size_t animus_shm_ring_pop_batch(void* ring, animus::RawEvent* out, size_t max_count);

    // Same animus::sys::ipc::ShmRing<T> primitive, instantiated for
    // animus::OrderRequest instead of animus::RawEvent -- routing orders
    // (not telemetry) across a shared-memory ring needs its own wire shape,
    // not a reinterpretation of RawEvent's fields. Otherwise byte-for-byte
    // the same 9-function surface and the same contracts as the
    // animus_shm_ring_* block above (create/open own the segment's
    // lifecycle the same way; try_push/try_pop never block;
    // push_batch/pop_batch stop at the first failure and report how far
    // they got) -- see that block's own comments for what each one means.
    ANIMUS_API void* animus_shm_ring_order_create(const char* name, size_t requested_capacity);
    ANIMUS_API void* animus_shm_ring_order_open(const char* name);
    ANIMUS_API void animus_shm_ring_order_close(void* ring);
    ANIMUS_API bool animus_shm_ring_order_unlink(const char* name);
    ANIMUS_API size_t animus_shm_ring_order_capacity(void* ring);
    ANIMUS_API bool animus_shm_ring_order_try_push(void* ring, const animus::OrderRequest* order);
    ANIMUS_API bool animus_shm_ring_order_try_pop(void* ring, animus::OrderRequest* out);
    ANIMUS_API size_t animus_shm_ring_order_push_batch(void* ring, const animus::OrderRequest* orders, size_t count);
    ANIMUS_API size_t animus_shm_ring_order_pop_batch(void* ring, animus::OrderRequest* out, size_t max_count);
}

// -------------------------------------------------------------------------
// animus_security.hpp -- Phase 8: RBAC + Multi-Tenant Isolation
// -------------------------------------------------------------------------
// Phase 8: RBAC + multi-tenant telemetry stream isolation.
//
// Platform-independent (unlike animus_transport.hpp, which is Windows-only)
// and header-only for the same reason as animus.hpp itself: any C++17
// translation unit can #include this alongside animus.hpp with no separate
// .cpp to build. This layer does not talk to the network -- it is the
// authorization/isolation boundary that animus_transport.hpp's Schannel
// mTLS server calls into once a client certificate has already been
// cryptographically verified.

#include <cstdint>
#include <string>
#include <deque>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace animus {
namespace security {

    // Least-privilege role set. Viewer can only observe already-matched
    // signals (e.g. a dashboard); Operator can additionally ingest telemetry
    // (e.g. a trading/agent process); Admin can additionally register rules
    // and manage persistence/tenants (e.g. an ops/config-management client).
    enum class Role : uint8_t {
        Viewer = 0,
        Operator = 1,
        Admin = 2,
    };

    enum class Permission : uint8_t {
        RecordEvent = 0,
        PollSignals = 1,
        AddRule = 2,
        ManagePersistence = 3,
        ManageTenants = 4,
        SubmitOrder = 5,
    };

    // Static role -> permission table, checked in O(1) with no allocation.
    // Deliberately a flat switch rather than a data-driven policy file: RBAC
    // here is a small, fixed lattice (3 roles x 6 permissions), and a
    // switch keeps the mapping exhaustively checkable by the compiler
    // (-Wswitch) if a role or permission is ever added.
    class RbacPolicy {
    public:
        static bool is_allowed(Role role, Permission perm) noexcept {
            switch (role) {
            case Role::Viewer:
                return perm == Permission::PollSignals;
            case Role::Operator:
                // Operator already covers "a trading/agent process" per
                // this enum's own docstring above -- SubmitOrder belongs
                // here for exactly the same reason RecordEvent does.
                return perm == Permission::PollSignals || perm == Permission::RecordEvent
                    || perm == Permission::SubmitOrder;
            case Role::Admin:
                return true;
            }
            return false;
        }
    };

    // Identity + entitlement for one call. Deliberately NOT self-certifying:
    // callers of SecureTelemetryGateway must construct an AccessToken from
    // an already-verified identity (e.g. a Schannel-verified client
    // certificate mapped through animus_transport::CertificateIdentityMap),
    // never from a value a remote peer asserts on the wire -- the gateway
    // enforces RBAC and tenant isolation against whatever token it is
    // given, so a spoofable token defeats both regardless of how carefully
    // the gateway itself is written.
    struct AccessToken {
        uint32_t tenant_id;
        uint64_t principal_id;
        Role role;
    };

    enum class AuditOutcome : uint8_t { Allowed = 0, Denied = 1 };

    struct AuditEvent {
        uint64_t timestamp_cycles;
        uint32_t tenant_id;
        uint64_t principal_id;
        Permission permission;
        AuditOutcome outcome;
    };

    // Owns one isolated Engine per tenant: separate lock-free ring buffer,
    // separate rule set, separate persistence file. This makes isolation
    // structural rather than a filter applied after the fact -- there is no
    // code path in SecureTelemetryGateway that can read tenant B's ring
    // buffer while authorized only for tenant A, because tenant A's calls
    // never resolve to tenant B's Engine pointer in the first place.
    class TenantRegistry {
    public:
        Engine* create_tenant(uint32_t tenant_id, size_t buffer_capacity = 65536) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tenants_.find(tenant_id);
            if (it != tenants_.end()) return it->second.get();
            auto engine = Engine::Create(buffer_capacity);
            Engine* raw = engine.get();
            tenants_.emplace(tenant_id, std::move(engine));
            return raw;
        }

        Engine* get_tenant(uint32_t tenant_id) const noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tenants_.find(tenant_id);
            return it == tenants_.end() ? nullptr : it->second.get();
        }

        bool remove_tenant(uint32_t tenant_id) {
            std::lock_guard<std::mutex> lock(mutex_);
            return tenants_.erase(tenant_id) > 0;
        }

        size_t tenant_count() const noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            return tenants_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<uint32_t, std::unique_ptr<Engine>> tenants_;
    };

    // Authorization + tenant-routing facade over TenantRegistry. Every
    // method takes an AccessToken, checks it against RbacPolicy, resolves
    // the token's own tenant Engine (never a caller-supplied Engine*), and
    // appends one AuditEvent per call -- allowed or denied -- to an
    // in-memory trail that is completely separate from tenant telemetry
    // (an auditor role is not required to have PollSignals on any tenant).
    class SecureTelemetryGateway {
    public:
        explicit SecureTelemetryGateway(TenantRegistry& registry) noexcept
            : registry_(registry) {
        }

        bool record(const AccessToken& token, uint32_t event_id, uint32_t trace_id, uint64_t value) {
            return authorize_and_dispatch(token, Permission::RecordEvent, [&](Engine& engine) {
                return engine.record(event_id, trace_id, value);
                });
        }

        bool add_rule(const AccessToken& token, uint32_t rule_id, uint32_t event_id, uint64_t threshold, uint8_t comparator, uint32_t severity) {
            return authorize_and_dispatch(token, Permission::AddRule, [&](Engine& engine) {
                return engine.add_rule(rule_id, event_id, threshold, comparator, severity);
                });
        }

        size_t poll_signals(const AccessToken& token, ThreatSignal* out, size_t max_count) {
            size_t result = 0;
            authorize_and_dispatch(token, Permission::PollSignals, [&](Engine& engine) {
                result = engine.poll_signals(out, max_count);
                return true;
                });
            return result;
        }

        bool start_persistence(const AccessToken& token, const std::string& log_filepath) {
            return authorize_and_dispatch(token, Permission::ManagePersistence, [&](Engine& engine) {
                engine.start_persistence(log_filepath);
                return true;
                });
        }

        bool stop_persistence(const AccessToken& token) {
            return authorize_and_dispatch(token, Permission::ManagePersistence, [&](Engine& engine) {
                engine.stop_persistence();
                return true;
                });
        }

        // Registers a new isolated tenant. Requires ManageTenants (Admin
        // only) rather than being routed through TenantRegistry directly,
        // so tenant creation itself is audited like every other action.
        bool create_tenant(const AccessToken& token, uint32_t new_tenant_id, size_t buffer_capacity = 65536) {
            bool allowed = RbacPolicy::is_allowed(token.role, Permission::ManageTenants);
            if (allowed) {
                registry_.create_tenant(new_tenant_id, buffer_capacity);
            }
            append_audit(token, Permission::ManageTenants, allowed ? AuditOutcome::Allowed : AuditOutcome::Denied);
            return allowed;
        }

        size_t poll_audit_log(AuditEvent* out, size_t max_count) {
            std::lock_guard<std::mutex> lock(audit_mutex_);
            size_t count = 0;
            while (count < max_count && !audit_log_.empty()) {
                out[count++] = audit_log_.front();
                audit_log_.pop_front();
            }
            return count;
        }

    private:
        template <typename Fn>
        bool authorize_and_dispatch(const AccessToken& token, Permission perm, Fn&& fn) {
            bool allowed = RbacPolicy::is_allowed(token.role, perm);
            Engine* engine = allowed ? registry_.get_tenant(token.tenant_id) : nullptr;
            bool ok = engine && fn(*engine);
            append_audit(token, perm, (allowed && engine) ? AuditOutcome::Allowed : AuditOutcome::Denied);
            return ok;
        }

        void append_audit(const AccessToken& token, Permission perm, AuditOutcome outcome) {
            std::lock_guard<std::mutex> lock(audit_mutex_);
            audit_log_.push_back(AuditEvent{
                read_cycle_counter(), token.tenant_id, token.principal_id, perm, outcome });
        }

        TenantRegistry& registry_;
        std::mutex audit_mutex_;
        std::deque<AuditEvent> audit_log_;
    };

    // Authorization + tenant-routing facade over animus::ExecutionClient,
    // same shape and same reasoning as SecureTelemetryGateway above -- every
    // method takes an AccessToken, checks it against RbacPolicy, resolves
    // the token's own tenant ExecutionClient (never a caller-supplied one),
    // and appends one AuditEvent per call to its own audit trail, separate
    // from SecureTelemetryGateway's (an auditor scoped to telemetry is not
    // automatically entitled to see execution decisions, or vice versa).
    //
    // Deliberately reuses the SAME TenantRegistry SecureTelemetryGateway
    // does, rather than owning a second, parallel notion of "tenant":
    // execution instrumentation (ExecutionClient::submit's own
    // kExecutionLatencyEventId telemetry) needs somewhere to record into,
    // and that's the tenant's already-isolated Engine -- one Engine per
    // tenant remains the single source of isolation, not two.
    class SecureExecutionGateway {
    public:
        explicit SecureExecutionGateway(TenantRegistry& registry) noexcept
            : registry_(registry) {
        }

        // Wires tenant_id's execution path: one LoopbackBrokerGateway + one
        // ExecutionClient bound to that tenant's existing Engine. Requires
        // ManageTenants (Admin only), and requires the tenant's Engine to
        // already exist (registry.create_tenant/gateway.create_tenant must
        // have been called first) -- there is no "create both at once"
        // convenience here, the same way TenantRegistry itself doesn't
        // auto-vivify a tenant on first use elsewhere in this file.
        // Idempotent: calling this again for an already-set-up tenant is a
        // no-op success, not an error.
        bool create_execution_tenant(const AccessToken& token, uint32_t tenant_id) {
            bool allowed = RbacPolicy::is_allowed(token.role, Permission::ManageTenants);
            Engine* engine = allowed ? registry_.get_tenant(tenant_id) : nullptr;
            bool ok = false;
            if (allowed && engine) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (tenants_.find(tenant_id) == tenants_.end()) {
                    auto gateway = std::make_unique<LoopbackBrokerGateway>();
                    auto client = std::make_unique<ExecutionClient>(*engine, *gateway);
                    tenants_.emplace(tenant_id, TenantExecution{ std::move(gateway), std::move(client) });
                }
                ok = true;
            }
            append_audit(token, Permission::ManageTenants, (allowed && engine) ? AuditOutcome::Allowed : AuditOutcome::Denied);
            return ok;
        }

        // Routes one order through the token's own tenant ExecutionClient.
        // Requires SubmitOrder. Returns false for BOTH a denied token and a
        // broker-rejected order -- same flat-bool convention as
        // SecureTelemetryGateway::record() above; poll_execution_audit_log()
        // is how a caller distinguishes "not authorized" from "the tenant's
        // execution path isn't set up yet" from "the broker rejected it",
        // not the return value of this call.
        bool submit(const AccessToken& token, const OrderRequest& request, ExecutionReport& out) {
            bool allowed = RbacPolicy::is_allowed(token.role, Permission::SubmitOrder);
            ExecutionClient* client = nullptr;
            if (allowed) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = tenants_.find(token.tenant_id);
                if (it != tenants_.end()) client = it->second.client.get();
            }
            bool ok = client && client->submit(request, out);
            append_audit(token, Permission::SubmitOrder, (allowed && client) ? AuditOutcome::Allowed : AuditOutcome::Denied);
            return ok;
        }

        size_t poll_execution_audit_log(AuditEvent* out, size_t max_count) {
            std::lock_guard<std::mutex> lock(audit_mutex_);
            size_t count = 0;
            while (count < max_count && !audit_log_.empty()) {
                out[count++] = audit_log_.front();
                audit_log_.pop_front();
            }
            return count;
        }

    private:
        struct TenantExecution {
            std::unique_ptr<LoopbackBrokerGateway> gateway;
            std::unique_ptr<ExecutionClient> client;
        };

        void append_audit(const AccessToken& token, Permission perm, AuditOutcome outcome) {
            std::lock_guard<std::mutex> lock(audit_mutex_);
            audit_log_.push_back(AuditEvent{
                read_cycle_counter(), token.tenant_id, token.principal_id, perm, outcome });
        }

        TenantRegistry& registry_;
        std::mutex mutex_;
        std::unordered_map<uint32_t, TenantExecution> tenants_;
        std::mutex audit_mutex_;
        std::deque<AuditEvent> audit_log_;
    };

} // namespace security
} // namespace animus

// ---- C-ABI: RBAC-gated multi-tenant execution orchestration ------------
// Declared here rather than in animus.hpp's own extern "C" block:
// animus::security::AccessToken/AuditEvent are defined in this header, not
// animus.hpp, and animus.hpp cannot depend on this file without inverting
// the layering animus_security.hpp itself documents (this file includes
// animus.hpp, not the other way around). Definitions live in
// animus_engine.cpp, same split as every other C-ABI export in this
// codebase (portable declaration, platform-specific/DLL-only definition).
//
// SecurityContext (animus_engine.cpp) bundles one TenantRegistry + one
// SecureTelemetryGateway + one SecureExecutionGateway behind a single
// handle -- Python drives one object, not three. Deliberately NOT
// exposing SecureTelemetryGateway's record/add_rule/poll_signals/
// persistence surface here: animus_security_create_tenant exists only
// because animus_security_create_execution_tenant requires the tenant's
// Engine to already exist, not to give Python a general-purpose RBAC'd
// telemetry API -- that would be a separate feature, not this one.
extern "C" {
    ANIMUS_API void* animus_security_create_context(void);
    ANIMUS_API void animus_security_close_context(void* ctx);

    // Requires ManageTenants (Admin). buffer_capacity sizes the new
    // tenant's own isolated Engine ring (same default as Engine::Create).
    ANIMUS_API bool animus_security_create_tenant(void* ctx, const animus::security::AccessToken* token,
        uint32_t new_tenant_id, size_t buffer_capacity);

    // Requires ManageTenants (Admin) AND new_tenant_id's telemetry tenant
    // to already exist via animus_security_create_tenant. Idempotent.
    ANIMUS_API bool animus_security_create_execution_tenant(void* ctx, const animus::security::AccessToken* token,
        uint32_t tenant_id);

    // Requires SubmitOrder (Operator/Admin). Returns false for a denied
    // token, a tenant with no execution path set up, or a broker-rejected
    // order alike -- poll_execution_audit_log distinguishes why, not this
    // return value.
    ANIMUS_API bool animus_security_submit_order(void* ctx, const animus::security::AccessToken* token,
        const animus::OrderRequest* request, animus::ExecutionReport* out);

    // Drains up to max_count pending execution RBAC decisions (allowed and
    // denied alike). Not gated by any permission itself -- same as
    // SecureTelemetryGateway's poll_audit_log, auditing the audit log is
    // intentionally not part of this lattice.
    ANIMUS_API size_t animus_security_poll_execution_audit_log(void* ctx, animus::security::AuditEvent* out, size_t max_count);
}

// -------------------------------------------------------------------------
// animus_transport.hpp -- Phase 8: mTLS / TLS 1.3 Transport (Schannel)
// -------------------------------------------------------------------------
#if defined(_WIN32) && defined(_MSC_VER)
// Phase 8: mTLS / TLS 1.3 encrypted transport for remote telemetry ingestion.
//
// Windows-only: built directly on the OS-native Schannel SSPI provider
// (secur32.lib / crypt32.lib / ws2_32.lib -- all part of the Windows SDK)
// rather than a third-party TLS library such as OpenSSL. This keeps
// animus.hpp's "zero external dependency" property intact on the one
// platform this project currently ships a native build for (see
// AnimusCore_v1.vcxproj) -- the tradeoff, made explicitly rather than
// silently, is that this header does not build on non-Windows platforms.
// animus_security.hpp (RBAC + multi-tenant isolation) has no such
// restriction and is plain portable C++17.
//
// Design: a verified client certificate is mapped to an
// animus::security::AccessToken (tenant_id + role) via CertificateIdentityMap
// *after* Schannel has cryptographically verified the certificate chain --
// so RBAC/tenant-isolation decisions downstream are made against an
// identity TLS has already proven the peer possesses the private key for,
// never a value the client merely asserts in application-layer data.
#define SECURITY_WIN32
// SCH_CREDENTIALS/TLS_PARAMETERS (the modern credential structure --
// required to actually negotiate TLS 1.3; see the comment in
// SecureChannel::handshake_as_client()) are compiled into schannel.h only
// under SCHANNEL_USE_BLACKLISTS, and that block itself uses UNICODE_STRING/
// PUNICODE_STRING, which come from subauth.h rather than windows.h.
#define SCHANNEL_USE_BLACKLISTS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <subauth.h>
#include <sspi.h>
#include <schannel.h>
#include <wincrypt.h>
#include <ncrypt.h>


#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <fstream>
#include <iterator>
#include <stdexcept>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "advapi32.lib")

// Windows SDKs prior to 10.0.20348 (Server 2022 / 21H2) don't define the
// TLS 1.3 protocol bits in schannel.h. Fall back to the documented values
// so this header still compiles (and correctly negotiates TLS 1.3) against
// an older SDK, rather than silently downgrading to TLS 1.2.
#ifndef SP_PROT_TLS1_3_SERVER
#define SP_PROT_TLS1_3_SERVER 0x00001000
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#define SP_PROT_TLS1_3_CLIENT 0x00002000
#endif
#ifndef SP_PROT_TLS1_3
#define SP_PROT_TLS1_3 (SP_PROT_TLS1_3_SERVER | SP_PROT_TLS1_3_CLIENT)
#endif

namespace animus {
namespace transport {

    // One length-prefixed-free telemetry frame sent per encrypted TLS
    // record. Mirrors TelemetryPayload's event_id/trace_id/metric_value.
    // tenant_id/role are intentionally NOT part of this struct: which
    // tenant a frame is attributed to, and under what role, is derived
    // server-side from the already-verified client certificate via
    // CertificateIdentityMap, never taken from wire-supplied fields --
    // a client cannot claim to be a different tenant just by changing a
    // value in its own request.
#pragma pack(push, 1)
    struct WireFrame {
        uint32_t event_id;
        uint32_t trace_id;
        uint64_t metric_value;
    };
#pragma pack(pop)

    inline std::string hresult_hex(long value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(value));
        return std::string(buf);
    }

    // -----------------------------------------------------------------------
    // Minimal blocking TCP wrapper (WinSock). Only what the handshake/frame
    // loops below need: connect, listen+accept, send-all, receive-some.
    // -----------------------------------------------------------------------
    class WinsockInit {
    public:
        WinsockInit() {
            WSADATA wsa;
            ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        }
        ~WinsockInit() { if (ok_) WSACleanup(); }
        WinsockInit(const WinsockInit&) = delete;
        WinsockInit& operator=(const WinsockInit&) = delete;
        bool ok() const noexcept { return ok_; }
    private:
        bool ok_ = false;
    };

    class TcpSocket {
    public:
        TcpSocket() = default;
        explicit TcpSocket(SOCKET s) noexcept : sock_(s) {}
        TcpSocket(const TcpSocket&) = delete;
        TcpSocket& operator=(const TcpSocket&) = delete;
        TcpSocket(TcpSocket&& other) noexcept : sock_(other.sock_) { other.sock_ = INVALID_SOCKET; }
        TcpSocket& operator=(TcpSocket&& other) noexcept {
            if (this != &other) { close(); sock_ = other.sock_; other.sock_ = INVALID_SOCKET; }
            return *this;
        }
        ~TcpSocket() { close(); }

        void close() noexcept {
            if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        }

        bool valid() const noexcept { return sock_ != INVALID_SOCKET; }

        // timeout_ms bounds how long a connection ATTEMPT can take -- not
        // just the degenerate "port refused" case (which is normally near-
        // instant on loopback and doesn't need this), but the case a
        // cluster actually cares about: a peer whose listening socket was
        // JUST closed (see animus_cluster.hpp's RaftNode::stop()) can, in
        // practice on Windows, leave a brief window where a fresh connect()
        // to that port is neither accepted nor immediately RST'd, and the
        // OS's default connect timeout for that case is many seconds --
        // long enough to stall an entire Raft election round waiting on a
        // single dead peer. Implemented as non-blocking connect + select()
        // rather than a plain blocking connect(), specifically so a dead
        // peer fails fast and predictably instead of at the mercy of the
        // OS's default TCP retransmission/timeout schedule.
        static TcpSocket connect_to(const std::string& host, uint16_t port, int timeout_ms = 5000) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) throw std::runtime_error("socket() failed: " + std::to_string(WSAGetLastError()));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
                closesocket(s);
                throw std::runtime_error("invalid IPv4 address: " + host);
            }

            u_long non_blocking = 1;
            ioctlsocket(s, FIONBIO, &non_blocking);

            int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            if (rc != 0) {
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) {
                    closesocket(s);
                    throw std::runtime_error("connect() failed: " + std::to_string(err));
                }
                fd_set write_set, err_set;
                FD_ZERO(&write_set); FD_SET(s, &write_set);
                FD_ZERO(&err_set); FD_SET(s, &err_set);
                timeval tv{};
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                int ready = select(0, nullptr, &write_set, &err_set, &tv);
                if (ready == 0) {
                    closesocket(s);
                    throw std::runtime_error("connect() timed out after " + std::to_string(timeout_ms) + " ms");
                }
                if (ready == SOCKET_ERROR || FD_ISSET(s, &err_set)) {
                    closesocket(s);
                    throw std::runtime_error("connect() failed: " + std::to_string(WSAGetLastError()));
                }
                int so_error = 0;
                int so_error_len = sizeof(so_error);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_error_len) != 0 || so_error != 0) {
                    closesocket(s);
                    throw std::runtime_error("connect() failed: " + std::to_string(so_error));
                }
            }

            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking); // every other TcpSocket method assumes blocking semantics
            return TcpSocket(s);
        }

        static TcpSocket listen_on(uint16_t port) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) throw std::runtime_error("socket() failed: " + std::to_string(WSAGetLastError()));
            BOOL reuse = TRUE;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                closesocket(s);
                throw std::runtime_error("bind() failed: " + std::to_string(WSAGetLastError()));
            }
            if (::listen(s, 1) != 0) {
                closesocket(s);
                throw std::runtime_error("listen() failed: " + std::to_string(WSAGetLastError()));
            }
            return TcpSocket(s);
        }

        TcpSocket accept_one() {
            SOCKET c = ::accept(sock_, nullptr, nullptr);
            if (c == INVALID_SOCKET) throw std::runtime_error("accept() failed: " + std::to_string(WSAGetLastError()));
            return TcpSocket(c);
        }

        bool send_all(const uint8_t* data, size_t len) noexcept {
            size_t sent = 0;
            while (sent < len) {
                int n = ::send(sock_, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0);
                if (n <= 0) return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        }

        // One raw recv() call -- may return fewer bytes than a caller wants,
        // by design: TLS record boundaries don't align with socket reads,
        // so callers accumulate into their own buffer across calls.
        bool recv_some(std::vector<uint8_t>& out) noexcept {
            char buf[8192];
            int n = ::recv(sock_, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            out.assign(buf, buf + n);
            return true;
        }

    private:
        SOCKET sock_ = INVALID_SOCKET;
    };

    // -----------------------------------------------------------------------
    // Certificate loading and chain verification (CryptoAPI / wincrypt.h)
    // -----------------------------------------------------------------------

    // A stateless functor deleter (rather than a bare function pointer)
    // keeps CertContextPtr default-constructible -- std::unique_ptr's
    // no-argument constructor is disabled for a function-pointer deleter
    // (it has no guaranteed-valid default value), but is fine for an
    // empty class type like this one.
    struct CertContextDeleter {
        void operator()(PCCERT_CONTEXT ctx) const noexcept {
            if (ctx) CertFreeCertificateContext(ctx);
        }
    };
    using CertContextPtr = std::unique_ptr<const CERT_CONTEXT, CertContextDeleter>;
    inline CertContextPtr make_cert_ptr(PCCERT_CONTEXT ctx) { return CertContextPtr(ctx); }

    // Loads a leaf certificate + its private key from a PFX file (see
    // generate_demo_certs.ps1) -- used for the server's own identity and
    // the client's own identity.
    //
    // The key is imported with CRYPT_USER_KEYSET (persisted to the calling
    // user's CNG/CAPI key store), not PKCS12_NO_PERSIST_KEY. That was a
    // deliberate correction, not the obvious choice: PKCS12_NO_PERSIST_KEY
    // (an in-memory-only ephemeral key, never touching disk) was tried
    // first as the safer-looking default, but was empirically found to
    // make AcquireCredentialsHandleW reject the resulting certificate with
    // SEC_E_UNKNOWN_CREDENTIALS on this platform/SDK -- reproduced with a
    // minimal isolated repro outside this header, independent of anything
    // else in animus_transport.hpp. delete_persisted_key() below removes
    // the key again once a SecureChannel is done with it, so a long-running
    // process (or repeated demo runs) doesn't accumulate key material in
    // the user's key store.
    inline CertContextPtr load_pfx_certificate(const std::wstring& pfx_path, const std::wstring& password) {
        std::ifstream file(pfx_path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open PFX file");
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty()) throw std::runtime_error("PFX file is empty");

        CRYPT_DATA_BLOB blob{};
        blob.cbData = static_cast<DWORD>(bytes.size());
        blob.pbData = reinterpret_cast<BYTE*>(bytes.data());

        HCERTSTORE store = PFXImportCertStore(&blob, password.c_str(), CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
        if (!store) throw std::runtime_error("PFXImportCertStore failed: " + hresult_hex(static_cast<long>(GetLastError())));

        PCCERT_CONTEXT cert = CertFindCertificateInStore(
            store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_HAS_PRIVATE_KEY, nullptr, nullptr);
        if (!cert) {
            CertCloseStore(store, 0);
            throw std::runtime_error("PFX contains no certificate with an attached private key");
        }
        CertContextPtr result = make_cert_ptr(CertDuplicateCertificateContext(cert));
        CertFreeCertificateContext(cert);
        CertCloseStore(store, 0);
        return result;
    }

    // Deletes the private key a load_pfx_certificate() call persisted to
    // the user's key store (CNG via NCryptDeleteKey, or legacy CAPI via
    // CryptAcquireContextW(..., CRYPT_DELETEKEYSET) as a fallback) --
    // pairs with load_pfx_certificate's CRYPT_USER_KEYSET so a demo/test
    // process doesn't accumulate key material run over run. Best-effort:
    // silently does nothing if the certificate has no private key or the
    // key was already removed.
    inline void delete_persisted_key(PCCERT_CONTEXT cert) noexcept {
        if (!cert) return;
        DWORD keySpec = 0;
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
        BOOL free_key = FALSE;
        if (!CryptAcquireCertificatePrivateKey(cert, CRYPT_ACQUIRE_SILENT_FLAG, nullptr, &key, &keySpec, &free_key)) {
            return;
        }
        if (keySpec == CERT_NCRYPT_KEY_SPEC) {
            NCryptDeleteKey(static_cast<NCRYPT_KEY_HANDLE>(key), 0); // also frees the handle
            return;
        }
        if (free_key) CryptReleaseContext(static_cast<HCRYPTPROV>(key), 0);

        DWORD size = 0;
        if (!CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size)) return;
        std::vector<BYTE> buf(size);
        auto* info = reinterpret_cast<CRYPT_KEY_PROV_INFO*>(buf.data());
        if (!CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, info, &size)) return;
        HCRYPTPROV prov = 0;
        CryptAcquireContextW(&prov, info->pwszContainerName, info->pwszProvName, info->dwProvType, CRYPT_DELETEKEYSET);
    }

    // Loads a DER-encoded CA certificate (see generate_demo_certs.ps1's
    // ca.cer, exported via Export-Certificate) -- no private key needed,
    // this is only ever used as a trust anchor.
    inline CertContextPtr load_cer_certificate(const std::wstring& cer_path) {
        std::ifstream file(cer_path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open CER file");
        std::vector<BYTE> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty()) throw std::runtime_error("CER file is empty");
        PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, bytes.data(), static_cast<DWORD>(bytes.size()));
        if (!cert) throw std::runtime_error("CertCreateCertificateContext failed: " + hresult_hex(static_cast<long>(GetLastError())));
        return make_cert_ptr(cert);
    }

    inline std::wstring get_subject_common_name(PCCERT_CONTEXT cert) {
        DWORD len = CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0,
            const_cast<LPSTR>(szOID_COMMON_NAME), nullptr, 0);
        if (len <= 1) return {};
        std::wstring name(len, L'\0');
        CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0,
            const_cast<LPSTR>(szOID_COMMON_NAME), name.data(), len);
        name.resize(std::wcslen(name.c_str()));
        return name;
    }

    inline bool certificate_has_eku(PCCERT_CONTEXT cert, LPCSTR oid) {
        DWORD size = 0;
        if (!CertGetEnhancedKeyUsage(cert, 0, nullptr, &size) || size == 0) return false;
        std::vector<BYTE> buf(size);
        auto* usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buf.data());
        if (!CertGetEnhancedKeyUsage(cert, 0, usage, &size)) return false;
        for (DWORD i = 0; i < usage->cUsageIdentifier; ++i) {
            if (std::strcmp(usage->rgpszUsageIdentifier[i], oid) == 0) return true;
        }
        return false;
    }

    // Wraps an in-memory certificate store holding exactly one trusted CA,
    // plus a CERT_CHAIN_ENGINE configured with that store as its EXCLUSIVE
    // root (CERT_CHAIN_ENGINE_CONFIG::hExclusiveRoot) -- chain verification
    // through this engine trusts only certificates issued by our own demo
    // CA, never anything already sitting in the Windows system Root store.
    // This is what makes SCH_CRED_MANUAL_CRED_VALIDATION (used by both the
    // client and server credentials below, so Schannel does not attempt
    // its own default trust check) safe: verify_certificate_chain() below
    // performs the real check that SCH_CRED_MANUAL_CRED_VALIDATION skipped.
    class TrustedRoot {
    public:
        explicit TrustedRoot(PCCERT_CONTEXT ca_cert) {
            store_ = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
            if (!store_) throw std::runtime_error("CertOpenStore(memory) failed");
            if (!CertAddCertificateContextToStore(store_, ca_cert, CERT_STORE_ADD_ALWAYS, nullptr)) {
                CertCloseStore(store_, 0);
                throw std::runtime_error("CertAddCertificateContextToStore failed");
            }
            CERT_CHAIN_ENGINE_CONFIG cfg{};
            cfg.cbSize = sizeof(cfg);
            cfg.hExclusiveRoot = store_;
            if (!CertCreateCertificateChainEngine(&cfg, &engine_)) {
                CertCloseStore(store_, 0);
                throw std::runtime_error("CertCreateCertificateChainEngine failed");
            }
        }
        ~TrustedRoot() {
            if (engine_) CertFreeCertificateChainEngine(engine_);
            if (store_) CertCloseStore(store_, 0);
        }
        TrustedRoot(const TrustedRoot&) = delete;
        TrustedRoot& operator=(const TrustedRoot&) = delete;

        HCERTCHAINENGINE engine() const noexcept { return engine_; }
        // Also used as CertGetCertificateChain's hAdditionalStore: our CA
        // is the leaf's issuer as well as its root (a two-tier demo chain),
        // so chain building needs it available both as a candidate issuer
        // and as the trusted root, not only the latter.
        HCERTSTORE store() const noexcept { return store_; }

    private:
        HCERTSTORE store_ = nullptr;
        HCERTCHAINENGINE engine_ = nullptr;
    };

    // Real chain-of-trust verification against `trust`'s exclusive CA --
    // NOT a stub. Deliberately out of scope for this demo-CA setup:
    // revocation checking (no CRL/OCSP endpoint exists for a private demo
    // CA) and full RFC 5280 policy constraints via
    // CertVerifyCertificateChainPolicy -- a production deployment with a
    // real internal CA should add both. required_eku, if non-null, must
    // also be present on the leaf (e.g. szOID_PKIX_KP_SERVER_AUTH /
    // szOID_PKIX_KP_CLIENT_AUTH) so a client cert can't be replayed as a
    // server cert or vice versa.
    inline bool verify_certificate_chain(PCCERT_CONTEXT cert, const TrustedRoot& trust, LPCSTR required_eku, std::string* error_out) {
        CERT_CHAIN_PARA chain_para{};
        chain_para.cbSize = sizeof(chain_para);

        PCCERT_CHAIN_CONTEXT chain = nullptr;
        BOOL ok = CertGetCertificateChain(trust.engine(), cert, nullptr, trust.store(), &chain_para,
            CERT_CHAIN_CACHE_END_CERT, nullptr, &chain);
        if (!ok || !chain) {
            if (error_out) *error_out = "CertGetCertificateChain failed: " + hresult_hex(static_cast<long>(GetLastError()));
            return false;
        }

        bool trusted = (chain->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR);
        if (!trusted && error_out) {
            *error_out = "certificate chain not trusted, dwErrorStatus=" + hresult_hex(static_cast<long>(chain->TrustStatus.dwErrorStatus));
        }
        CertFreeCertificateChain(chain);
        if (!trusted) return false;

        if (required_eku && !certificate_has_eku(cert, required_eku)) {
            if (error_out) *error_out = std::string("certificate missing required EKU: ") + required_eku;
            return false;
        }
        return true;
    }

    // Maps a verified peer certificate's subject CN to an AccessToken.
    // Only ever consulted AFTER verify_certificate_chain() has succeeded --
    // an unverified certificate's CN is not a trustworthy input.
    class CertificateIdentityMap {
    public:
        void add(const std::wstring& common_name, const security::AccessToken& token) {
            map_[common_name] = token;
        }

        bool resolve(PCCERT_CONTEXT cert, security::AccessToken& out) const {
            std::wstring cn = get_subject_common_name(cert);
            auto it = map_.find(cn);
            if (it == map_.end()) return false;
            out = it->second;
            return true;
        }

    private:
        std::unordered_map<std::wstring, security::AccessToken> map_;
    };

    // -----------------------------------------------------------------------
    // SecureChannel: one Schannel (SSPI) TLS 1.3 session, client or server
    // side, layered directly over a TcpSocket. Owns the CredHandle/CtxtHandle
    // for its whole lifetime; the destructor sends a close_notify and frees
    // both. Mutual authentication is mandatory in both directions: a client
    // must present a cert (ISC_REQ_USE_SUPPLIED_CREDS + a supplied cert), a
    // server requires one (ASC_REQ_MUTUAL_AUTH), and finish_handshake()
    // fails the connection if the peer's certificate cannot be retrieved
    // after the handshake completes.
    // -----------------------------------------------------------------------
    class SecureChannel {
    public:
        explicit SecureChannel(TcpSocket socket) noexcept : socket_(std::move(socket)) {
            SecInvalidateHandle(&cred_handle_);
            SecInvalidateHandle(&ctxt_handle_);
        }

        SecureChannel(const SecureChannel&) = delete;
        SecureChannel& operator=(const SecureChannel&) = delete;

        ~SecureChannel() {
            if (handshake_done_) send_close_notify();
            if (SecIsValidHandle(&ctxt_handle_)) DeleteSecurityContext(&ctxt_handle_);
            if (SecIsValidHandle(&cred_handle_)) FreeCredentialsHandle(&cred_handle_);
        }

        bool handshake_as_client(PCCERT_CONTEXT client_cert, const std::wstring& target_name) {
            is_server_ = false;
            // TLS 1.3's cipher suites are AEAD/HKDF-based and don't map onto
            // the legacy SCHANNEL_CRED::grbitEnabledProtocols allow-list
            // model (attempting to force SP_PROT_TLS1_3_CLIENT that way
            // fails AcquireCredentialsHandle with SEC_E_UNKNOWN_CREDENTIALS
            // / SEC_E_ALGORITHM_MISMATCH on this SDK) -- SCH_CREDENTIALS +
            // TLS_PARAMETERS::grbitDisabledProtocols (a DENY-list) is the
            // documented mechanism for TLS 1.3 with Schannel.
            TLS_PARAMETERS tls_params{};
            tls_params.grbitDisabledProtocols = static_cast<DWORD>(~SP_PROT_TLS1_3);

            PCCERT_CONTEXT certs[1] = { client_cert };
            SCH_CREDENTIALS cred{};
            cred.dwVersion = SCH_CREDENTIALS_VERSION;
            cred.dwCredFormat = SCH_CRED_FORMAT_CERT_CONTEXT;
            cred.cCreds = 1;
            cred.paCred = certs;
            // SCH_CRED_DISABLE_RECONNECTS: also stops the PEER (a server
            // we connect to) from issuing TLS 1.3 post-handshake
            // NewSessionTicket messages it otherwise would -- see the
            // matching comment on the server side below for why that
            // matters to every reader of this connection, not just this
            // client.
            cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_DISABLE_RECONNECTS;
            cred.cTlsParameters = 1;
            cred.pTlsParameters = &tls_params;

            TimeStamp expiry{};
            SECURITY_STATUS status = AcquireCredentialsHandleW(
                nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                nullptr, &cred, nullptr, nullptr, &cred_handle_, &expiry);
            if (status != SEC_E_OK) { last_error_ = "AcquireCredentialsHandle(client): " + hresult_hex(status); return false; }

            DWORD context_req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS;

            std::vector<uint8_t> in_buf;
            bool have_context = false;
            DWORD out_flags = 0;
            std::wstring target = target_name; // InitializeSecurityContextW takes a non-const pointer

            for (;;) {
                SecBuffer out_buf{}; out_buf.BufferType = SECBUFFER_TOKEN;
                SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, &out_buf };
                SECURITY_STATUS ss;

                if (!have_context) {
                    ss = InitializeSecurityContextW(&cred_handle_, nullptr, target.data(), context_req, 0, 0,
                        nullptr, 0, &ctxt_handle_, &out_desc, &out_flags, nullptr);
                }
                else {
                    SecBuffer in_bufs[2]{};
                    in_bufs[0].BufferType = SECBUFFER_TOKEN;
                    in_bufs[0].pvBuffer = in_buf.data();
                    in_bufs[0].cbBuffer = static_cast<unsigned long>(in_buf.size());
                    in_bufs[1].BufferType = SECBUFFER_EMPTY;
                    SecBufferDesc in_desc{ SECBUFFER_VERSION, 2, in_bufs };

                    ss = InitializeSecurityContextW(&cred_handle_, &ctxt_handle_, target.data(), context_req, 0, 0,
                        &in_desc, 0, nullptr, &out_desc, &out_flags, nullptr);

                    consume_input_buffer(in_buf, in_bufs[1], ss);
                }
                have_context = true;

                if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                    bool sent = socket_.send_all(reinterpret_cast<const uint8_t*>(out_buf.pvBuffer), out_buf.cbBuffer);
                    FreeContextBuffer(out_buf.pvBuffer);
                    if (!sent) { last_error_ = "socket send failed during client handshake"; return false; }
                }

                if (ss == SEC_E_OK) break;
                if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection during client handshake"; return false; }
                    in_buf.insert(in_buf.end(), chunk.begin(), chunk.end());
                    continue;
                }
                last_error_ = "InitializeSecurityContext: " + hresult_hex(ss);
                return false;
            }

            return finish_handshake();
        }

        bool handshake_as_server(PCCERT_CONTEXT server_cert) {
            is_server_ = true;
            // See handshake_as_client()'s comment: SCH_CREDENTIALS +
            // TLS_PARAMETERS::grbitDisabledProtocols, not legacy
            // SCHANNEL_CRED, is what actually negotiates TLS 1.3 here.
            TLS_PARAMETERS tls_params{};
            tls_params.grbitDisabledProtocols = static_cast<DWORD>(~SP_PROT_TLS1_3);

            PCCERT_CONTEXT certs[1] = { server_cert };
            SCH_CREDENTIALS cred{};
            cred.dwVersion = SCH_CREDENTIALS_VERSION;
            cred.dwCredFormat = SCH_CRED_FORMAT_CERT_CONTEXT;
            cred.cCreds = 1;
            cred.paCred = certs;
            // SCH_CRED_DISABLE_RECONNECTS suppresses TLS 1.3 session
            // resumption on this server credential -- in particular, the
            // automatic post-handshake NewSessionTicket message(s) a
            // Schannel TLS 1.3 server would otherwise send right after the
            // handshake completes. Those tickets are encrypted with the
            // application traffic key and arrive indistinguishable from a
            // real data record until DecryptMessage is called on them, at
            // which point it returns SEC_I_RENEGOTIATE instead of handing
            // back application data -- correctly reprocessing that status
            // (rather than just discarding the record) requires re-driving
            // {Initialize,Accept}SecurityContext with exactly the right
            // leftover bytes, which is finicky enough that a real attempt
            // at it here produced a genuine deadlock (SecureChannel is
            // symmetric for Phase 9 -- every connection's dialer both
            // sends a request AND blocks reading the response, so the two
            // ends can end up mutually waiting). Not issuing tickets in
            // the first place avoids the whole class of bug; recv_frame()/
            // decrypt_one_record() below still fail fast with a clear
            // error if SEC_I_RENEGOTIATE ever shows up anyway, rather than
            // silently hanging.
            cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_DISABLE_RECONNECTS;
            cred.cTlsParameters = 1;
            cred.pTlsParameters = &tls_params;

            TimeStamp expiry{};
            SECURITY_STATUS status = AcquireCredentialsHandleW(
                nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
                nullptr, &cred, nullptr, nullptr, &cred_handle_, &expiry);
            if (status != SEC_E_OK) { last_error_ = "AcquireCredentialsHandle(server): " + hresult_hex(status); return false; }

            DWORD context_req = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH;

            std::vector<uint8_t> in_buf;
            bool have_context = false;
            DWORD out_flags = 0;

            for (;;) {
                std::vector<uint8_t> chunk;
                if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection during server handshake"; return false; }
                in_buf.insert(in_buf.end(), chunk.begin(), chunk.end());

                for (;;) {
                    SecBuffer in_bufs[2]{};
                    in_bufs[0].BufferType = SECBUFFER_TOKEN;
                    in_bufs[0].pvBuffer = in_buf.data();
                    in_bufs[0].cbBuffer = static_cast<unsigned long>(in_buf.size());
                    in_bufs[1].BufferType = SECBUFFER_EMPTY;
                    SecBufferDesc in_desc{ SECBUFFER_VERSION, 2, in_bufs };

                    SecBuffer out_buf{}; out_buf.BufferType = SECBUFFER_TOKEN;
                    SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, &out_buf };

                    SECURITY_STATUS ss = AcceptSecurityContext(&cred_handle_,
                        have_context ? &ctxt_handle_ : nullptr, &in_desc, context_req, 0,
                        &ctxt_handle_, &out_desc, &out_flags, nullptr);
                    have_context = true;

                    bool had_extra = consume_input_buffer(in_buf, in_bufs[1], ss);

                    if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                        bool sent = socket_.send_all(reinterpret_cast<const uint8_t*>(out_buf.pvBuffer), out_buf.cbBuffer);
                        FreeContextBuffer(out_buf.pvBuffer);
                        if (!sent) { last_error_ = "socket send failed during server handshake"; return false; }
                    }

                    if (ss == SEC_E_OK) return finish_handshake();
                    if (ss == SEC_I_CONTINUE_NEEDED) {
                        if (had_extra) continue; // more buffered handshake data already in hand
                        break; // need more bytes from the socket
                    }
                    if (ss == SEC_E_INCOMPLETE_MESSAGE) break; // need more bytes from the socket
                    last_error_ = "AcceptSecurityContext: " + hresult_hex(ss);
                    return false;
                }
            }
        }

        bool send_frame(const WireFrame& frame) noexcept {
            if (!handshake_done_) return false;
            std::vector<uint8_t> io_buf(stream_sizes_.cbHeader + sizeof(WireFrame) + stream_sizes_.cbTrailer);
            std::memcpy(io_buf.data() + stream_sizes_.cbHeader, &frame, sizeof(WireFrame));

            SecBuffer bufs[4]{};
            bufs[0] = { stream_sizes_.cbHeader, SECBUFFER_STREAM_HEADER, io_buf.data() };
            bufs[1] = { static_cast<unsigned long>(sizeof(WireFrame)), SECBUFFER_DATA, io_buf.data() + stream_sizes_.cbHeader };
            bufs[2] = { stream_sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, io_buf.data() + stream_sizes_.cbHeader + sizeof(WireFrame) };
            bufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
            SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

            SECURITY_STATUS ss = EncryptMessage(&ctxt_handle_, 0, &desc, 0);
            if (ss != SEC_E_OK) { last_error_ = "EncryptMessage: " + hresult_hex(ss); return false; }

            size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
            return socket_.send_all(io_buf.data(), total);
        }

        // -------------------------------------------------------------
        // Generic variable-length message framing, layered on the same
        // EncryptMessage/DecryptMessage record plumbing as send_frame/
        // recv_frame above but not tied to the fixed-size WireFrame --
        // Phase 9's cluster RPCs (RequestVote/AppendEntries) carry a
        // variable number of log entries, so a fixed struct doesn't fit.
        // Wire shape: a 4-byte little-endian length prefix, itself sent
        // as its own TLS record, followed by that many payload bytes
        // (chunked into further records if larger than one TLS record's
        // max message size). recv_message() reassembles across however
        // many DecryptMessage calls that takes, buffering leftover
        // plaintext in plaintext_buf_ for the next call.
        // -------------------------------------------------------------
        bool send_message(const std::vector<uint8_t>& payload) noexcept {
            uint32_t len = static_cast<uint32_t>(payload.size());
            uint8_t hdr[4];
            std::memcpy(hdr, &len, 4);
            if (!send_raw(hdr, 4)) return false;
            if (payload.empty()) return true;
            return send_raw(payload.data(), payload.size());
        }

        bool recv_message(std::vector<uint8_t>& payload) noexcept {
            std::vector<uint8_t> hdr;
            if (!recv_exact(4, hdr)) return false;
            uint32_t len;
            std::memcpy(&len, hdr.data(), 4);
            if (len > (64u * 1024 * 1024)) { last_error_ = "recv_message: implausible length prefix"; return false; }
            return recv_exact(len, payload);
        }

        bool recv_frame(WireFrame& out) noexcept {
            if (!handshake_done_) return false;
            for (;;) {
                // Only pull more bytes off the socket when recv_buf_ can't
                // already satisfy a decrypt -- a single recv_some() often
                // returns several whole TLS records at once (loopback TCP
                // delivers in bursts), and the leftover SECBUFFER_EXTRA
                // tail from the previous recv_frame() call may already be
                // a complete record. Blocking on recv_some() unconditionally
                // here would stall waiting for bytes the peer has no more
                // of to send once it has finished and closed the socket,
                // even though the last frame(s) are already sitting in
                // recv_buf_ undecrypted.
                if (recv_buf_.empty()) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection"; return false; }
                    recv_buf_.insert(recv_buf_.end(), chunk.begin(), chunk.end());
                }

                SecBuffer bufs[4]{};
                bufs[0] = { static_cast<unsigned long>(recv_buf_.size()), SECBUFFER_DATA, recv_buf_.data() };
                bufs[1] = { 0, SECBUFFER_EMPTY, nullptr };
                bufs[2] = { 0, SECBUFFER_EMPTY, nullptr };
                bufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
                SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

                SECURITY_STATUS ss = DecryptMessage(&ctxt_handle_, &desc, 0, nullptr);
                if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection (incomplete message)"; return false; }
                    recv_buf_.insert(recv_buf_.end(), chunk.begin(), chunk.end());
                    continue;
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) { last_error_ = "peer sent close_notify"; return false; }
                if (ss == SEC_I_RENEGOTIATE) {
                    // Both handshake_as_client() and handshake_as_server()
                    // set SCH_CRED_DISABLE_RECONNECTS specifically so this
                    // should never happen (see the comment there for the
                    // full story: it stops the peer from ever sending the
                    // TLS 1.3 post-handshake message that would otherwise
                    // trigger this). Fail fast with a clear diagnostic
                    // rather than attempt to reprocess it -- an earlier,
                    // more "helpful" attempt at reprocessing it here
                    // produced a genuine deadlock under Phase 9's
                    // symmetric (both-sides-send-and-receive) connections.
                    last_error_ = "unexpected SEC_I_RENEGOTIATE (a peer sent a post-handshake TLS message; SCH_CRED_DISABLE_RECONNECTS should have prevented this)";
                    return false;
                }
                if (ss != SEC_E_OK) { last_error_ = "DecryptMessage: " + hresult_hex(ss); return false; }

                uint8_t* data_ptr = nullptr; unsigned long data_len = 0;
                uint8_t* extra_ptr = nullptr; unsigned long extra_len = 0;
                for (const SecBuffer& b : bufs) {
                    if (b.BufferType == SECBUFFER_DATA && !data_ptr) { data_ptr = static_cast<uint8_t*>(b.pvBuffer); data_len = b.cbBuffer; }
                    if (b.BufferType == SECBUFFER_EXTRA) { extra_ptr = static_cast<uint8_t*>(b.pvBuffer); extra_len = b.cbBuffer; }
                }
                if (!data_ptr || data_len != sizeof(WireFrame)) { last_error_ = "unexpected decrypted frame size"; return false; }
                std::memcpy(&out, data_ptr, sizeof(WireFrame));

                recv_buf_ = (extra_ptr && extra_len > 0)
                    ? std::vector<uint8_t>(extra_ptr, extra_ptr + extra_len)
                    : std::vector<uint8_t>();
                return true;
            }
        }

        // Valid only after a successful handshake; the channel retains
        // ownership (caller must not CertFreeCertificateContext it).
        PCCERT_CONTEXT peer_certificate() const noexcept { return peer_cert_.get(); }
        DWORD negotiated_protocol() const noexcept { return negotiated_protocol_; }
        const char* negotiated_protocol_name() const noexcept {
            if (negotiated_protocol_ & SP_PROT_TLS1_3) return "TLS 1.3";
            if (negotiated_protocol_ & SP_PROT_TLS1_2) return "TLS 1.2";
            return "unknown";
        }
        const std::string& last_error() const noexcept { return last_error_; }

        // Forcibly aborts this channel's underlying socket from a thread
        // OTHER than the one that owns/uses this channel -- unblocks
        // whatever recv_some()/send_all() call the owning thread may
        // currently be parked in (it observes a socket error and the
        // in-flight send_message/recv_message call returns false), since
        // that thread has no other way to learn the channel is being torn
        // down while blocked on a peer that will never send it anything
        // else. Used by animus_cluster.hpp's RaftNode::stop() to tear down
        // still-open inbound connections' handler threads on shutdown --
        // without this, stopping a node whose peers are still sending it
        // requests would hang forever joining those threads.
        void force_close() noexcept { socket_.close(); }

    private:
        // Encrypts and sends an arbitrary-length buffer as one or more TLS
        // records (each capped at stream_sizes_.cbMaximumMessage plaintext
        // bytes) -- the record-boundary bookkeeping needed to reassemble
        // these on the far side is decrypt_one_record()/recv_exact()'s job,
        // not this function's.
        bool send_raw(const uint8_t* data, size_t len) noexcept {
            if (!handshake_done_) return false;
            size_t max_msg = stream_sizes_.cbMaximumMessage ? stream_sizes_.cbMaximumMessage : len;
            if (max_msg == 0) max_msg = len ? len : 1;
            size_t offset = 0;
            while (offset < len) {
                size_t chunk = (len - offset < max_msg) ? (len - offset) : max_msg;
                std::vector<uint8_t> io_buf(stream_sizes_.cbHeader + chunk + stream_sizes_.cbTrailer);
                std::memcpy(io_buf.data() + stream_sizes_.cbHeader, data + offset, chunk);

                SecBuffer bufs[4]{};
                bufs[0] = { stream_sizes_.cbHeader, SECBUFFER_STREAM_HEADER, io_buf.data() };
                bufs[1] = { static_cast<unsigned long>(chunk), SECBUFFER_DATA, io_buf.data() + stream_sizes_.cbHeader };
                bufs[2] = { stream_sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, io_buf.data() + stream_sizes_.cbHeader + chunk };
                bufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
                SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

                SECURITY_STATUS ss = EncryptMessage(&ctxt_handle_, 0, &desc, 0);
                if (ss != SEC_E_OK) { last_error_ = "EncryptMessage: " + hresult_hex(ss); return false; }

                size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
                if (!socket_.send_all(io_buf.data(), total)) return false;
                offset += chunk;
            }
            return true;
        }

        // Decrypts exactly one TLS record (blocking on the socket for more
        // bytes as needed) and appends its plaintext to plaintext_buf_.
        bool decrypt_one_record() noexcept {
            for (;;) {
                if (recv_buf_.empty()) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection"; return false; }
                    recv_buf_.insert(recv_buf_.end(), chunk.begin(), chunk.end());
                }

                SecBuffer bufs[4]{};
                bufs[0] = { static_cast<unsigned long>(recv_buf_.size()), SECBUFFER_DATA, recv_buf_.data() };
                bufs[1] = { 0, SECBUFFER_EMPTY, nullptr };
                bufs[2] = { 0, SECBUFFER_EMPTY, nullptr };
                bufs[3] = { 0, SECBUFFER_EMPTY, nullptr };
                SecBufferDesc desc{ SECBUFFER_VERSION, 4, bufs };

                SECURITY_STATUS ss = DecryptMessage(&ctxt_handle_, &desc, 0, nullptr);
                if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                    std::vector<uint8_t> chunk;
                    if (!socket_.recv_some(chunk)) { last_error_ = "peer closed connection (incomplete message)"; return false; }
                    recv_buf_.insert(recv_buf_.end(), chunk.begin(), chunk.end());
                    continue;
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) { last_error_ = "peer sent close_notify"; return false; }
                if (ss == SEC_I_RENEGOTIATE) {
                    // TLS 1.3 has no real in-band renegotiation; Schannel
                    // reuses this status to mean "I decrypted a
                    // non-application-data protocol record" -- in practice
                    // almost always a server's post-handshake
                    // NewSessionTicket message. Phase 8's fixed-direction
                    // demo (server only ever recv_frame()s, client only
                    // ever send_frame()s) never triggered this because the
                    // client -- the side a ticket actually arrives at --
                    // never calls a receive function; Phase 9's cluster
                    // RPCs are bidirectional per connection (the dialing
                    // side sends a request AND receives the response), so
                    // it hit this on essentially every first RPC. An
                    // earlier attempt at reprocessing the leftover bytes
                    // via {Initialize,Accept}SecurityContext here -- rather
                    // than treating this as a hard failure -- produced a
                    // genuine deadlock (each side blocked waiting on the
                    // other). Both handshake_as_client() and
                    // handshake_as_server() now set
                    // SCH_CRED_DISABLE_RECONNECTS, which stops the peer
                    // from ever sending a session ticket, so this code path
                    // should not be reachable in practice -- it fails fast
                    // rather than silently hanging if it somehow is.
                    last_error_ = "unexpected SEC_I_RENEGOTIATE (a peer sent a post-handshake TLS message; SCH_CRED_DISABLE_RECONNECTS should have prevented this)";
                    return false;
                }
                if (ss != SEC_E_OK) { last_error_ = "DecryptMessage: " + hresult_hex(ss); return false; }

                uint8_t* data_ptr = nullptr; unsigned long data_len = 0;
                uint8_t* extra_ptr = nullptr; unsigned long extra_len = 0;
                for (const SecBuffer& b : bufs) {
                    if (b.BufferType == SECBUFFER_DATA && !data_ptr) { data_ptr = static_cast<uint8_t*>(b.pvBuffer); data_len = b.cbBuffer; }
                    if (b.BufferType == SECBUFFER_EXTRA) { extra_ptr = static_cast<uint8_t*>(b.pvBuffer); extra_len = b.cbBuffer; }
                }
                if (data_ptr && data_len > 0) plaintext_buf_.insert(plaintext_buf_.end(), data_ptr, data_ptr + data_len);

                recv_buf_ = (extra_ptr && extra_len > 0)
                    ? std::vector<uint8_t>(extra_ptr, extra_ptr + extra_len)
                    : std::vector<uint8_t>();
                return true;
            }
        }

        // Blocks until at least n bytes of decrypted plaintext are
        // available (decrypting further records as needed), then hands
        // back exactly n bytes and keeps any remainder buffered.
        bool recv_exact(size_t n, std::vector<uint8_t>& out) noexcept {
            while (plaintext_buf_.size() < n) {
                if (!decrypt_one_record()) return false;
            }
            out.assign(plaintext_buf_.begin(), plaintext_buf_.begin() + n);
            plaintext_buf_.erase(plaintext_buf_.begin(), plaintext_buf_.begin() + n);
            return true;
        }

        // After AcceptSecurityContext/InitializeSecurityContext, checks
        // whether the trailing input buffer was retagged SECBUFFER_EXTRA
        // (unconsumed bytes belonging to the *next* handshake message,
        // already in hand) and if so keeps just those bytes; otherwise
        // clears the buffer once its contents have been fully consumed.
        // Returns true iff there is leftover SECBUFFER_EXTRA data.
        static bool consume_input_buffer(std::vector<uint8_t>& in_buf, const SecBuffer& trailing, SECURITY_STATUS ss) {
            if (trailing.BufferType == SECBUFFER_EXTRA && trailing.cbBuffer > 0) {
                std::vector<uint8_t> extra(in_buf.end() - trailing.cbBuffer, in_buf.end());
                in_buf = std::move(extra);
                return true;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                in_buf.clear();
            }
            return false;
        }

        bool finish_handshake() {
            SECURITY_STATUS ss = QueryContextAttributesW(&ctxt_handle_, SECPKG_ATTR_STREAM_SIZES, &stream_sizes_);
            if (ss != SEC_E_OK) { last_error_ = "QueryContextAttributes(STREAM_SIZES): " + hresult_hex(ss); return false; }

            SecPkgContext_ConnectionInfo conn_info{};
            ss = QueryContextAttributesW(&ctxt_handle_, SECPKG_ATTR_CONNECTION_INFO, &conn_info);
            negotiated_protocol_ = (ss == SEC_E_OK) ? conn_info.dwProtocol : 0;

            PCCERT_CONTEXT peer = nullptr;
            ss = QueryContextAttributesW(&ctxt_handle_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &peer);
            if (ss != SEC_E_OK || !peer) {
                last_error_ = "peer did not present a verifiable certificate (mutual auth not satisfied): " + hresult_hex(ss);
                return false;
            }
            peer_cert_ = make_cert_ptr(peer);
            handshake_done_ = true;
            return true;
        }

        void send_close_notify() noexcept {
            DWORD type = SCHANNEL_SHUTDOWN;
            SecBuffer ctrl_buf{ sizeof(type), SECBUFFER_TOKEN, &type };
            SecBufferDesc ctrl_desc{ SECBUFFER_VERSION, 1, &ctrl_buf };
            ApplyControlToken(&ctxt_handle_, &ctrl_desc);

            SecBuffer out_buf{}; out_buf.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc out_desc{ SECBUFFER_VERSION, 1, &out_buf };
            DWORD out_flags = 0;
            SECURITY_STATUS ss;
            if (is_server_) {
                DWORD context_req = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                    ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH;
                ss = AcceptSecurityContext(&cred_handle_, &ctxt_handle_, nullptr, context_req, 0,
                    &ctxt_handle_, &out_desc, &out_flags, nullptr);
            }
            else {
                DWORD context_req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                    ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
                wchar_t empty_target[] = L"";
                ss = InitializeSecurityContextW(&cred_handle_, &ctxt_handle_, empty_target, context_req, 0, 0,
                    nullptr, 0, nullptr, &out_desc, &out_flags, nullptr);
            }
            if (ss == SEC_E_OK && out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
                socket_.send_all(reinterpret_cast<const uint8_t*>(out_buf.pvBuffer), out_buf.cbBuffer);
                FreeContextBuffer(out_buf.pvBuffer);
            }
        }

        TcpSocket socket_;
        CredHandle cred_handle_{};
        CtxtHandle ctxt_handle_{};
        bool is_server_ = false;
        bool handshake_done_ = false;
        SecPkgContext_StreamSizes stream_sizes_{};
        DWORD negotiated_protocol_ = 0;
        CertContextPtr peer_cert_;
        std::vector<uint8_t> recv_buf_;
        std::vector<uint8_t> plaintext_buf_;
        std::string last_error_;
    };

} // namespace transport
} // namespace animus
#endif // defined(_WIN32) && defined(_MSC_VER) -- end of animus_transport.hpp

// -------------------------------------------------------------------------
// animus_cluster.hpp -- Phase 9: Distributed Cloud Orchestration & Clustering (Raft-lite over mTLS)
// -------------------------------------------------------------------------
#if defined(_WIN32) && defined(_MSC_VER)
// Phase 9: Distributed Cloud Orchestration & Clustering.
//
// Windows-only: layered directly on animus_transport.hpp's Schannel mTLS
// SecureChannel, per the explicit Phase 9 scoping decision -- real gRPC +
// Protobuf would be this project's first external build dependency, so
// inter-node RPC here is a small hand-rolled binary protocol (fixed-size
// structs + a length-prefixed message envelope, see
// SecureChannel::send_message/recv_message) reusing the same mTLS
// transport, RBAC-cert machinery, and "zero external dependency" property
// established in Phase 8, rather than a parallel wire stack.
//
// What's replicated: control-plane commands (currently just AddRule) that
// every node's local animus::Engine must apply identically, not telemetry
// itself -- each node keeps ingesting/processing its own telemetry locally
// on the existing zero-copy hot path (animus.hpp); only the *rule set* that
// hot path evaluates against goes through consensus, so cluster membership
// changes cost nothing on the ingestion fast path.
//
// This is deliberately a "lite" Raft (see the whitepaper term used in the
// Phase 9 request): in-memory-only log/term state (no durable storage, so
// a node that restarts loses its Raft state -- acceptable for a demo,
// would need a WAL for production), no log compaction/snapshotting, and
// blocking sockets with no per-RPC timeout (a cleanly-stopped peer fails
// fast because its listening socket closes; a hung-but-open peer would
// stall a round rather than failing over promptly -- a real deployment
// would want send/recv timeouts here). The core algorithm itself --
// randomized election timeouts, term-based safety, log-consistency-checked
// replication with conflict truncation, and the "only commit entries from
// the leader's own current term" safety rule -- is real Raft, not a stub.
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace animus {
namespace cluster {

    using NodeId = uint32_t;

    // The one command type this "lite" cluster replicates today: register a
    // detection rule on every node's local engine. Fixed-size POD so log
    // entries need no variable-length field serialization beyond the
    // entry-count prefix in AppendEntriesArgs below.
#pragma pack(push, 1)
    struct AddRuleCommand {
        uint32_t rule_id;
        uint32_t event_id;
        uint64_t threshold;
        uint8_t comparator;
        uint32_t severity;
    };

    // is_noop marks a leader-election no-op entry (see start_election()'s
    // success branch) rather than a real AddRuleCommand -- apply_committed_
    // entries_locked() skips calling engine_.add_rule() for these. Needed
    // because of a genuine Raft subtlety (the paper's Section 5.4.2): a
    // leader can never advance commit_index_ by counting replicas of an
    // entry from a PREVIOUS term, only its own -- so a brand-new leader
    // that inherits an already-majority-replicated-but-not-yet-committed
    // entry from the previous leader's term can get permanently stuck
    // unable to commit it if nothing new is ever proposed through the new
    // leader. Appending one no-op entry in the new leader's own term as
    // the first thing it does, then replicating it, guarantees there is
    // always a current-term entry whose commitment (once it reaches a
    // majority) carries every earlier, still-pending entry forward with
    // it -- this was found and fixed after reproducing exactly this
    // stall in a real 3-node failover run (10 runs; ~20% hit it).
    struct LogEntry {
        uint64_t term;
        uint8_t is_noop;
        AddRuleCommand command;
    };
#pragma pack(pop)

    enum class Role : uint8_t { Follower = 0, Candidate = 1, Leader = 2 };

    inline const char* role_name(Role r) noexcept {
        switch (r) {
        case Role::Follower: return "Follower";
        case Role::Candidate: return "Candidate";
        case Role::Leader: return "Leader";
        }
        return "?";
    }

    enum class ProposeResult : uint8_t { Ok = 0, NotLeader = 1, Failed = 2 };

    // Static cluster membership: every node knows every peer's id, address,
    // and expected certificate CN up front (no dynamic membership changes
    // in this "lite" implementation).
    struct PeerConfig {
        NodeId id;
        std::string host;
        uint16_t port;
        std::wstring cert_cn;
    };

    namespace wire {
        enum class MsgType : uint8_t {
            RequestVoteReq = 1,
            RequestVoteResp = 2,
            AppendEntriesReq = 3,
            AppendEntriesResp = 4,
        };

#pragma pack(push, 1)
        struct RequestVoteArgs {
            uint64_t term;
            uint32_t candidate_id;
            uint64_t last_log_index;
            uint64_t last_log_term;
        };
        struct RequestVoteResult {
            uint64_t term;
            uint8_t vote_granted;
        };
        struct AppendEntriesHeader {
            uint64_t term;
            uint32_t leader_id;
            uint64_t prev_log_index;
            uint64_t prev_log_term;
            uint64_t leader_commit;
            uint32_t entry_count;
        };
        struct AppendEntriesResult {
            uint64_t term;
            uint8_t success;
            uint64_t match_index;
        };
#pragma pack(pop)

        template <typename T>
        std::vector<uint8_t> pack(MsgType type, const T& body) {
            std::vector<uint8_t> out(1 + sizeof(T));
            out[0] = static_cast<uint8_t>(type);
            std::memcpy(out.data() + 1, &body, sizeof(T));
            return out;
        }

        inline std::vector<uint8_t> pack_append_entries(const AppendEntriesHeader& hdr, const std::vector<LogEntry>& entries) {
            std::vector<uint8_t> out(1 + sizeof(AppendEntriesHeader) + entries.size() * sizeof(LogEntry));
            out[0] = static_cast<uint8_t>(MsgType::AppendEntriesReq);
            std::memcpy(out.data() + 1, &hdr, sizeof(AppendEntriesHeader));
            if (!entries.empty()) {
                std::memcpy(out.data() + 1 + sizeof(AppendEntriesHeader), entries.data(), entries.size() * sizeof(LogEntry));
            }
            return out;
        }
    } // namespace wire

    // One persistent, mTLS-authenticated, direction-specific link to a
    // single peer: this node dials the peer and is the ONLY side that
    // issues RPC requests over the resulting connection (the peer's
    // symmetric outbound connection back to this node carries requests the
    // other way). This avoids needing to multiplex concurrent requests
    // from both ends over one socket.
    class PeerLink {
    public:
        std::mutex mtx;
        std::unique_ptr<transport::SecureChannel> channel;
    };

    class RaftNode {
    public:
        RaftNode(NodeId id, std::vector<PeerConfig> peers, transport::CertContextPtr my_cert,
            transport::TrustedRoot& trust, animus::Engine& engine, uint16_t listen_port)
            : id_(id), peers_(std::move(peers)), my_cert_(std::move(my_cert)), trust_(trust),
            engine_(engine), listen_port_(listen_port), rng_(std::random_device{}()) {
            for (const auto& p : peers_) cn_to_node_id_[p.cert_cn] = p.id;
        }

        ~RaftNode() { stop(); }

        RaftNode(const RaftNode&) = delete;
        RaftNode& operator=(const RaftNode&) = delete;

        void start() {
            if (running_.exchange(true)) return;
            reset_election_deadline();
            listener_thread_ = std::thread([this] { listener_loop(); });
            tick_thread_ = std::thread([this] { tick_loop(); });
        }

        // Closes the listener and every peer connection, then joins all
        // background threads. Used both for orderly shutdown and to
        // simulate a node failure in the failover demo -- a peer trying to
        // reach a stopped node's closed listener socket fails fast
        // (connection refused) rather than hanging.
        void stop() {
            if (!running_.exchange(false)) return;
            {
                std::lock_guard<std::mutex> lock(listener_mutex_);
                listener_socket_.close();
            }
            {
                std::lock_guard<std::mutex> lock(links_mutex_);
                for (auto& kv : links_) {
                    std::lock_guard<std::mutex> lk(kv.second->mtx);
                    kv.second->channel.reset();
                }
            }
            {
                // Force-close every still-open INBOUND connection too --
                // closing only the listener stops new peers from
                // connecting, but an already-accepted handler thread is
                // blocked in recv_message() on its own socket, which the
                // listener close does nothing to. See the comment at
                // inbound_channels_'s use-site in listener_loop().
                std::lock_guard<std::mutex> lock(handlers_mutex_);
                for (auto& ch : inbound_channels_) ch->force_close();
            }
            if (tick_thread_.joinable()) tick_thread_.join();
            if (listener_thread_.joinable()) listener_thread_.join();
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            for (auto& t : handler_threads_) if (t.joinable()) t.join();
            handler_threads_.clear();
            inbound_channels_.clear();
        }

        Role role() const noexcept { return role_.load(); }
        bool is_leader() const noexcept { return role_.load() == Role::Leader; }
        uint64_t current_term() const {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return current_term_;
        }
        NodeId leader_hint() const {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return has_leader_ ? current_leader_ : 0;
        }
        size_t log_size() const {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return log_.size();
        }
        uint64_t committed_count() const {
            std::lock_guard<std::mutex> lock(state_mutex_);
            return commit_index_;
        }

        // Leader-only: appends the command to the local log, replicates it
        // to a majority, and blocks (bounded by timeout_ms) until it is
        // committed and applied to this node's own engine. Followers
        // reject immediately with NotLeader + a hint at who the leader is
        // (if known), mirroring how a real Raft-backed client library
        // redirects a write to the current leader instead of retrying
        // blindly.
        ProposeResult propose(const AddRuleCommand& cmd, NodeId* leader_hint_out, int timeout_ms = 3000) {
            size_t my_index;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (role_.load() != Role::Leader) {
                    if (leader_hint_out) *leader_hint_out = has_leader_ ? current_leader_ : 0;
                    return ProposeResult::NotLeader;
                }
                log_.push_back(LogEntry{ current_term_, /*is_noop=*/0, cmd });
                my_index = log_.size();
            }
            replicate_to_all_peers();

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (commit_index_ >= my_index) return ProposeResult::Ok;
                    if (role_.load() != Role::Leader) {
                        if (leader_hint_out) *leader_hint_out = has_leader_ ? current_leader_ : 0;
                        return ProposeResult::NotLeader;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return ProposeResult::Failed;
        }

    private:
        // ------------------------------------------------------------
        // Networking: listener + per-connection server handler + a
        // lazily-(re)connected outbound PeerLink per peer.
        // ------------------------------------------------------------
        void listener_loop() {
            transport::TcpSocket sock;
            try {
                sock = transport::TcpSocket::listen_on(listen_port_);
            }
            catch (const std::exception&) {
                running_.store(false);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(listener_mutex_);
                listener_socket_ = std::move(sock);
            }
            while (running_.load()) {
                transport::TcpSocket accepted;
                try {
                    accepted = listener_socket_.accept_one();
                }
                catch (const std::exception&) {
                    break; // listener socket closed by stop()
                }
                // The channel is heap-allocated and registered in
                // inbound_channels_ (rather than owned locally inside the
                // handler thread) so that stop() -- running on a different
                // thread -- can reach in and force_close() it: a handler
                // thread blocked in recv_message() on a peer that has
                // nothing more to send has no other way to learn the node
                // is shutting down, and stop()'s handler_threads_ join
                // would otherwise hang forever waiting for it.
                auto channel = std::make_shared<transport::SecureChannel>(std::move(accepted));
                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    inbound_channels_.push_back(channel);
                    handler_threads_.emplace_back([this, channel] { handle_inbound(*channel); });
                }
            }
        }

        void handle_inbound(transport::SecureChannel& channel) {
            if (!channel.handshake_as_server(my_cert_.get())) return;

            std::string err;
            if (!transport::verify_certificate_chain(channel.peer_certificate(), trust_, nullptr, &err)) return;

            std::wstring peer_cn = transport::get_subject_common_name(channel.peer_certificate());
            auto it = cn_to_node_id_.find(peer_cn);
            if (it == cn_to_node_id_.end()) return; // unrecognized peer identity, refuse
            NodeId peer_id = it->second;

            for (;;) {
                std::vector<uint8_t> req;
                if (!channel.recv_message(req)) return;
                if (req.empty()) return;
                std::vector<uint8_t> resp = dispatch_request(peer_id, req);
                if (resp.empty() || !channel.send_message(resp)) return;
            }
        }

        std::vector<uint8_t> dispatch_request(NodeId from, const std::vector<uint8_t>& req) {
            auto type = static_cast<wire::MsgType>(req[0]);
            if (type == wire::MsgType::RequestVoteReq && req.size() == 1 + sizeof(wire::RequestVoteArgs)) {
                wire::RequestVoteArgs args;
                std::memcpy(&args, req.data() + 1, sizeof(args));
                wire::RequestVoteResult result = handle_request_vote(from, args);
                return wire::pack(wire::MsgType::RequestVoteResp, result);
            }
            if (type == wire::MsgType::AppendEntriesReq && req.size() >= 1 + sizeof(wire::AppendEntriesHeader)) {
                wire::AppendEntriesHeader hdr;
                std::memcpy(&hdr, req.data() + 1, sizeof(hdr));
                // Validate the claimed entry count against the message we
                // actually received BEFORE sizing a vector off it -- a
                // corrupt or hostile entry_count must not drive an
                // unbounded allocation.
                size_t expected = 1 + sizeof(hdr) + static_cast<size_t>(hdr.entry_count) * sizeof(LogEntry);
                if (req.size() != expected) return {};
                std::vector<LogEntry> entries(hdr.entry_count);
                if (hdr.entry_count > 0) {
                    std::memcpy(entries.data(), req.data() + 1 + sizeof(hdr), entries.size() * sizeof(LogEntry));
                }
                wire::AppendEntriesResult result = handle_append_entries(from, hdr, entries);
                return wire::pack(wire::MsgType::AppendEntriesResp, result);
            }
            return {};
        }

        PeerLink& link_for(NodeId peer_id) {
            std::lock_guard<std::mutex> lock(links_mutex_);
            auto it = links_.find(peer_id);
            if (it != links_.end()) return *it->second;
            auto link = std::make_unique<PeerLink>();
            PeerLink& ref = *link;
            links_.emplace(peer_id, std::move(link));
            return ref;
        }

        const PeerConfig* peer_config(NodeId peer_id) const {
            for (const auto& p : peers_) if (p.id == peer_id) return &p;
            return nullptr;
        }

        // Sends [1 request byte + payload] to peer_id over its dedicated
        // outbound link, (re)connecting and mTLS-handshaking first if
        // necessary, and verifying the peer's certificate resolves to the
        // exact node id we intended to call -- not merely "some trusted
        // node" -- before trusting the response.
        bool call_rpc(NodeId peer_id, const std::vector<uint8_t>& request, std::vector<uint8_t>& response) {
            const PeerConfig* cfg = peer_config(peer_id);
            if (!cfg) return false;
            PeerLink& link = link_for(peer_id);
            std::lock_guard<std::mutex> lock(link.mtx);

            if (!link.channel) {
                try {
                    // A short connect timeout matters much more here than
                    // in Phase 8's client/server demo: a dead cluster peer
                    // is a NORMAL, expected condition every election round
                    // has to route around (see connect_to()'s comment in
                    // animus_transport.hpp for why the OS's default
                    // timeout is unsuitable), and it must fail well within
                    // a single election round rather than stall the whole
                    // round's worker-thread join.
                    transport::TcpSocket sock = transport::TcpSocket::connect_to(cfg->host, cfg->port, /*timeout_ms=*/200);
                    auto ch = std::make_unique<transport::SecureChannel>(std::move(sock));
                    if (!ch->handshake_as_client(my_cert_.get(), L"localhost")) return false;
                    std::string err;
                    if (!transport::verify_certificate_chain(ch->peer_certificate(), trust_, nullptr, &err)) return false;
                    if (transport::get_subject_common_name(ch->peer_certificate()) != cfg->cert_cn) return false;
                    link.channel = std::move(ch);
                }
                catch (const std::exception&) {
                    return false;
                }
            }

            if (!link.channel->send_message(request) || !link.channel->recv_message(response)) {
                link.channel.reset(); // drop a broken link; next call reconnects
                return false;
            }
            return true;
        }

        // ------------------------------------------------------------
        // Raft algorithm. All access to term/log/commit state goes
        // through state_mutex_; network I/O in call_rpc() above always
        // happens with state_mutex_ NOT held, so a slow/unreachable peer
        // blocks only the thread calling it, never the whole node.
        // ------------------------------------------------------------
        uint64_t last_log_term_locked() const {
            return log_.empty() ? 0 : log_.back().term;
        }

        wire::RequestVoteResult handle_request_vote(NodeId candidate_id, const wire::RequestVoteArgs& args) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (args.term > current_term_) {
                current_term_ = args.term;
                role_.store(Role::Follower);
                has_voted_this_term_ = false;
            }
            bool vote_granted = false;
            if (args.term >= current_term_) {
                bool log_ok = (args.last_log_term > last_log_term_locked()) ||
                    (args.last_log_term == last_log_term_locked() && args.last_log_index >= log_.size());
                if ((!has_voted_this_term_ || voted_for_ == candidate_id) && log_ok) {
                    vote_granted = true;
                    has_voted_this_term_ = true;
                    voted_for_ = candidate_id;
                    reset_election_deadline_locked();
                }
            }
            return wire::RequestVoteResult{ current_term_, vote_granted ? uint8_t(1) : uint8_t(0) };
        }

        wire::AppendEntriesResult handle_append_entries(NodeId leader_id, const wire::AppendEntriesHeader& hdr, const std::vector<LogEntry>& entries) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (hdr.term < current_term_) {
                return wire::AppendEntriesResult{ current_term_, 0, 0 };
            }
            // Only clear the "already voted this term" flag when the term
            // actually advances -- NOT on every heartbeat from the term's
            // legitimate leader. Otherwise a stale concurrent candidate
            // still canvassing for the SAME term could win a second vote
            // from a follower who already voted, after that follower's
            // vote flag was wrongly cleared by an unrelated heartbeat --
            // a real violation of Raft's one-vote-per-term Election Safety
            // property, not merely a cosmetic bug.
            if (hdr.term > current_term_) {
                current_term_ = hdr.term;
                has_voted_this_term_ = false;
            }
            role_.store(Role::Follower);
            current_leader_ = leader_id;
            has_leader_ = true;
            reset_election_deadline_locked();

            if (hdr.prev_log_index > 0) {
                if (log_.size() < hdr.prev_log_index) {
                    return wire::AppendEntriesResult{ current_term_, 0, 0 };
                }
                if (log_[static_cast<size_t>(hdr.prev_log_index) - 1].term != hdr.prev_log_term) {
                    log_.resize(hdr.prev_log_index - 1);
                    return wire::AppendEntriesResult{ current_term_, 0, 0 };
                }
            }

            size_t idx = static_cast<size_t>(hdr.prev_log_index);
            for (const LogEntry& e : entries) {
                if (idx < log_.size()) {
                    if (log_[idx].term != e.term) {
                        log_.resize(idx);
                        log_.push_back(e);
                    }
                }
                else {
                    log_.push_back(e);
                }
                ++idx;
            }

            if (hdr.leader_commit > commit_index_) {
                commit_index_ = std::min<uint64_t>(hdr.leader_commit, log_.size());
                apply_committed_entries_locked();
            }
            return wire::AppendEntriesResult{ current_term_, 1, log_.size() };
        }

        void apply_committed_entries_locked() {
            while (last_applied_ < commit_index_) {
                const LogEntry& entry = log_[static_cast<size_t>(last_applied_)];
                if (!entry.is_noop) {
                    const AddRuleCommand& cmd = entry.command;
                    engine_.add_rule(cmd.rule_id, cmd.event_id, cmd.threshold, cmd.comparator, cmd.severity);
                }
                ++last_applied_;
            }
        }

        void reset_election_deadline_locked() {
            std::uniform_int_distribution<int> dist(kElectionTimeoutMinMs, kElectionTimeoutMaxMs);
            election_deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(dist(rng_));
        }
        void reset_election_deadline() {
            std::lock_guard<std::mutex> lock(state_mutex_);
            reset_election_deadline_locked();
        }

        void tick_loop() {
            while (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
                if (!running_.load()) break;
                if (role_.load() == Role::Leader) {
                    replicate_to_all_peers();
                }
                else {
                    bool expired;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        expired = std::chrono::steady_clock::now() >= election_deadline_;
                    }
                    if (expired) start_election();
                }
            }
        }

        void start_election() {
            uint64_t term_at_election;
            uint64_t last_idx, last_term;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                ++current_term_;
                role_.store(Role::Candidate);
                voted_for_ = id_;
                has_voted_this_term_ = true;
                has_leader_ = false;
                reset_election_deadline_locked();
                term_at_election = current_term_;
                last_idx = log_.size();
                last_term = last_log_term_locked();
            }

            wire::RequestVoteArgs args{ term_at_election, id_, last_idx, last_term };
            std::vector<uint8_t> req = wire::pack(wire::MsgType::RequestVoteReq, args);

            std::atomic<int> votes{ 1 }; // vote for self
            std::vector<std::thread> workers;
            for (const auto& p : peers_) {
                // Capture peer_id BY VALUE, not a reference to the loop
                // variable p -- p is rebound/destroyed on each iteration
                // while these threads are still running (they're only
                // joined after this loop finishes below), so a captured
                // reference to p would dangle.
                NodeId peer_id = p.id;
                workers.emplace_back([this, peer_id, &req, &votes, term_at_election] {
                    std::vector<uint8_t> resp;
                    if (!call_rpc(peer_id, req, resp)) return;
                    if (resp.size() != 1 + sizeof(wire::RequestVoteResult)) return;
                    wire::RequestVoteResult result;
                    std::memcpy(&result, resp.data() + 1, sizeof(result));
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (result.term > current_term_) {
                        current_term_ = result.term;
                        role_.store(Role::Follower);
                        has_voted_this_term_ = false;
                        reset_election_deadline_locked(); // re-randomize rather than keep whatever was set when OUR candidacy began, for the same dueling-candidate mitigation as the wider timeout spread above
                        return;
                    }
                    if (result.vote_granted && result.term == term_at_election) votes.fetch_add(1);
                    });
            }
            for (auto& t : workers) t.join();

            bool became_leader;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                size_t majority = (peers_.size() + 1) / 2 + 1;
                became_leader = role_.load() == Role::Candidate && current_term_ == term_at_election &&
                    static_cast<size_t>(votes.load()) >= majority;
                if (became_leader) {
                    role_.store(Role::Leader);
                    current_leader_ = id_;
                    has_leader_ = true;
                    for (const auto& p : peers_) {
                        next_index_[p.id] = log_.size() + 1;
                        match_index_[p.id] = 0;
                    }
                    // See LogEntry::is_noop's comment: without a current-
                    // term entry to anchor commit advancement to, an
                    // already-majority-replicated-but-not-yet-committed
                    // entry inherited from the PREVIOUS leader's term can
                    // get stuck uncommitted forever if nothing new is ever
                    // proposed afterward. Appending and immediately
                    // replicating this guarantees that never happens.
                    log_.push_back(LogEntry{ current_term_, /*is_noop=*/1, AddRuleCommand{} });
                }
            }
            // replicate_to_all_peers() acquires state_mutex_ itself, so
            // this must run after the lock above is released.
            if (became_leader) replicate_to_all_peers();
        }

        void replicate_to_all_peers() {
            uint64_t term_snapshot, commit_snapshot;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (role_.load() != Role::Leader) return;
                term_snapshot = current_term_;
                commit_snapshot = commit_index_;
            }

            std::vector<std::thread> workers;
            for (const auto& p : peers_) {
                // Same by-value capture rationale as start_election() above.
                NodeId peer_id = p.id;
                workers.emplace_back([this, peer_id, term_snapshot, commit_snapshot] {
                    replicate_to_peer(peer_id, term_snapshot, commit_snapshot);
                    });
            }
            for (auto& t : workers) t.join();

            // Recompute commit_index_: highest index replicated to a
            // majority (this node + peers with match_index_ >= N) whose
            // entry's term equals the leader's CURRENT term -- Raft never
            // commits a prior-term entry purely by count, only by being
            // carried forward alongside a current-term entry, to avoid the
            // classic "leader crashes right after replicating, a new
            // leader overwrites it" safety hole.
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (role_.load() != Role::Leader || current_term_ != term_snapshot) return;
            for (uint64_t n = log_.size(); n > commit_index_; --n) {
                if (log_[static_cast<size_t>(n) - 1].term != current_term_) continue;
                size_t count = 1; // self
                for (const auto& p : peers_) if (match_index_[p.id] >= n) ++count;
                if (count >= (peers_.size() + 1) / 2 + 1) {
                    commit_index_ = n;
                    apply_committed_entries_locked();
                    break;
                }
            }
        }

        void replicate_to_peer(NodeId peer_id, uint64_t term_snapshot, uint64_t commit_snapshot) {
            uint64_t next_idx;
            std::vector<LogEntry> entries;
            uint64_t prev_log_index, prev_log_term;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (role_.load() != Role::Leader || current_term_ != term_snapshot) return;
                next_idx = next_index_[peer_id];
                prev_log_index = next_idx > 0 ? next_idx - 1 : 0;
                prev_log_term = (prev_log_index > 0 && prev_log_index <= log_.size())
                    ? log_[static_cast<size_t>(prev_log_index) - 1].term : 0;
                for (uint64_t i = next_idx; i <= log_.size() && entries.size() < kMaxEntriesPerRpc; ++i) {
                    entries.push_back(log_[static_cast<size_t>(i) - 1]);
                }
            }

            wire::AppendEntriesHeader hdr{ term_snapshot, id_, prev_log_index, prev_log_term,
                commit_snapshot, static_cast<uint32_t>(entries.size()) };
            std::vector<uint8_t> req = wire::pack_append_entries(hdr, entries);

            std::vector<uint8_t> resp;
            if (!call_rpc(peer_id, req, resp)) return;
            if (resp.size() != 1 + sizeof(wire::AppendEntriesResult)) return;
            wire::AppendEntriesResult result;
            std::memcpy(&result, resp.data() + 1, sizeof(result));

            std::lock_guard<std::mutex> lock(state_mutex_);
            if (result.term > current_term_) {
                current_term_ = result.term;
                role_.store(Role::Follower);
                has_voted_this_term_ = false;
                return;
            }
            if (role_.load() != Role::Leader || current_term_ != term_snapshot) return;
            if (result.success) {
                next_index_[peer_id] = prev_log_index + entries.size() + 1;
                match_index_[peer_id] = prev_log_index + entries.size();
            }
            else {
                uint64_t& ni = next_index_[peer_id];
                if (ni > 1) --ni;
            }
        }

        static constexpr int kTickMs = 30;
        // A wider spread lowers the odds that two simultaneously-viable
        // candidates (e.g. the two survivors in a 3-node cluster after the
        // third dies) keep re-colliding round after round -- collision
        // probability per round is roughly proportional to (RPC round-trip
        // time) / (spread), so widening the spread instead of just hoping
        // for a lucky draw is the standard Raft mitigation.
        static constexpr int kElectionTimeoutMinMs = 300;
        static constexpr int kElectionTimeoutMaxMs = 900;
        static constexpr size_t kMaxEntriesPerRpc = 64;

        NodeId id_;
        std::vector<PeerConfig> peers_;
        transport::CertContextPtr my_cert_;
        transport::TrustedRoot& trust_;
        animus::Engine& engine_;
        uint16_t listen_port_;
        std::unordered_map<std::wstring, NodeId> cn_to_node_id_;

        std::atomic<bool> running_{ false };
        std::atomic<Role> role_{ Role::Follower };
        std::mt19937 rng_;

        mutable std::mutex state_mutex_;
        uint64_t current_term_ = 0;
        NodeId voted_for_ = 0;
        bool has_voted_this_term_ = false;
        std::vector<LogEntry> log_;
        uint64_t commit_index_ = 0;
        uint64_t last_applied_ = 0;
        NodeId current_leader_ = 0;
        bool has_leader_ = false;
        std::chrono::steady_clock::time_point election_deadline_;
        std::unordered_map<NodeId, uint64_t> next_index_;
        std::unordered_map<NodeId, uint64_t> match_index_;

        std::mutex listener_mutex_;
        transport::TcpSocket listener_socket_;
        std::thread listener_thread_;
        std::thread tick_thread_;
        std::mutex handlers_mutex_;
        std::vector<std::thread> handler_threads_;
        std::vector<std::shared_ptr<transport::SecureChannel>> inbound_channels_;
        std::mutex links_mutex_;
        std::unordered_map<NodeId, std::unique_ptr<PeerLink>> links_;
    };

} // namespace cluster
} // namespace animus
#endif // defined(_WIN32) && defined(_MSC_VER) -- end of animus_cluster.hpp
