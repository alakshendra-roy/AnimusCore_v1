// Phase 10 benchmark: real multi-node write-latency and throughput
// measurement for the Phase 9 Raft-lite cluster (animus_cluster.hpp), run
// specifically to produce the "final multi-node latency metrics" documented
// in BENCHMARKS.md -- not estimated, not reused from the Phase 9
// correctness-verification numbers (those measured election/failover
// timing, not steady-state write latency).
//
// Sets up the same real 3-node mTLS cluster as cluster_demo.cpp, elects a
// leader, then proposes many distinct AddRule commands back-to-back through
// it, timing each one. Two distinct latencies are captured per proposal,
// since they mean different things to a client of this system:
//   1. propose() call latency -- wall time from calling propose() to it
//      returning Ok. RaftNode::propose() blocks until the entry is
//      committed on a MAJORITY (leader + 1 follower in this 3-node
//      cluster), so this is the real "how long until my write is
//      durable/safe" latency a caller experiences.
//   2. Full-cluster convergence latency -- additional wall time, measured
//      from when propose() returns, until the SLOWEST follower (not just
//      the majority) has also applied the entry. This is 0 whenever the
//      lagging follower happened to be the one that already contributed to
//      the majority; when it's the other follower, this is the time for
//      the next heartbeat/replication round to reach it.
//
// Requires demo_certs/{ca.cer,node1.pfx,node2.pfx,node3.pfx}: run
// generate_demo_certs.ps1 first.
//
// Intentionally excluded from the default MSBuild targets (see
// AnimusCore_v1.vcxproj), same as the other Phase 7/8/9 standalone demos.
// Build and run from a "x64 Native Tools Command Prompt for VS":
//   cl /std:c++17 /EHsc /O2 cluster_latency_bench.cpp /Fe:cluster_latency_bench.exe
//   cluster_latency_bench.exe
#include "animus_cluster.hpp"
#include "../include/animus/thread_affinity.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

using namespace animus;
using namespace animus::transport;
using namespace animus::cluster;

namespace {
    std::wstring cert_dir() { return L"demo_certs\\"; }

    template <typename Pred>
    NodeId wait_for(Pred pred, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            NodeId result = pred();
            if (result != 0) return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }

    struct Stats {
        double min_ms = 0, p50_ms = 0, p90_ms = 0, p99_ms = 0, max_ms = 0, avg_ms = 0;
    };

    Stats summarize(std::vector<double> samples_ms) {
        Stats s{};
        if (samples_ms.empty()) return s;
        std::sort(samples_ms.begin(), samples_ms.end());
        auto pct = [&](double p) {
            size_t idx = static_cast<size_t>(p * (samples_ms.size() - 1));
            return samples_ms[idx];
            };
        s.min_ms = samples_ms.front();
        s.max_ms = samples_ms.back();
        s.p50_ms = pct(0.50);
        s.p90_ms = pct(0.90);
        s.p99_ms = pct(0.99);
        s.avg_ms = std::accumulate(samples_ms.begin(), samples_ms.end(), 0.0) / samples_ms.size();
        return s;
    }

    void print_stats(const char* label, const Stats& s, size_t n) {
        std::printf("%-38s n=%-4zu min=%7.3f ms  p50=%7.3f ms  p90=%7.3f ms  p99=%7.3f ms  max=%7.3f ms  avg=%7.3f ms\n",
            label, n, s.min_ms, s.p50_ms, s.p90_ms, s.p99_ms, s.max_ms, s.avg_ms);
    }
}

