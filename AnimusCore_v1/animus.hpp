#pragma once
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
}