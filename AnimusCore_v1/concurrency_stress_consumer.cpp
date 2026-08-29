// Phase 22: concurrent multi-tenant stress hardening.
//
// N genuinely concurrent tenant pairs, not multiple producers sharing one
// ring: ShmRing<T> (include/animus/shm_ipc.hpp) is strictly single-
// producer/single-consumer, so "multiple Python producers and C++
// consumers" here means N independent (Python producer process, native
// consumer thread) pairs running at once, one ShmRing<RawEvent> per
// tenant -- the same "isolation is structural" pattern
// shm_ipc_ingest_demo.cpp (Phase 20) and execution_orchestration_demo.py
// (Phase 21) already established, scaled up in tenant count and run
// concurrently instead of sequentially to find the system's aggregate
// throughput ceiling.
//
// Each tenant thread is Phase 20's shm_ipc_ingest_demo.cpp run_consumer
// logic, parameterized by tenant index -- including both fixes that
// commit already had to make: record_batch()'s "stop at the first push
// that fails" remainder is retried (animus::cpu_relax()) until the
// persistence worker catches up, and the signal ring is drained by a
// spin-polled (not sleep_for-polled) background thread, since a
// millisecond of poller inactivity can let an entire signal ring's worth
// of matches accumulate under this kind of load.
//
// A separate background thread samples this process's own resident
// memory (RSS) every ~200ms throughout the run -- the native equivalent
// of benchmarks/stress_test_engine.py's get_rss_bytes(), same Windows
// API, same reasoning.
//
// Usage:
//   concurrency_stress_consumer.exe <tenant_count> <events_per_tenant> <ring_capacity>
//
// Prints exactly one BENCHMARK_RESULT_CONCURRENCY_STRESS {...} JSON line
// to stdout when done (matching animus_benchmark_suite.cpp's own
// JSON-result convention) -- everything else goes to stderr, so a caller
// parsing stdout never has to distinguish diagnostic noise from the result.
#include "animus.hpp"
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/thread_affinity.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <cstdio>
#endif

using animus::RawEvent;
using animus::sys::ipc::ShmRing;

namespace {

    constexpr uint32_t kEventId = 101;
    constexpr size_t kBatchSize = 1024;

    // Same Windows API (GetProcessMemoryInfo) and same reasoning as
    // benchmarks/stress_test_engine.py's get_rss_bytes() -- mirrored here
    // rather than shared, since one is Python/ctypes and this is native
    // C++, but the methodology (and the POSIX fallback) is identical.
    uint64_t get_rss_bytes() {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof(pmc);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
        return static_cast<uint64_t>(pmc.WorkingSetSize);
#else
        FILE* f = std::fopen("/proc/self/status", "r");
        if (!f) return 0;
        char line[256];
        uint64_t kb = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "VmRSS:", 6) == 0) {
                std::sscanf(line + 6, "%llu", reinterpret_cast<unsigned long long*>(&kb));
                break;
            }
        }
        std::fclose(f);
        return kb * 1024;
