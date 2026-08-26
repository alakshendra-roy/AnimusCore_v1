#include <iostream>
#include <chrono>
#include <numeric>
#include "animus.hpp"

int main() {
    std::cout << "====================================================\n";
    std::cout << " ANIMUS Core v1.0 - High-Throughput Batch Stress Test\n";
    std::cout << "====================================================\n\n";

    constexpr size_t BATCH_SIZE = 60000;
    constexpr size_t TOTAL_RUNS = 10; // Total 600,000 operations tested

    uint64_t total_ingested = 0;
    double total_execution_ms = 0.0;

    std::cout << "Executing " << TOTAL_RUNS << " sequential stress-test batches ("
        << BATCH_SIZE * TOTAL_RUNS << " total events)...\n\n";

    for (size_t run = 0; run < TOTAL_RUNS; ++run) {
        // Create fresh engine instance per batch to test clean allocations
        auto engine = animus::Engine::Create(65536);

        auto start = std::chrono::high_resolution_clock::now();

        size_t batch_ingested = 0;
        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            if (engine->record(101, static_cast<uint32_t>(i), static_cast<uint64_t>(i))) {
                batch_ingested++;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();

        double batch_ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;

        total_ingested += batch_ingested;
        total_execution_ms += batch_ms;
    }

    double total_ns = total_execution_ms * 1'000'000.0;
    double avg_latency_ns = total_ns / total_ingested;
    double throughput_mops = (total_ingested / (total_execution_ms / 1000.0)) / 1'000'000.0;

    std::cout << "====================================================\n";
    std::cout << " Total Ingested Events : " << total_ingested << "\n";
    std::cout << " Total Test Duration   : " << total_execution_ms << " ms\n";
    std::cout << " Throughput            : " << throughput_mops << " Million ops/sec\n";
    std::cout << " Average Latency / Op  : " << avg_latency_ns << " ns\n";
    std::cout << "====================================================\n";

    return 0;
}