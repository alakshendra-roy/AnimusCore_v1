// Phase 8 demo/verification: RBAC + multi-tenant telemetry isolation
// (animus_security.hpp), driven with zero DLL, zero Python, and zero
// network dependency -- this part of Phase 8 is platform-independent.
//
// Intentionally excluded from the default MSBuild targets (see
// AnimusCore_v1.vcxproj), same as execution_interop_demo.cpp. Build and
// run standalone, e.g.:
//   g++ -std=c++17 -O2 -pthread secure_multitenancy_demo.cpp -o secure_multitenancy_demo.exe
#include "animus_security.hpp"

#include <cassert>
#include <iostream>

using namespace animus;
using namespace animus::security;

int main() {
    TenantRegistry registry;
    SecureTelemetryGateway gateway(registry);

    AccessToken admin{ /*tenant_id=*/0, /*principal_id=*/1, Role::Admin };
    bool created_a = gateway.create_tenant(admin, /*new_tenant_id=*/10);
    bool created_b = gateway.create_tenant(admin, /*new_tenant_id=*/20);
    std::cout << "[SECURITY DEMO] create_tenant(10) admin-authorized : " << std::boolalpha << created_a << "\n";
    std::cout << "[SECURITY DEMO] create_tenant(20) admin-authorized : " << created_b << "\n";
    assert(created_a && created_b);

    AccessToken viewer_denied_admin{ /*tenant_id=*/0, /*principal_id=*/2, Role::Viewer };
    bool created_c = gateway.create_tenant(viewer_denied_admin, /*new_tenant_id=*/30);
    std::cout << "[SECURITY DEMO] create_tenant(30) viewer-denied     : " << created_c << " (expected false)\n";
    assert(!created_c);

    // Operator for tenant 10 records telemetry into tenant 10's isolated engine.
    AccessToken operator_a{ /*tenant_id=*/10, /*principal_id=*/100, Role::Operator };
    for (uint32_t i = 0; i < 5; ++i) {
        bool ok = gateway.record(operator_a, /*event_id=*/1, /*trace_id=*/i, /*value=*/1000 + i);
        assert(ok);
    }

    // A viewer scoped to tenant 20 must NOT be able to see tenant 10's
    // signals, even indirectly -- there is no rule registered against
    // tenant 20's isolated engine, so nothing should ever appear there.
    AccessToken admin_a{ /*tenant_id=*/10, /*principal_id=*/1, Role::Admin };
    bool rule_ok = gateway.add_rule(admin_a, /*rule_id=*/1, /*event_id=*/1,
        /*threshold=*/1002, static_cast<uint8_t>(RuleComparator::GreaterThan), /*severity=*/5);
    assert(rule_ok);

    // Feed two more events after the rule so the persistence worker's
    // NEXT batch has both the rule snapshot and events to evaluate it
    // against isn't relevant here -- add_rule takes effect for the *next*
    // start_persistence-driven batch, so start persistence, wait briefly,
    // then check signals.
    gateway.start_persistence(admin_a, "secure_demo_tenant10.log");
    for (uint32_t i = 5; i < 10; ++i) {
        gateway.record(operator_a, /*event_id=*/1, /*trace_id=*/i, /*value=*/1000 + i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    gateway.stop_persistence(admin_a);

    AccessToken viewer_a{ /*tenant_id=*/10, /*principal_id=*/3, Role::Viewer };
    ThreatSignal signals_a[16];
    size_t n_a = gateway.poll_signals(viewer_a, signals_a, 16);
    std::cout << "[SECURITY DEMO] tenant 10 viewer poll_signals count : " << n_a << " (expected > 0)\n";
    assert(n_a > 0);

    AccessToken viewer_b{ /*tenant_id=*/20, /*principal_id=*/4, Role::Viewer };
    ThreatSignal signals_b[16];
    size_t n_b = gateway.poll_signals(viewer_b, signals_b, 16);
    std::cout << "[SECURITY DEMO] tenant 20 viewer poll_signals count : " << n_b << " (expected 0, isolated from tenant 10)\n";
    assert(n_b == 0);

    // Viewer attempting RecordEvent (not entitled) must be denied, and the
    // event must never reach the tenant's engine.
    bool viewer_record_denied = gateway.record(viewer_a, /*event_id=*/1, /*trace_id=*/999, /*value=*/1);
    std::cout << "[SECURITY DEMO] viewer record() denied              : " << !viewer_record_denied << " (expected true)\n";
    assert(!viewer_record_denied);

    // A token pointed at a tenant_id that was never created must fail
    // closed (no engine to route to), not silently create one.
    AccessToken operator_ghost{ /*tenant_id=*/999, /*principal_id=*/5, Role::Operator };
    bool ghost_record = gateway.record(operator_ghost, 1, 1, 1);
    std::cout << "[SECURITY DEMO] unknown-tenant record() fails closed: " << !ghost_record << " (expected true)\n";
    assert(!ghost_record);

    AuditEvent audit[64];
    size_t n_audit = gateway.poll_audit_log(audit, 64);
    size_t denied_count = 0;
    for (size_t i = 0; i < n_audit; ++i) {
        if (audit[i].outcome == AuditOutcome::Denied) ++denied_count;
    }
    std::cout << "[SECURITY DEMO] audit events captured               : " << n_audit << "\n";
    std::cout << "[SECURITY DEMO] audit events denied                 : " << denied_count << " (expected >= 3)\n";
    assert(denied_count >= 3);

    std::cout << "[SECURITY DEMO] ALL CHECKS PASSED\n";
    return 0;
}
