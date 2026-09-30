#pragma once
// Phase 8: RBAC + multi-tenant telemetry stream isolation.
//
// Platform-independent (unlike animus_transport.hpp, which is Windows-only)
// and header-only for the same reason as animus.hpp itself: any C++17
// translation unit can #include this alongside animus.hpp with no separate
// .cpp to build. This layer does not talk to the network -- it is the
// authorization/isolation boundary that animus_transport.hpp's Schannel
// mTLS server calls into once a client certificate has already been
// cryptographically verified.
#include "animus.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

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

    // Compile-time capacity of each gateway's audit trail, in entries (32
    // bytes each: 4096 -> 128 KiB per trail). Must be a power of two so slot
    // selection is a mask. Override with -DANIMUS_AUDIT_LOG_CAPACITY=<2^k>.
#ifndef ANIMUS_AUDIT_LOG_CAPACITY
#define ANIMUS_AUDIT_LOG_CAPACITY 4096
#endif
    inline constexpr size_t AUDIT_LOG_CAPACITY = ANIMUS_AUDIT_LOG_CAPACITY;
    static_assert(AUDIT_LOG_CAPACITY >= 2 && (AUDIT_LOG_CAPACITY & (AUDIT_LOG_CAPACITY - 1)) == 0,
        "AUDIT_LOG_CAPACITY must be a power of two >= 2");

    // Bounded, allocation-free audit trail. Replaces an unbounded
    // std::deque<AuditEvent>, which grew without limit whenever nothing
    // polled it (every gateway call appends one entry, allowed or denied) and
    // heap-allocated on the hot path -- an OOM under sustained throughput.
    //
    // All storage is inline and pre-faulted at construction, so append() and
    // drain() never allocate and never page-fault. When the trail is full the
    // OLDEST unread entry is overwritten and dropped_count() is incremented:
    // memory is constant and loss is explicit, never silent. (Trade-off worth
    // knowing: a flood of calls can evict older entries before an auditor
    // polls. The counter makes that visible; poll often enough, or raise the
    // capacity, if the trail is evidence you cannot afford to lose.)
    //
    // Thread-safe (any number of appenders and drainers). The critical
    // section is an O(1) 32-byte store; the timestamp is taken inside it so
    // the trail's order is also timestamp order. Cache layout: the lock and
    // cursors, the drop counter (read by monitors without the lock), and the
    // entry array each start on their own line, so a monitor polling
    // dropped_count() never contends with appenders for the lock's line.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // intentional cache-line padding (see above)