#endif
    }

    struct TenantResult {
        uint32_t tenant_id = 0;
        uint64_t received = 0;
        uint64_t accepted = 0;
        uint64_t rule_hits = 0;
        long long persisted_bytes = -1;
        bool ok = false;
    };

    bool register_rules(animus::Engine& engine) {
        return engine.add_rule(/*rule_id=*/1, kEventId, /*threshold=*/5000,
            static_cast<uint8_t>(animus::RuleComparator::GreaterThan), /*severity=*/3);
    }

    void tenant_worker(uint32_t tenant_id, uint64_t events_per_tenant, size_t ring_capacity,
        unsigned pin_core, TenantResult* result) {
        result->tenant_id = tenant_id;
        animus::sys::pin_current_thread_to_core(pin_core);
        animus::sys::set_thread_high_priority();

        const std::string ring_name = "AnimusConcurrencyStressT" + std::to_string(tenant_id);
        const std::string log_path = "concurrency_stress_tenant" + std::to_string(tenant_id) + ".bin";
        std::remove(log_path.c_str()); // clear any stale data from a previous run

        auto ring = ShmRing<RawEvent>::create(ring_name.c_str(), ring_capacity);
        if (!ring) {
            std::fprintf(stderr, "[tenant %u] failed to create ring '%s'\n", tenant_id, ring_name.c_str());
            return;
        }
        auto engine = animus::Engine::Create(1 << 16);
        if (!register_rules(*engine)) {
            std::fprintf(stderr, "[tenant %u] add_rule failed\n", tenant_id);
            return;
        }
        engine->start_persistence(log_path);

        std::atomic<bool> stop_poll{ false };
        uint64_t rule_hits = 0;
        std::thread poller([&]() {
            animus::ThreatSignal buf[4096];
            while (!stop_poll.load(std::memory_order_acquire)) {
                size_t n = engine->poll_signals(buf, 4096);
                if (n > 0) {
                    for (size_t i = 0; i < n; ++i) if (buf[i].rule_id == 1) ++rule_hits;
                }
                else {
                    animus::cpu_relax();
                }
            }
            });

        std::vector<RawEvent> batch;
        batch.reserve(kBatchSize);
        uint64_t received = 0, accepted = 0;
        auto flush = [&]() {
            if (batch.empty()) return;
            size_t offset = 0;
            while (offset < batch.size()) {
                size_t pushed = engine->record_batch(batch.data() + offset, batch.size() - offset);
                accepted += pushed;
                offset += pushed;
                if (pushed == 0) animus::cpu_relax();
            }
            batch.clear();
            };

        RawEvent event{};
        while (received < events_per_tenant) {
            if (!ring->pop_spin(event)) {
                std::fprintf(stderr, "[tenant %u] timed out waiting for event %llu\n",
                    tenant_id, static_cast<unsigned long long>(received));
                stop_poll.store(true, std::memory_order_release);
                poller.join();
                return;
            }
            ++received;
            batch.push_back(event);
            if (batch.size() >= kBatchSize) flush();
        }
        flush();

        engine->stop_persistence();
        stop_poll.store(true, std::memory_order_release);
        poller.join();
        // Trailing drain: signals from the last batch that landed after the
        // poller's last check but before it observed stop_poll.
        {
            animus::ThreatSignal buf[4096];
            size_t n;
            while ((n = engine->poll_signals(buf, 4096)) > 0) {
                for (size_t i = 0; i < n; ++i) if (buf[i].rule_id == 1) ++rule_hits;
            }
        }

        long long persisted_bytes = -1;
        {
            // Scoped so log_check is closed (its destructor runs) before
            // the std::remove() below -- std::remove() on Windows fails
            // silently (return value unchecked, no exception) on a file
            // that still has an open handle, unlike POSIX where unlinking
            // an open file is fine. Left unscoped once, every tenant's
            // temp log file survived every run instead of being cleaned up.
            std::ifstream log_check(log_path, std::ios::binary | std::ios::ate);
            if (log_check.is_open()) persisted_bytes = static_cast<long long>(log_check.tellg());
        }

        result->received = received;
        result->accepted = accepted;
        result->rule_hits = rule_hits;
        result->persisted_bytes = persisted_bytes;
        result->ok = (received == events_per_tenant) && (accepted == received) && (rule_hits == accepted)
            && (persisted_bytes == static_cast<long long>(accepted) * static_cast<long long>(sizeof(animus::TelemetryPayload)));

        ShmRing<RawEvent>::unlink(ring_name.c_str());
        std::remove(log_path.c_str());
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: %s <tenant_count> <events_per_tenant> <ring_capacity>\n", argv[0]);
        return 2;
    }
    const uint32_t tenant_count = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10));
    const uint64_t events_per_tenant = std::strtoull(argv[2], nullptr, 10);
    const size_t ring_capacity = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    if (tenant_count == 0 || events_per_tenant == 0 || ring_capacity == 0) {
        std::fprintf(stderr, "tenant_count, events_per_tenant, and ring_capacity must all be > 0\n");
        return 2;
    }

    const unsigned cpu_count = std::max(1u, std::thread::hardware_concurrency());

    // Background RSS sampler: started before any tenant thread so the very
    // first sample is a genuine "just started" baseline; the *warm*
    // baseline used for leak-growth comparison is chosen later (the first
    // sample taken after some ramp-up), not this one -- same "cold
    // baseline vs warm baseline" distinction Phase 12's own memory check
    // already established (a one-time lazy-page-commit jump right after
    // startup is not a leak signature).
    std::atomic<bool> stop_sampler{ false };
    std::mutex samples_mutex;
    std::vector<std::pair<uint64_t, uint64_t>> samples; // (ms_since_start, rss_bytes)
    const auto t0 = std::chrono::steady_clock::now();
    std::thread sampler([&]() {
        while (!stop_sampler.load(std::memory_order_acquire)) {
            uint64_t ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
            uint64_t rss = get_rss_bytes();
            {
                std::lock_guard<std::mutex> lock(samples_mutex);
                samples.emplace_back(ms, rss);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        });

    std::fprintf(stderr, "[consumer] starting %u tenants, %llu events each, ring capacity %zu...\n",
        tenant_count, static_cast<unsigned long long>(events_per_tenant), ring_capacity);

    std::vector<TenantResult> results(tenant_count);
    std::vector<std::thread> tenants;
    tenants.reserve(tenant_count);
    for (uint32_t i = 0; i < tenant_count; ++i) {
        tenants.emplace_back(tenant_worker, i, events_per_tenant, ring_capacity, i % cpu_count, &results[i]);
    }
    for (auto& t : tenants) t.join();

    const auto t1 = std::chrono::steady_clock::now();
    // Explicit, precisely-timed sample taken the instant every tenant has
    // joined (so every Engine/ShmRing has definitely been destructed) --
    // NOT samples.back() from the periodic 200ms-cadence sampler below,
    // whose last tick can land anywhere up to 200ms before or after this
    // exact point, making it noisy for a "right after full teardown"
    // reading specifically (confirmed empirically: it swung from ~15MB to
    // ~75MB between two otherwise-identical runs, purely from where the
    // last periodic tick happened to fall).
    const uint64_t post_teardown_residual = get_rss_bytes();
    stop_sampler.store(true, std::memory_order_release);
    sampler.join();

    uint64_t total_received = 0, total_accepted = 0, total_rule_hits = 0;
    bool all_ok = true;
    for (const auto& r : results) {
        total_received += r.received;
        total_accepted += r.accepted;
        total_rule_hits += r.rule_hits;
        if (!r.ok) {
            all_ok = false;
            std::fprintf(stderr, "[consumer] tenant %u FAILED: received=%llu accepted=%llu rule_hits=%llu persisted_bytes=%lld\n",
                r.tenant_id, static_cast<unsigned long long>(r.received), static_cast<unsigned long long>(r.accepted),
                static_cast<unsigned long long>(r.rule_hits), r.persisted_bytes);
        }
    }

    // Warm baseline: the first sample taken at least 200ms in (i.e. the
    // second sample overall, if one exists) rather than the very first --
    // avoids counting the one-time ring/engine allocation jump as if it
    // were growth-under-load. Falls back to the first sample if the run
    // was too short to produce a second one.
    //
    // Two DIFFERENT questions, deliberately not conflated into one
    // "growth %" the way Phase 12's single-long-lived-Engine check could
    // get away with: this test's tenants each do a FINITE amount of work
    // and then tear down (unlike Phase 12's engine, which stayed alive for
    // the whole measurement), so "warm vs the sample after everyone
    // finished" is not a leak signal here -- it is proper cleanup, and an
    // earlier version of this file measured exactly that and reported a
    // nonsensical "-82% growth" as if shrinking were suspicious.
    //   1. growth_during_load_pct: warm -> peak, while all tenants are
    //      still concurrently active -- does sustained multi-tenant
    //      contention itself cause runaway growth? A real leak would show
    //      up here, growing without bound as more events are processed.
    //   2. post_teardown_residual_mb: the sample after every tenant thread
    //      has joined and freed its own Engine/ShmRing -- reported as an
    //      absolute figure, not a percentage, and checked for
    //      run-to-run STABILITY (not "did it shrink") by the orchestrator:
    //      a residual that stays flat across repeated runs is healthy
    //      (some fixed CRT/allocator/DLL overhead that never fully
    //      returns to the OS); one that climbs run over run would be the
    //      real leak signature to watch for.
    const uint64_t cold_rss = samples.empty() ? 0 : samples[0].second;
    const uint64_t warm_rss = samples.size() > 1 ? samples[1].second : cold_rss;
    uint64_t peak_rss = 0;
    for (const auto& s : samples) peak_rss = std::max(peak_rss, s.second);
    const double growth_during_load_pct = warm_rss > 0
        ? (static_cast<double>(peak_rss) - static_cast<double>(warm_rss)) / static_cast<double>(warm_rss) * 100.0
        : 0.0;

    const double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    const double throughput = elapsed_s > 0 ? static_cast<double>(total_accepted) / elapsed_s : 0.0;

    std::fprintf(stderr, "[consumer] done: %llu/%llu received, %llu accepted, %llu rule hits, elapsed=%.2fs\n",
        static_cast<unsigned long long>(total_received),
        static_cast<unsigned long long>(tenant_count) * events_per_tenant,
        static_cast<unsigned long long>(total_accepted), static_cast<unsigned long long>(total_rule_hits), elapsed_s);

    std::printf("BENCHMARK_RESULT_CONCURRENCY_STRESS {"
        "\"tenant_count\":%u,\"events_per_tenant\":%llu,\"ring_capacity\":%zu,"
        "\"total_received\":%llu,\"total_accepted\":%llu,\"total_rule_hits\":%llu,"
        "\"elapsed_s\":%.4f,\"combined_throughput_eps\":%.1f,"
        "\"cold_rss_mb\":%.2f,\"warm_rss_mb\":%.2f,\"peak_rss_mb\":%.2f,"
        "\"growth_during_load_pct\":%.2f,\"post_teardown_residual_mb\":%.2f,"
        "\"all_tenants_ok\":%s}\n",
        tenant_count, static_cast<unsigned long long>(events_per_tenant), ring_capacity,
        static_cast<unsigned long long>(total_received), static_cast<unsigned long long>(total_accepted),
        static_cast<unsigned long long>(total_rule_hits),
        elapsed_s, throughput,
        cold_rss / (1024.0 * 1024.0), warm_rss / (1024.0 * 1024.0), peak_rss / (1024.0 * 1024.0),
        growth_during_load_pct, post_teardown_residual / (1024.0 * 1024.0),
        all_ok ? "true" : "false");
    std::fflush(stdout);

    return all_ok ? 0 : 1;
}
