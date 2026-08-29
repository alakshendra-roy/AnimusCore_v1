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
