#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <fstream>
#include <mutex>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#pragma intrinsic(__rdtsc)
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

                bool matched = false;
                switch (rule.comparator) {
                case RuleComparator::GreaterThan: matched = payload.metric_value > rule.threshold; break;
                case RuleComparator::LessThan:    matched = payload.metric_value < rule.threshold; break;
                case RuleComparator::Equal:       matched = payload.metric_value == rule.threshold; break;
                }

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
                    for (const TelemetryPayload& payload : batch) {
                        evaluate_rules(payload, *rules_snapshot);
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
            rules_(std::make_shared<const std::vector<RuleThreshold>>()) {
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
}