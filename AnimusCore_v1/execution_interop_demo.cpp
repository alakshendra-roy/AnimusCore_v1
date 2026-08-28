// Phase 7 demo/verification: header-only C++ engine + broker/execution
// interop wrappers, driven with zero DLL, zero Python, and zero ctypes --
// #include "animus.hpp" is the entire dependency footprint.
//
// Intentionally excluded from the default MSBuild targets (see
// AnimusCore_v1.vcxproj): AnimusCore_v1.cpp already owns main() for the
// Application configurations. Build and run standalone, e.g.:
//   g++ -std=c++17 -O2 -pthread execution_interop_demo.cpp -o execution_interop_demo.exe
//   cl /EHsc /std:c++17 /O2 execution_interop_demo.cpp
#include "animus.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>

int main() {
    constexpr size_t TOTAL_ORDERS = 500'000;
    constexpr uint64_t SLOW_FILL_THRESHOLD_NS = 2'000; // flag any fill slower than 2 us

    auto engine = animus::Engine::Create(1 << 20);
    animus::LoopbackBrokerGateway gateway;
    animus::ExecutionClient client(*engine, gateway);

    engine->add_rule(
        /*rule_id=*/1,
        animus::kExecutionLatencyEventId,
        SLOW_FILL_THRESHOLD_NS,
        static_cast<uint8_t>(animus::RuleComparator::GreaterThan),
        /*severity=*/1);

    std::cout << "[EXECUTION INTEROP DEMO] Gateway: " << client.gateway_name() << "\n";
    std::cout << "[EXECUTION INTEROP DEMO] Routing " << TOTAL_ORDERS << " orders through ExecutionClient...\n";

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(TOTAL_ORDERS);

    auto start_total = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 0; i < TOTAL_ORDERS; ++i) {
        animus::OrderRequest request{
            /*client_order_id=*/i,
            /*instrument_id=*/static_cast<uint32_t>(i % 16),
            (i % 2 == 0) ? animus::OrderSide::Buy : animus::OrderSide::Sell,
            animus::OrderType::Limit,
            /*price_ticks=*/100'000 + (i % 500),
            /*quantity=*/100 + (i % 50),
        };
        animus::ExecutionReport report;

        auto t1 = std::chrono::high_resolution_clock::now();
        client.submit(request, report);
        auto t2 = std::chrono::high_resolution_clock::now();
        latencies_ns.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count()));

        if (report.status != animus::ExecStatus::Filled) {
            std::cerr << "[EXECUTION INTEROP DEMO] Unexpected non-fill for order " << i << "\n";
            return 1;
        }
    }

    auto end_total = std::chrono::high_resolution_clock::now();
    double total_duration_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();

    // Drain every ThreatSignal produced by the SLOW_FILL_THRESHOLD_NS rule --
    // proof that a latency-risk check on the execution path is just a normal
    // SOAR rule, evaluated by the exact same engine as telemetry ingestion.
    size_t slow_fills = 0;
    animus::ThreatSignal signal_batch[1024];
    size_t drained;
    while ((drained = engine->poll_signals(signal_batch, 1024)) > 0) {
        slow_fills += drained;
    }

    uint64_t sum_ns = std::accumulate(latencies_ns.begin(), latencies_ns.end(), uint64_t{ 0 });
    double avg_ns = static_cast<double>(sum_ns) / TOTAL_ORDERS;
    std::sort(latencies_ns.begin(), latencies_ns.end());
    uint64_t p99_ns = latencies_ns[static_cast<size_t>(TOTAL_ORDERS * 0.99)];
    double orders_per_sec = TOTAL_ORDERS / (total_duration_ms / 1000.0);

    std::cout << "=== PHASE 7 EXECUTION INTEROP BENCHMARK ===\n";
    std::cout << "Orders Submitted        : " << TOTAL_ORDERS << "\n";
    std::cout << "Total Duration           : " << total_duration_ms << " ms\n";
    std::cout << "Sustained Throughput     : " << orders_per_sec << " orders/sec\n";
    std::cout << "Avg submit() Latency     : " << avg_ns << " ns/order\n";
    std::cout << "p99 submit() Latency     : " << p99_ns << " ns\n";
    std::cout << "Slow-fill threshold      : " << SLOW_FILL_THRESHOLD_NS << " ns\n";
    std::cout << "Slow-fill signals raised : " << slow_fills << "\n";

    return 0;
}