#endif
    class BoundedAuditLog {
    public:
        static constexpr size_t kCapacity = AUDIT_LOG_CAPACITY;
        static constexpr size_t kMask = kCapacity - 1;

        BoundedAuditLog() noexcept : entries_{} {}
        BoundedAuditLog(const BoundedAuditLog&) = delete;
        BoundedAuditLog& operator=(const BoundedAuditLog&) = delete;

        void append(const AccessToken& token, Permission perm, AuditOutcome outcome) noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_[write_ & kMask] = AuditEvent{
                read_cycle_counter(), token.tenant_id, token.principal_id, perm, outcome };
            ++write_;
            if (write_ - read_ > kCapacity) {
                ++read_; // the slot just written held the oldest unread entry
                // Every writer holds mutex_, so a plain load+store is enough
                // (no locked RMW); the atomic exists for lock-free readers.
                dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            }
        }

        // Moves up to max_count of the oldest unread entries into `out`, in
        // order. Returns how many were moved.
        size_t drain(AuditEvent* out, size_t max_count) noexcept {
            if (!out) return 0;
            std::lock_guard<std::mutex> lock(mutex_);
            const uint64_t unread = write_ - read_;
            const size_t count = unread < max_count ? static_cast<size_t>(unread) : max_count;
            for (size_t i = 0; i < count; ++i) out[i] = entries_[(read_ + i) & kMask];
            read_ += count;
            return count;
        }

        size_t size() const noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            return static_cast<size_t>(write_ - read_);
        }

        // Entries overwritten before anyone read them. Lock-free.
        uint64_t dropped_count() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    private:
        alignas(kCachelineBytes) mutable std::mutex mutex_;
        uint64_t write_ = 0; // entries ever appended
        uint64_t read_ = 0;  // index of the oldest unread entry
        alignas(kCachelineBytes) std::atomic<uint64_t> dropped_{ 0 };
        alignas(kCachelineBytes) AuditEvent entries_[kCapacity];
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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

        size_t poll_audit_log(AuditEvent* out, size_t max_count) noexcept {
            return audit_log_.drain(out, max_count);
        }

        // Audit entries lost to the bounded trail overwriting its oldest
        // unread entry (see BoundedAuditLog). 0 means nothing was ever lost.
        uint64_t audit_dropped_count() const noexcept { return audit_log_.dropped_count(); }

    private:
        template <typename Fn>
        bool authorize_and_dispatch(const AccessToken& token, Permission perm, Fn&& fn) {
            bool allowed = RbacPolicy::is_allowed(token.role, perm);
            Engine* engine = allowed ? registry_.get_tenant(token.tenant_id) : nullptr;
            bool ok = engine && fn(*engine);
            append_audit(token, perm, (allowed && engine) ? AuditOutcome::Allowed : AuditOutcome::Denied);
            return ok;
        }

        void append_audit(const AccessToken& token, Permission perm, AuditOutcome outcome) noexcept {
            audit_log_.append(token, perm, outcome);
        }

        TenantRegistry& registry_;
        BoundedAuditLog audit_log_;
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

        // Opt-in, OFF by default: when enabled, submit() below additionally
        // requires a verified offline license (animus_is_licensed(), see
        // animus_verify_license/animus_check_license_status in
        // animus_engine.cpp) before routing an otherwise-authorized order.
        // Defaulting to OFF is deliberate, not an oversight -- this feature
        // shipped (v1.1.0-rc1) and was tested with no license concept
        // attached to it at all; flipping the default here would silently
        // break every existing caller (and every existing test) that never
        // verifies a license today. A deployment that wants execution
        // itself gated on a valid license calls this once at startup;
        // everyone else sees identical behavior to before this existed.
        void set_execution_license_required(bool required) noexcept {
            require_license_.store(required, std::memory_order_release);
        }

        // Routes one order through the token's own tenant ExecutionClient.
        // Requires SubmitOrder, and (only if set_execution_license_required
        // has been called with true) a verified license. Returns false for
        // a denied token, an unlicensed process when required, and a
        // broker-rejected order -- same flat-bool convention as
        // SecureTelemetryGateway::record() above; poll_execution_audit_log()
        // is how a caller distinguishes "not authorized" from "the tenant's
        // execution path isn't set up yet" from "the broker rejected it",
        // not the return value of this call.
        bool submit(const AccessToken& token, const OrderRequest& request, ExecutionReport& out) {
            bool allowed = RbacPolicy::is_allowed(token.role, Permission::SubmitOrder);
            if (allowed && require_license_.load(std::memory_order_acquire) && !animus_is_licensed()) {
                allowed = false;
            }
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

        size_t poll_execution_audit_log(AuditEvent* out, size_t max_count) noexcept {
            return audit_log_.drain(out, max_count);
        }

        // Same meaning as SecureTelemetryGateway::audit_dropped_count().
        uint64_t audit_dropped_count() const noexcept { return audit_log_.dropped_count(); }

    private:
        struct TenantExecution {
            std::unique_ptr<LoopbackBrokerGateway> gateway;
            std::unique_ptr<ExecutionClient> client;
        };

        void append_audit(const AccessToken& token, Permission perm, AuditOutcome outcome) noexcept {
            audit_log_.append(token, perm, outcome);
        }

        TenantRegistry& registry_;
        std::mutex mutex_;
        std::unordered_map<uint32_t, TenantExecution> tenants_;
        BoundedAuditLog audit_log_;
        std::atomic<bool> require_license_{ false };
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

    // Opt-in, OFF by default -- see SecureExecutionGateway::set_execution_license_required's
    // own docstring for why the default must stay OFF. Toggling this affects
    // every subsequent animus_security_submit_order call for this context.
    ANIMUS_API void animus_security_set_execution_license_required(void* ctx, bool required);
}