int main() {
    std::cout << std::unitbuf;

    // Pin and raise the priority of THIS thread -- the one whose wall-clock
    // gap around each propose() call is what Pass 1/Pass 2 below actually
    // report -- before anything else runs. Deliberately NOT extended to
    // RaftNode's own listener/tick/handler threads (animus_cluster.hpp):
    // this benchmark runs all three simulated cluster nodes' background
    // threads in one shared process, so hard-pinning each of their threads
    // to fixed cores here would create fixed 3-way contention on whichever
    // cores were chosen rather than remove scheduling jitter -- a real
    // one-node-per-process deployment is a different, and more useful,
    // place to pin those. Note this benchmark's own latency floor is set
    // by real loopback TLS network round-trips and majority-commit waits
    // (millisecond scale, not sub-microsecond); pinning here reduces
    // scheduler-induced tail-latency noise on top of that floor, it does
    // not (and cannot) shrink the floor itself.
    if (!animus::sys::pin_current_thread_to_core(0)) {
        std::cerr << "[CLUSTER LATENCY BENCH] warning: could not pin benchmark thread to core 0 (continuing unpinned)\n";
    }
    animus::sys::set_thread_high_priority();

    WinsockInit wsa;
    if (!wsa.ok()) {
        std::cerr << "[CLUSTER LATENCY BENCH] WSAStartup failed\n";
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
        std::cerr << "[CLUSTER LATENCY BENCH] certificate load failed: " << ex.what() << "\n"
            << "Run generate_demo_certs.ps1 first (from AnimusCore_v1/).\n";
        return 1;
    }
    TrustedRoot trust(ca_cert.get());
    PCCERT_CONTEXT raw_cert1 = cert1.get();
    PCCERT_CONTEXT raw_cert2 = cert2.get();
    PCCERT_CONTEXT raw_cert3 = cert3.get();

    constexpr uint16_t kPort1 = 47911, kPort2 = 47912, kPort3 = 47913;
    PeerConfig p1{ 1, "127.0.0.1", kPort1, L"animus-node-1" };
    PeerConfig p2{ 2, "127.0.0.1", kPort2, L"animus-node-2" };
    PeerConfig p3{ 3, "127.0.0.1", kPort3, L"animus-node-3" };

    auto engine1 = Engine::Create(1 << 16);
    auto engine2 = Engine::Create(1 << 16);
    auto engine3 = Engine::Create(1 << 16);
    engine1->start_persistence("cluster_bench_node1.log");
    engine2->start_persistence("cluster_bench_node2.log");
    engine3->start_persistence("cluster_bench_node3.log");

    RaftNode node1(1, { p2, p3 }, std::move(cert1), trust, *engine1, kPort1);
    RaftNode node2(2, { p1, p3 }, std::move(cert2), trust, *engine2, kPort2);
    RaftNode node3(3, { p1, p2 }, std::move(cert3), trust, *engine3, kPort3);

    std::cout << "=== PHASE 10 CLUSTER MULTI-NODE LATENCY BENCHMARK ===\n";
    node1.start();
    node2.start();
    node3.start();

    RaftNode* nodes[3] = { &node1, &node2, &node3 };
    auto by_id = [&](NodeId id) -> RaftNode& { return *nodes[id - 1]; };
    auto leader_of = [&]() -> NodeId {
        if (node1.is_leader()) return 1;
        if (node2.is_leader()) return 2;
        if (node3.is_leader()) return 3;
        return 0;
        };

    NodeId leader_id = wait_for(leader_of, 5000);
    if (leader_id == 0) {
        std::cerr << "[CLUSTER LATENCY BENCH] no leader elected within 5s -- aborting\n";
        node1.stop(); node2.stop(); node3.stop();
        return 1;
    }
    std::cout << "Leader elected: node-" << leader_id << "\n";

    NodeId follower_ids[2];
    {
        int k = 0;
        for (NodeId id = 1; id <= 3; ++id) if (id != leader_id) follower_ids[k++] = id;
    }

    // Pass 1: back-to-back propose() calls with NO extra wait in between --
    // this is the real sustained-throughput number, since the only thing
    // gating each call is propose()'s own majority-commit wait. Measuring
    // throughput from a loop that also waited for full-cluster (not just
    // majority) convergence each round would report this benchmark's own
    // polling overhead as if it were the system's ceiling, not the system's
    // actual capacity -- so that measurement is kept entirely separate, in
    // Pass 2 below.
    constexpr int kThroughputProposals = 300;
    std::vector<double> propose_latency_ms;
    propose_latency_ms.reserve(kThroughputProposals);

    int failures = 0;
    auto pass1_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kThroughputProposals; ++i) {
        AddRuleCommand cmd{
            /*rule_id=*/static_cast<uint32_t>(1000 + i),
            /*event_id=*/static_cast<uint32_t>(2000 + i),
            /*threshold=*/1,
            /*comparator=*/0, // GreaterThan
            /*severity=*/1
        };
        NodeId hint = 0;
        auto t0 = std::chrono::steady_clock::now();
        ProposeResult r = by_id(leader_id).propose(cmd, &hint);
        auto t1 = std::chrono::steady_clock::now();
        if (r != ProposeResult::Ok) {
            ++failures;
            continue;
        }
        propose_latency_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    auto pass1_end = std::chrono::steady_clock::now();
    double pass1_total_s = std::chrono::duration<double>(pass1_end - pass1_start).count();

    std::cout << "\n--- Pass 1: back-to-back proposal throughput ---\n";
    std::cout << "Proposals attempted : " << kThroughputProposals << "  failed: " << failures << "\n";
    std::cout << "Total wall time     : " << pass1_total_s << " s\n";
    std::cout << "Sustained throughput: " << (kThroughputProposals - failures) / pass1_total_s << " proposals/sec\n\n";
    print_stats("propose() call latency (majority)", summarize(propose_latency_ms), propose_latency_ms.size());

    // Pass 2: a smaller, separately-timed batch that also waits (after each
    // propose() already returned) for the SLOWEST follower -- not just the
    // majority propose() itself required -- to catch up, isolating "extra
    // time until the whole cluster (not just a majority) has the write" as
    // its own distinct number.
    constexpr int kConvergenceProposals = 100;
    std::vector<double> full_convergence_extra_ms;
    full_convergence_extra_ms.reserve(kConvergenceProposals);
    int convergence_failures = 0;
    for (int i = 0; i < kConvergenceProposals; ++i) {
        AddRuleCommand cmd{
            /*rule_id=*/static_cast<uint32_t>(5000 + i),
            /*event_id=*/static_cast<uint32_t>(6000 + i),
            /*threshold=*/1,
            /*comparator=*/0, // GreaterThan
            /*severity=*/1
        };
        NodeId hint = 0;
        ProposeResult r = by_id(leader_id).propose(cmd, &hint);
        auto t1 = std::chrono::steady_clock::now();
        if (r != ProposeResult::Ok) {
            ++convergence_failures;
            continue;
        }
        uint64_t index = by_id(leader_id).log_size();
        auto conv_deadline = t1 + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < conv_deadline) {
            if (by_id(follower_ids[0]).committed_count() >= index &&
                by_id(follower_ids[1]).committed_count() >= index) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto t2 = std::chrono::steady_clock::now();
        full_convergence_extra_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    failures += convergence_failures;

    std::cout << "\n--- Pass 2: full-cluster (not just majority) convergence ---\n";
    print_stats("extra time to full-cluster commit", summarize(full_convergence_extra_ms), full_convergence_extra_ms.size());

    node1.stop();
    node2.stop();
    node3.stop();

    delete_persisted_key(raw_cert1);
    delete_persisted_key(raw_cert2);
    delete_persisted_key(raw_cert3);
    std::remove("cluster_bench_node1.log");
    std::remove("cluster_bench_node2.log");
    std::remove("cluster_bench_node3.log");

    std::cout << "\n[CLUSTER LATENCY BENCH] " << (failures == 0 ? "ALL PROPOSALS COMMITTED" : "SOME PROPOSALS FAILED") << "\n";
    return failures == 0 ? 0 : 1;
}
