// Wires include/animus/shm_ipc.hpp's ShmRing<T> into a real ingestion
// pipeline, not just a synthetic latency probe (shm_ipc_bench.cpp already
// covers that). Two genuinely separate OS processes: a producer generates
// telemetry events and pushes them across ShmRing<animus::RawEvent>; a
// consumer drains them and feeds them into a real animus::Engine --
// add_rule(), record_batch(), start_persistence()/stop_persistence(),
// poll_signals() -- exactly the same native ingestion pipeline
// ingest_engine.py drives from Python via ctypes, except the events arrive
// over zero-copy shared memory from an entirely separate process instead
// of from Python-level record_events_batch() calls in the same process.
//
// animus::RawEvent (animus.hpp) is used directly as ShmRing<T>'s record
// type rather than a demo-local struct: it's already the exact 16-byte,
// trivially-copyable type Engine::record_batch() takes, so there is no
// translation step between "what came off the ring" and "what the engine
// ingests" -- draining a batch off the ring IS building the array
// record_batch() wants, not a separate conversion.
//
// Usage:
//   shm_ipc_ingest_demo.exe --consumer <ring_name> <total_events> <log_path>
//   shm_ipc_ingest_demo.exe --producer <ring_name> <total_events>
//
// Start the consumer first -- it owns ring creation/unlink and the Engine
// instance, same "the process that should be up first, is" convention as
// shm_ipc_bench.cpp and cluster_latency_bench.cpp. Unlike shm_ipc_bench.cpp,
// this is deliberately NOT lockstep: an unpaced producer bursting into a
// generously-capacity ring is exactly how ingest_engine.py's own producer
// threads behave, and this demo exists to measure real ingestion
// throughput/correctness through this transport, not per-message transit
// latency (shm_ipc_bench.cpp already covers that, with the pacing genuine
// latency measurement requires).
//
// Zero-loss by design: ShmRing<RawEvent> never drops anything (it's a
// bounded lock-free ring, not a lossy queue), and record_batch()'s
// "stop at the first push that fails" remainder is retried with
// animus::cpu_relax() until the engine's own persistence worker catches
// up, rather than being counted as dropped -- see run_consumer's flush()
// for a real, in-development instance of this: an earlier version called
// record_batch() exactly once per batch, and a fast unthrottled consumer
// loop could silently discard most of a run's events despite ShmRing
// itself having delivered every one of them intact.
#include "animus.hpp"
#include "../include/animus/shm_ipc.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using animus::RawEvent;
using animus::sys::ipc::ShmRing;

namespace {

    constexpr uint32_t kEventId = 101;
    constexpr size_t kBatchSize = 1024;

    // Matches ingest_engine.py's own rule setup exactly (RULE_EVENT_ID=101,
    // rule 1 matches every event this demo's producer emits, rule 2 never
    // matches) -- so a run through this native/ShmRing transport and a run
    // through ingest_engine.py's ctypes transport are directly comparable,
    // not two different pipelines wearing the same event_id.
    bool register_rules(animus::Engine& engine) {
        if (!engine.add_rule(/*rule_id=*/1, kEventId, /*threshold=*/5000,
            static_cast<uint8_t>(animus::RuleComparator::GreaterThan), /*severity=*/3)) {
            return false;
        }
        if (!engine.add_rule(/*rule_id=*/2, kEventId, /*threshold=*/10,
            static_cast<uint8_t>(animus::RuleComparator::LessThan), /*severity=*/1)) {
            return false;
        }
        return true;
    }

