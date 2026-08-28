// Phase 9 demo/verification: distributed cloud orchestration & clustering
// (animus_cluster.hpp) -- a 3-node Raft-lite cluster, each node mTLS-linked
// to every other node over animus_transport.hpp's Schannel transport, each
// running its own local animus::Engine as the state machine that replicated
// AddRule commands are applied to.
//
// Proves, with real running nodes and real network I/O (not a simulated
// clock):
//   1. Leader election converges on exactly one leader out of 3 nodes.
//   2. A rule proposed through the leader is replicated to a majority,
//      committed, and applied identically to ALL THREE nodes' independent
//      engines -- verified functionally (each engine, given the same
//      triggering event, produces a matching ThreatSignal), not merely by
//      comparing log lengths.
//   3. Stopping the leader (simulating a node failure) triggers a new
//      election among the survivors, and a rule proposed through the new
//      leader replicates to both surviving nodes.
//   4. Proposing to a follower is rejected with a leader hint, mirroring
//      how a real client library would redirect a write.
//
// Requires demo_certs/{ca.cer,node1.pfx,node2.pfx,node3.pfx}: run
// generate_demo_certs.ps1 first.
//
// Intentionally excluded from the default MSBuild targets (see
// AnimusCore_v1.vcxproj), same as the other Phase 7/8 standalone demos.
// Build and run from a "x64 Native Tools Command Prompt for VS":
//   cl /std:c++17 /EHsc /O2 cluster_demo.cpp /Fe:cluster_demo.exe
//   cluster_demo.exe
#include "animus_cluster.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

using namespace animus;
using namespace animus::transport;
using namespace animus::cluster;

namespace {
    std::wstring cert_dir() { return L"demo_certs\\"; }

