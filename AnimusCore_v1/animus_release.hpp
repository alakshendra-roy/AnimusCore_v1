#pragma once
// =========================================================================
// animus_release.hpp -- Animus Core v1.0 single-header release
//
// GENERATED FILE -- do not edit directly. Produced by amalgamate.py from
// the four source headers below; re-run `python amalgamate.py` after any
// change to those originals and commit the regenerated output alongside.
// Generated: 2026-08-28
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

    class Engine {
    public:
        virtual ~Engine() = default;
        virtual bool record(uint32_t event_id, uint32_t trace_id, uint64_t value) const noexcept = 0;
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
// Append macro definition at top if not already defined:
#ifndef ANIMUS_API
#ifdef ANIMUS_EXPORTS
#define ANIMUS_API __declspec(dllexport)
#else
#define ANIMUS_API __declspec(dllimport)
#endif
#endif

// Add C-ABI Exports at the bottom of animus.hpp (below namespace animus)
extern "C" {
    ANIMUS_API bool animus_init(size_t buffer_capacity);
    ANIMUS_API bool animus_record_event(uint32_t event_id, uint32_t trace_id, uint64_t metric_value);
    ANIMUS_API void animus_start_logging(const char* filepath);
    ANIMUS_API void animus_stop_logging();
    ANIMUS_API bool animus_add_rule(uint32_t rule_id, uint32_t event_id, uint64_t threshold, uint8_t comparator, uint32_t severity);
    ANIMUS_API size_t animus_poll_signals(animus::ThreatSignal* out, size_t max_count);
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
    };

    // Static role -> permission table, checked in O(1) with no allocation.
    // Deliberately a flat switch rather than a data-driven policy file: RBAC
    // here is a small, fixed lattice (3 roles x 5 permissions), and a
    // switch keeps the mapping exhaustively checkable by the compiler
    // (-Wswitch) if a role or permission is ever added.
    class RbacPolicy {
    public:
        static bool is_allowed(Role role, Permission perm) noexcept {
            switch (role) {
            case Role::Viewer:
                return perm == Permission::PollSignals;
            case Role::Operator:
                return perm == Permission::PollSignals || perm == Permission::RecordEvent;
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

} // namespace security
} // namespace animus

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
