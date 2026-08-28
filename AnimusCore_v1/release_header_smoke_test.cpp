// Phase 10 verification: proves animus_release.hpp (the generated
// single-header amalgamation -- see amalgamate.py) is actually usable as a
// drop-in replacement for #include-ing all four source headers separately.
// One #include below must give working access to all four layers with no
// ODR/macro collisions between the concatenated sections -- exactly what a
// client vendoring only animus_release.hpp into their own project would
// get, and exactly what an un-stripped local #include (see amalgamate.py's
// _LOCAL_INCLUDE_RE) would have silently broken.
//
// Regenerate animus_release.hpp after any change to the four source
// headers (`python amalgamate.py`), then re-run this to confirm nothing
// broke:
//   cl /std:c++17 /EHsc /O2 release_header_smoke_test.cpp /Fe:release_header_smoke_test.exe
//   release_header_smoke_test.exe
//
// Intentionally excluded from the default MSBuild targets (see
// AnimusCore_v1.vcxproj), same as the other Phase 7/8/9 standalone demos.
#include "animus_release.hpp"

#include <cassert>
#include <cstdio>

int main() {
    // Layer 1: core engine (animus.hpp content).
    auto engine = animus::Engine::Create(1 << 10);
    bool added = engine->add_rule(1, 500, 100, static_cast<uint8_t>(animus::RuleComparator::GreaterThan), 5);
    assert(added);
    engine->record(500, 1, 150);

    // Layer 2: RBAC + multi-tenancy (animus_security.hpp content).
    animus::security::TenantRegistry registry;
    registry.create_tenant(10, 1 << 10);
    animus::security::AccessToken admin{10, /*principal_id=*/1, animus::security::Role::Admin};
    animus::security::SecureTelemetryGateway gateway(registry);
    bool recorded = gateway.record(admin, 500, 1, 150);
    assert(recorded);

    // Layers 3-4 (mTLS transport, Raft-lite cluster) only exist on Windows
    // -- verified by real compile+link here since this repo's only
    // supported native target is Windows/MSVC (see animus_transport.hpp's
    // own #error guard for non-Windows).
#if defined(_WIN32)
    animus::transport::WinsockInit wsa;
    (void)wsa.ok();
    // Reference (not run -- no certs/sockets in this smoke test) the
    // cluster type to prove animus_cluster.hpp's section linked cleanly.
    static_assert(sizeof(animus::cluster::RaftNode) > 0, "RaftNode must be a complete type here");
#endif

    std::printf("[RELEASE HEADER SMOKE TEST] animus_release.hpp: all four layers linked and exercised OK\n");
    return 0;
}