    int run_consumer(const std::string& ring_name, uint64_t total_events, const std::string& log_path) {
        animus::sys::pin_current_thread_to_core(0);
        animus::sys::set_thread_high_priority();

        auto ring = ShmRing<RawEvent>::create(ring_name.c_str(), /*requested_capacity=*/1 << 16);
        if (!ring) {
            std::fprintf(stderr, "[shm_ingest][consumer] failed to create ring '%s'\n", ring_name.c_str());
            return 1;
        }
        std::fprintf(stderr, "[shm_ingest][consumer] ring '%s' created (capacity=%zu), waiting for producer...\n",
            ring_name.c_str(), ring->capacity());

        auto engine = animus::Engine::Create(1 << 16);
        if (!register_rules(*engine)) {
            std::fprintf(stderr, "[shm_ingest][consumer] add_rule failed\n");
            return 1;
        }
        engine->start_persistence(log_path);

        // The signal ring is a SEPARATE bounded ring from the telemetry
        // ring (EngineImpl::signal_ring_, same default capacity), and
        // nothing drains it unless something calls poll_signals() --
        // Engine::record_batch()'s own retry above only protects the
        // telemetry side. With every one of this demo's events matching
        // rule 1, a single poll_signals() call made only after the run
        // finishes hits exactly the documented "signal ring saturates
        // under high fan-out with no concurrent poll_signals() consumer"
        // limit (BENCHMARKS.md's Phase 4 Known Limit) the moment more than
        // one ring's worth of matches occurs -- silently dropping the
        // remainder despite every underlying event having been ingested
        // correctly. Fixed the same way ingest_engine.py's own producer/
        // consumer harness already does: a background thread continuously
        // drains poll_signals() while ingestion is still running, not only
        // once at the end. Spin-polled with animus::cpu_relax(), not a
        // millisecond sleep_for like ingest_engine.py's own Python poller
        // uses -- that first version of this loop still dropped most
        // signals even though it polled continuously, because this
        // native/ShmRing pipeline generates matches far faster than
        // ingest_engine.py's ctypes-throttled one does: with a 1ms sleep
        // between empty polls, the persistence worker can fill an entire
        // signal-ring's worth of matches (and start silently dropping the
        // rest, same Known Limit as before) *during* a single sleep
        // interval. A tight spin keeps this poller's worst-case reaction
        // latency at the same nanosecond scale as the rest of this
        // pipeline instead.
        std::atomic<bool> stop_poll{ false };
        std::vector<animus::ThreatSignal> collected_signals;
        std::thread poller([&]() {
            animus::sys::pin_current_thread_to_core(2);
            animus::sys::set_thread_high_priority();
            animus::ThreatSignal buf[4096];
            while (!stop_poll.load(std::memory_order_acquire)) {
                size_t n = engine->poll_signals(buf, 4096);
                if (n > 0) {
                    collected_signals.insert(collected_signals.end(), buf, buf + n);
                }
                else {
                    animus::cpu_relax();
                }
            }
            });

        std::vector<RawEvent> batch;
        batch.reserve(kBatchSize);
        uint64_t received = 0, accepted = 0;

        // record_batch() never blocks -- it stops at the first push that
        // fails and returns how many of the batch actually made it in
        // (see animus.hpp's own docstring on Engine::record_batch). A
        // failed push here only ever means the engine's OWN internal ring
        // is momentarily full because its persistence worker hasn't caught
        // up yet -- not that the data is gone (ShmRing already delivered
        // it losslessly) -- so the remainder is retried with
        // animus::cpu_relax() between attempts until the worker catches up,
        // rather than counted as dropped. An earlier version of this loop
        // called record_batch() exactly once per batch and treated
        // anything left over as "rejected" -- correct per record_batch()'s
        // own contract, but it meant this demo's own draining loop
        // (nothing but a pop + a single non-retrying push) could outrun
        // the engine's persistence worker and silently discard the
        // majority of a run's events despite ShmRing itself never having
        // lost a single one. Fixed to retry, matching the "only the
        // caller's own choice not to retry loses data over a momentarily
        // full ring" pattern the rest of this codebase's push_spin/
        // pop_spin already establish.
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

        const auto t0 = std::chrono::steady_clock::now();
        RawEvent event{};
        while (received < total_events) {
            if (!ring->pop_spin(event)) {
                std::fprintf(stderr, "[shm_ingest][consumer] timed out waiting for event %llu\n",
                    static_cast<unsigned long long>(received));
                return 1;
            }
            ++received;
            batch.push_back(event);
            if (batch.size() >= kBatchSize) flush();
        }
        flush(); // final partial batch
        const auto t1 = std::chrono::steady_clock::now();

        // Blocks until the persistence worker has fully drained everything
        // record_batch() above already pushed, so persisted_bytes/rule
        // matches below reflect every accepted event, same convention as
        // ingest_engine.py's stop_logging().
        engine->stop_persistence();

        stop_poll.store(true, std::memory_order_release);
        poller.join();
        // Final drain: catches any signals pushed in the last batch before
        // the poller thread observed stop_poll, same convention as
        // ingest_engine.py's own trailing drain after its poller stops.
        animus::ThreatSignal trailing[4096];
        size_t n;
        while ((n = engine->poll_signals(trailing, 4096)) > 0) {
            collected_signals.insert(collected_signals.end(), trailing, trailing + n);
        }

        uint64_t rule1_hits = 0, rule2_hits = 0;
        for (const auto& signal : collected_signals) {
            if (signal.rule_id == 1) ++rule1_hits;
            else if (signal.rule_id == 2) ++rule2_hits;
        }
        const uint64_t total_signals = collected_signals.size();

        const double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
        const double throughput = elapsed_s > 0 ? static_cast<double>(received) / elapsed_s : 0.0;

        std::ifstream log_check(log_path, std::ios::binary | std::ios::ate);
        const long long persisted_bytes = log_check.is_open() ? static_cast<long long>(log_check.tellg()) : -1;

        std::printf("============================================================\n");
        std::printf("[shm_ingest] Events received:        %llu\n", static_cast<unsigned long long>(received));
        std::printf("[shm_ingest] Events accepted:         %llu (zero-loss: record_batch() remainders are retried, never dropped)\n",
            static_cast<unsigned long long>(accepted));
        std::printf("[shm_ingest] Total wall time:         %.2f ms\n", elapsed_s * 1000.0);
        std::printf("[shm_ingest] Throughput:              %.0f events/sec\n", throughput);
        std::printf("[shm_ingest] Rule signals detected:   %llu (rule 1: %llu, rule 2: %llu)\n",
            static_cast<unsigned long long>(total_signals),
            static_cast<unsigned long long>(rule1_hits), static_cast<unsigned long long>(rule2_hits));
        std::printf("[shm_ingest] Persisted bytes:         %lld\n", persisted_bytes);
        std::printf("============================================================\n");

        bool ok = true;
        if (received != total_events) {
            std::fprintf(stderr, "[shm_ingest] FAIL: received %llu, expected %llu\n",
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(total_events));
            ok = false;
        }
        if (accepted != received) {
            std::fprintf(stderr, "[shm_ingest] FAIL: accepted %llu, received %llu -- record_batch() retry loop should make these equal\n",
                static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(received));
            ok = false;
        }
        if (rule1_hits != accepted) {
            std::fprintf(stderr, "[shm_ingest] FAIL: rule 1 should match every accepted event (%llu accepted, %llu matched)\n",
                static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(rule1_hits));
            ok = false;
        }
        if (rule2_hits != 0) {
            std::fprintf(stderr, "[shm_ingest] FAIL: rule 2 should never match, got %llu hits\n",
                static_cast<unsigned long long>(rule2_hits));
            ok = false;
        }
        const long long expected_bytes = static_cast<long long>(accepted) * static_cast<long long>(sizeof(animus::TelemetryPayload));
        if (persisted_bytes != expected_bytes) {
            std::fprintf(stderr, "[shm_ingest] FAIL: persisted %lld bytes, expected %lld (%llu records * %zu bytes/record)\n",
                persisted_bytes, expected_bytes, static_cast<unsigned long long>(accepted), sizeof(animus::TelemetryPayload));
            ok = false;
        }
        std::printf("[shm_ingest] %s\n", ok ? "End-to-end verification passed." : "End-to-end verification FAILED.");

        ShmRing<RawEvent>::unlink(ring_name.c_str());
        return ok ? 0 : 1;
    }

