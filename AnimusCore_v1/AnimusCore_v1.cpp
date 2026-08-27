#include "animus.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>

int main() {
    constexpr size_t TOTAL_EVENTS = 600'000;
    // Increased capacity to 1MB buffer (1048576) to eliminate thread contention
    auto engine = animus::Engine::Create(1048576);

    // Initialize Phase 3 Asynchronous Persistence Worker
    std::cout << "[ANIMUS Core] Launching Asynchronous Persistence Worker...\n";
    engine->start_persistence("telemetry_data.bin");

    std::vector<uint64_t> latencies;
    latencies.reserve(TOTAL_EVENTS);

    std::cout << "[ANIMUS Core] Executing Phase 3 Stress Test (" << TOTAL_EVENTS << " events)...\n";

    auto start_total = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < TOTAL_EVENTS; ++i) {
        auto t1 = std::chrono::high_resolution_clock::now();

        // Hot-path record
        while (!engine->record(101, static_cast<uint32_t>(i), i * 5)) {
            // Spin-wait briefly if ring buffer is temporarily full
        }

        auto t2 = std::chrono::high_resolution_clock::now();
        uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        latencies.push_back(elapsed_ns);
    }

    auto end_total = std::chrono::high_resolution_clock::now();
    double total_duration_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();

    // Drain remaining items and close disk stream
    engine->stop_persistence();
    std::cout << "[ANIMUS Core] Persistence Worker safely drained and shut down.\n\n";

    // Metrics Calculation
    uint64_t sum_latencies = std::accumulate(latencies.begin(), latencies.end(), 0ULL);
    double avg_latency = static_cast<double>(sum_latencies) / TOTAL_EVENTS;

    std::sort(latencies.begin(), latencies.end());
    uint64_t p99_latency = latencies[static_cast<size_t>(TOTAL_EVENTS * 0.99)];
    double ops_per_sec = (TOTAL_EVENTS / (total_duration_ms / 1000.0)) / 1'000'000.0;

    std::cout << "=== PHASE 3 BENCHMARK RESULTS (ASYNC DISK PERSISTENCE) ===\n";
    std::cout << "Total Ingested Events : " << TOTAL_EVENTS << "\n";
    std::cout << "Total Test Duration   : " << total_duration_ms << " ms\n";
    std::cout << "Sustained Throughput  : " << ops_per_sec << " Million ops/sec\n";
    std::cout << "Avg Hot-Path Latency  : " << avg_latency << " ns / op\n";
    std::cout << "p99 Tail Latency      : " << p99_latency << " ns\n";
    std::cout << "Binary Output File    : telemetry_data.bin\n";

    return 0;
}