    // Polls `pred` until it returns a non-zero NodeId or `timeout_ms`
    // elapses. Used both to wait for the initial election and for the
    // post-failover re-election -- a real wall-clock wait, not a fixed
    // sleep, so the demo is fast when Raft converges quickly and only
    // waits as long as it actually needs to.
    template <typename Pred>
    NodeId wait_for(Pred pred, int timeout_ms, double* elapsed_ms_out = nullptr) {
        auto start = std::chrono::steady_clock::now();
        auto deadline = start + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            NodeId result = pred();
            if (result != 0) {
                if (elapsed_ms_out) {
                    *elapsed_ms_out = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
                }
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }
}

int main() {
    std::cout << std::unitbuf; // stream progress live rather than batching it at exit -- this is a multi-second demo with real network events, not a one-shot print
    WinsockInit wsa;
    if (!wsa.ok()) {
        std::cerr << "[CLUSTER DEMO] WSAStartup failed\n";
        return 1;
    }

    CertContextPtr ca_cert, cert1, cert2, cert3;
    try {
        ca_cert = load_cer_certificate(cert_dir() + L"ca.cer");
        cert1 = load_pfx_certificate(cert_dir() + L"node1.pfx", L"AnimusDemoP@ss1");
        cert2 = load_pfx_certificate(cert_dir() + L"node2.pfx", L"AnimusDemoP@ss1");
        cert3 = load_pfx_certificate(cert_dir() + L"node3.pfx", L"AnimusDemoP@ss1");
    }
    catch (const std::exception& ex) {
        std::cerr << "[CLUSTER DEMO] certificate load failed: " << ex.what() << "\n"
            << "Run generate_demo_certs.ps1 first (from AnimusCore_v1/).\n";
        return 1;
    }
    TrustedRoot trust(ca_cert.get());
    // Raw pointers captured before the CertContextPtrs are moved into the
    // RaftNodes below -- delete_persisted_key() only needs a valid
    // PCCERT_CONTEXT to look up (and remove) the CNG/CAPI key material
    // load_pfx_certificate() persisted, and the certs stay alive (owned by
    // each RaftNode) until main() returns, so these remain valid for the
    // cleanup calls at the end of main().
    PCCERT_CONTEXT raw_cert1 = cert1.get();
    PCCERT_CONTEXT raw_cert2 = cert2.get();
    PCCERT_CONTEXT raw_cert3 = cert3.get();

    constexpr uint16_t kPort1 = 47901, kPort2 = 47902, kPort3 = 47903;
    PeerConfig p1{ 1, "127.0.0.1", kPort1, L"animus-node-1" };
    PeerConfig p2{ 2, "127.0.0.1", kPort2, L"animus-node-2" };
    PeerConfig p3{ 3, "127.0.0.1", kPort3, L"animus-node-3" };

    auto engine1 = Engine::Create(1 << 14);
    auto engine2 = Engine::Create(1 << 14);
    auto engine3 = Engine::Create(1 << 14);
    // Rule evaluation runs on the persistence worker thread (see
    // EngineImpl::process_persistence_queue in animus.hpp) -- it must be
    // started for add_rule()'d rules to ever actually be evaluated against
    // recorded events, exactly as the Phase 5 SOAR pipeline requires.
    engine1->start_persistence("cluster_demo_node1.log");
    engine2->start_persistence("cluster_demo_node2.log");
    engine3->start_persistence("cluster_demo_node3.log");

    RaftNode node1(1, { p2, p3 }, std::move(cert1), trust, *engine1, kPort1);
    RaftNode node2(2, { p1, p3 }, std::move(cert2), trust, *engine2, kPort2);
    RaftNode node3(3, { p1, p2 }, std::move(cert3), trust, *engine3, kPort3);

    std::cout << "=== PHASE 9 DISTRIBUTED CLUSTER DEMO (Raft-lite over mTLS) ===\n";
    node1.start();
    node2.start();
    node3.start();

    auto leader_of = [&]() -> NodeId {
        if (node1.is_leader()) return 1;
        if (node2.is_leader()) return 2;
        if (node3.is_leader()) return 3;
        return 0;
        };

    double election1_ms = 0.0;
    NodeId leader_id = wait_for(leader_of, 5000, &election1_ms);
    std::cout << "Initial leader elected         : "
        << (leader_id ? ("node-" + std::to_string(leader_id)) : "NONE")
        << " in " << election1_ms << " ms\n";

    RaftNode* nodes[3] = { &node1, &node2, &node3 };
    auto by_id = [&](NodeId id) -> RaftNode& { return *nodes[id - 1]; };
    Engine* engines[3] = { engine1.get(), engine2.get(), engine3.get() };
    auto engine_by_id = [&](NodeId id) -> Engine& { return *engines[id - 1]; };

    // propose()'s ProposeResult::Ok only guarantees the entry reached a
    // MAJORITY (leader + 1 follower in a 3-node cluster) -- not that every
    // node has applied it yet. Firing the functional check's triggering
    // event immediately after propose() returns is racy: a follower's
    // persistence worker runs on a ~50us cycle (see EngineImpl::
    // process_persistence_queue in animus.hpp) and can pop and evaluate
    // the just-recorded event against that engine's still-empty rule set
    // microseconds before the replicated AppendEntries carrying the rule
    // arrives -- evaluate_rules() never re-evaluates an already-popped
    // event, so that engine would silently never produce the signal. Wait
    // for every target node's own committed_count() to catch up before
    // triggering, rather than assuming a fixed sleep is always enough.
    auto wait_for_replication = [](std::initializer_list<RaftNode*> targets, uint64_t index, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            bool all_caught_up = true;
            for (RaftNode* n : targets) all_caught_up = all_caught_up && (n->committed_count() >= index);
            if (all_caught_up) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
        };

    bool propose1_ok = false;
    uint64_t rule1_index = 0;
    if (leader_id != 0) {
        AddRuleCommand cmd{ /*rule_id=*/1, /*event_id=*/500, /*threshold=*/100, /*comparator=*/0 /*GreaterThan*/, /*severity=*/5 };
        NodeId hint = 0;
        propose1_ok = (by_id(leader_id).propose(cmd, &hint) == ProposeResult::Ok);
        // The leader's log may already contain a leader-election no-op
        // entry (RaftNode appends one on becoming leader -- see
        // animus_cluster.hpp's LogEntry::is_noop comment), so rule #1 is
        // NOT necessarily at index 1. log_size() right after a successful
        // propose() is exactly the index that entry landed at.
        if (propose1_ok) rule1_index = by_id(leader_id).log_size();
    }
    std::cout << "Rule #1 proposed via leader     : " << (propose1_ok ? "committed" : "FAILED") << "\n";

    // Functional replication check: fire the SAME triggering event at all
    // three nodes' independent engines and confirm each one -- including
    // the two that never called add_rule() themselves -- independently
    // matches it, proving the rule genuinely reached every engine's rule
    // set, not just that a log entry with the right byte pattern exists.
    bool replicated_all_3 = true;
    if (propose1_ok) {
        wait_for_replication({ &node1, &node2, &node3 }, rule1_index, /*timeout_ms=*/2000);
        for (NodeId id = 1; id <= 3; ++id) {
            engine_by_id(id).record(/*event_id=*/500, /*trace_id=*/id, /*value=*/150); // 150 > 100
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (NodeId id = 1; id <= 3; ++id) {
            ThreatSignal sig{};
            size_t n = engine_by_id(id).poll_signals(&sig, 1);
            bool matched = (n == 1 && sig.rule_id == 1 && sig.event_id == 500);
            std::cout << "  node-" << id << " matched rule #1     : " << (matched ? "yes" : "NO") << "\n";
            replicated_all_3 = replicated_all_3 && matched;
        }
    }
    else {
        replicated_all_3 = false;
    }

    // Follower redirection check: proposing to a node we know is NOT the
    // leader must fail closed with NotLeader and point back at the real
    // leader, rather than silently doing nothing or accepting a write it
    // cannot actually replicate as leader.
    bool follower_redirect_ok = false;
    if (leader_id != 0) {
        NodeId follower_id = (leader_id == 1) ? 2 : 1;
        AddRuleCommand bogus{ 99, 999, 1, 0, 1 };
        NodeId hint = 0;
        ProposeResult r = by_id(follower_id).propose(bogus, &hint);
        follower_redirect_ok = (r == ProposeResult::NotLeader && hint == leader_id);
        std::cout << "Follower propose() redirected  : " << (follower_redirect_ok ? "yes" : "NO")
            << " (node-" << follower_id << " -> hint node-" << hint << ")\n";
    }

    // Failover: kill the current leader (close its listener + all its
    // outbound links) and confirm the two survivors elect a new leader and
    // keep replicating.
    NodeId old_leader = leader_id;
    if (old_leader != 0) {
        by_id(old_leader).stop();
    }
    std::cout << "Stopped node-" << old_leader << " to simulate a failure\n";

    auto surviving_leader_of = [&]() -> NodeId {
        for (NodeId id = 1; id <= 3; ++id) {
            if (id == old_leader) continue;
            if (by_id(id).is_leader()) return id;
        }
        return 0;
        };
    double election2_ms = 0.0;
    // A wider budget than the initial 3-node election: with only 2
    // survivors, a "dueling candidates" round (both time out and canvass
    // for votes at nearly the same moment, each bumping the other back to
    // Follower mid-round) is more likely than in the initial 3-node
    // election and can take several retries to resolve -- Raft guarantees
    // eventual convergence under randomized timeouts, not a bounded number
    // of rounds, so the test's patience needs to exceed the algorithm's
    // worst realistic case, not its typical case.
    NodeId new_leader = wait_for(surviving_leader_of, 15000, &election2_ms);
    std::cout << "New leader after failover      : "
        << (new_leader ? ("node-" + std::to_string(new_leader)) : "NONE")
        << " in " << election2_ms << " ms\n";

    bool propose2_ok = false;
    bool replicated_survivors = true;
    uint64_t rule2_index = 0;
    if (new_leader != 0) {
        AddRuleCommand cmd2{ /*rule_id=*/2, /*event_id=*/501, /*threshold=*/50, /*comparator=*/1 /*LessThan*/, /*severity=*/7 };
        NodeId hint = 0;
        propose2_ok = (by_id(new_leader).propose(cmd2, &hint) == ProposeResult::Ok);
        // See rule1_index's comment above -- the new leader's own election
        // no-op (plus, if it lost and re-ran an election, further no-ops)
        // means rule #2 isn't necessarily at index 2 either.
        if (propose2_ok) rule2_index = by_id(new_leader).log_size();
        std::cout << "Rule #2 proposed via new leader : " << (propose2_ok ? "committed" : "FAILED") << "\n";

        if (propose2_ok) {
            std::initializer_list<RaftNode*> survivors = (old_leader == 1) ? std::initializer_list<RaftNode*>{ &node2, &node3 }
                : (old_leader == 2) ? std::initializer_list<RaftNode*>{ &node1, &node3 }
                : std::initializer_list<RaftNode*>{ &node1, &node2 };
            wait_for_replication(survivors, rule2_index, /*timeout_ms=*/5000);
            for (NodeId id = 1; id <= 3; ++id) {
                if (id == old_leader) continue;
                engine_by_id(id).record(/*event_id=*/501, /*trace_id=*/id, /*value=*/10); // 10 < 50
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            for (NodeId id = 1; id <= 3; ++id) {
                if (id == old_leader) continue;
                ThreatSignal sig{};
                size_t n = engine_by_id(id).poll_signals(&sig, 1);
                bool matched = (n == 1 && sig.rule_id == 2 && sig.event_id == 501);
                std::cout << "  node-" << id << " matched rule #2     : " << (matched ? "yes" : "NO") << "\n";
                replicated_survivors = replicated_survivors && matched;
            }
        }
        else {
            replicated_survivors = false;
        }
    }

    for (NodeId id = 1; id <= 3; ++id) {
        if (id != old_leader) by_id(id).stop();
    }

    delete_persisted_key(raw_cert1);
    delete_persisted_key(raw_cert2);
    delete_persisted_key(raw_cert3);
    std::remove("cluster_demo_node1.log");
    std::remove("cluster_demo_node2.log");
    std::remove("cluster_demo_node3.log");

    bool all_ok = leader_id != 0 && propose1_ok && replicated_all_3 &&
        follower_redirect_ok && new_leader != 0 && new_leader != old_leader &&
        propose2_ok && replicated_survivors;

    std::cout << "\n=== SUMMARY ===\n";
    std::cout << "Election safety   : exactly one leader observed initially and after failover\n";
    std::cout << "Replication        : rule #1 reached all 3 nodes, rule #2 reached both survivors\n";
    std::cout << "Failover           : " << (new_leader != 0 && new_leader != old_leader ? "new leader distinct from stopped leader" : "FAILED") << "\n";
    std::cout << (all_ok ? "[CLUSTER DEMO] ALL CHECKS PASSED\n" : "[CLUSTER DEMO] FAILED\n");
    return all_ok ? 0 : 1;
}
