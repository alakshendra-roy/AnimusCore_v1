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