    int run_producer(const std::string& ring_name, uint64_t total_events) {
        animus::sys::pin_current_thread_to_core(1);
        animus::sys::set_thread_high_priority();

        std::unique_ptr<ShmRing<RawEvent>> ring;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            ring = ShmRing<RawEvent>::open(ring_name.c_str());
            if (ring) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!ring) {
            std::fprintf(stderr, "[shm_ingest][producer] could not open ring '%s' within 5s -- start the consumer first\n",
                ring_name.c_str());
            return 1;
        }
        std::fprintf(stderr, "[shm_ingest][producer] ring '%s' opened, sending %llu events...\n",
            ring_name.c_str(), static_cast<unsigned long long>(total_events));

        for (uint64_t i = 0; i < total_events; ++i) {
            const RawEvent event{ kEventId, static_cast<uint32_t>(i & 0xFFFFFFFFu), /*metric_value=*/9999 };
            if (!ring->push_spin(event)) {
                std::fprintf(stderr, "[shm_ingest][producer] push_spin failed at event %llu (consumer gone?)\n",
                    static_cast<unsigned long long>(i));
                return 1;
            }
        }
        std::fprintf(stderr, "[shm_ingest][producer] done: sent %llu events\n", static_cast<unsigned long long>(total_events));
        return 0;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--consumer") == 0) {
        const std::string ring_name = argv[2];
        const uint64_t total_events = std::strtoull(argv[3], nullptr, 10);
        const std::string log_path = argc >= 5 ? argv[4] : "shm_ingest_demo.bin";
        return run_consumer(ring_name, total_events, log_path);
    }
    if (argc >= 4 && std::strcmp(argv[1], "--producer") == 0) {
        const std::string ring_name = argv[2];
        const uint64_t total_events = std::strtoull(argv[3], nullptr, 10);
        return run_producer(ring_name, total_events);
    }
    std::fprintf(stderr,
        "Usage:\n"
        "  %s --consumer <ring_name> <total_events> [log_path]\n"
        "  %s --producer <ring_name> <total_events>\n"
        "Launch these as two SEPARATE OS processes, consumer first (it owns\n"
        "ring creation/unlink and the Engine instance).\n",
        argv[0], argv[0]);
    return 2;
}
