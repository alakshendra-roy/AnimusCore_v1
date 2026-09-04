// Milestone 1 regression test: proves ShmRing<T>'s schema-descriptor
// header (schema_version_hash/payload_size/stride/wire_format, all
// written once at create() and validated again at open() -- see
// include/animus/shm_ipc.hpp and include/animus/schema.hpp) actually
// round-trips real payloads across a genuine OS process boundary for two
// different wire schemas: the 40-byte animus::ExecutionEvent (the
// primary, backward-compatible specialization) and the 64-byte
// animus::schema::OrderBookL2 (the alternate schema schema.hpp defines
// specifically for this test) -- proving the dynamic-schema machinery
// isn't just hardcoded for the one record type ShmRing<T> originally
// shipped with, and also proving the new open()-time schema mismatch
// guard actually rejects a wrong-T attach instead of silently misreading
// bytes.
//
// No C++ test framework here, consistent with tests/test_licensing.cpp
// (CLAUDE.md's zero-dependency rule) -- asserts-by-hand + a PASS/FAIL
// summary + a non-zero exit code on any failure.
//
// "Across processes" is literal: this binary IS both sides of each ring,
// selected by argv, exactly like AnimusCore_v1/shm_ipc_bench.cpp's own
// --consumer/--producer convention -- launch two SEPARATE OS processes
// (see the orchestration this file's own --help text documents, and
// .github/workflows/linux_ci.yml's bash `&`/`wait` pattern for how the
// existing harness_benchmark.cpp + consumer.py pair is driven in CI). The
// consumer owns ring creation and unlink (same convention as
// shm_ipc_bench.cpp: "start the consumer first"); the producer retries
// open() for up to 10 seconds to absorb the startup race.
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/schema.hpp"
#include "../include/animus/execution_event.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using animus::ExecutionEvent;
using animus::schema::OrderBookL2;

namespace {

template <typename T> T make_record(uint64_t i) noexcept;
template <typename T> bool verify_record(const T& got, uint64_t i) noexcept;

template <>
ExecutionEvent make_record<ExecutionEvent>(uint64_t i) noexcept {
    ExecutionEvent ev{};
    ev.sequence = i;
    ev.dispatch_ts_raw = i * 7 + 1;
    ev.price_ticks = static_cast<int64_t>(100000 + (i % 4096));
    ev.quantity = static_cast<int64_t>(1 + (i % 500));
    ev.instrument_id = static_cast<uint32_t>(i % 256);
    ev.flags = 0;
    return ev;
}
template <>
bool verify_record<ExecutionEvent>(const ExecutionEvent& got, uint64_t i) noexcept {
    const ExecutionEvent want = make_record<ExecutionEvent>(i);
    return got.sequence == want.sequence && got.dispatch_ts_raw == want.dispatch_ts_raw &&
           got.price_ticks == want.price_ticks && got.quantity == want.quantity &&
           got.instrument_id == want.instrument_id && got.flags == want.flags;
}

template <>
OrderBookL2 make_record<OrderBookL2>(uint64_t i) noexcept {
    OrderBookL2 ob{};
    ob.sequence = i;
    ob.dispatch_ts_raw = i * 11 + 3;
    ob.instrument_id = static_cast<uint32_t>(i % 128);
    ob.level = static_cast<uint32_t>(i % 10);
    ob.bid_price_ticks = static_cast<int64_t>(90000 + i);
    ob.ask_price_ticks = static_cast<int64_t>(90010 + i);
    ob.bid_quantity = static_cast<int64_t>(10 + (i % 50));
    ob.ask_quantity = static_cast<int64_t>(12 + (i % 60));
    ob.flags = 0;
    return ob;
}
template <>
bool verify_record<OrderBookL2>(const OrderBookL2& got, uint64_t i) noexcept {
    const OrderBookL2 want = make_record<OrderBookL2>(i);
    return std::memcmp(&got, &want, sizeof(OrderBookL2)) == 0;
}

template <typename T, typename WrongT>
int run_consumer(const std::string& name, uint64_t count) {
    using Ring = animus::sys::ipc::ShmRing<T>;
    auto ring = Ring::create(name.c_str(), /*requested_capacity=*/1 << 14);
    if (!ring) {
        std::fprintf(stderr, "[consumer:%s] create('%s') failed\n", animus::schema::Traits<T>::kName, name.c_str());
        return 1;
    }
    ring->mark_consumer_attached();
    std::printf("[consumer:%s] schema_version_hash=0x%016llx payload_size=%zu stride=%zu wire_format=\"%s\"\n",
        animus::schema::Traits<T>::kName,
        static_cast<unsigned long long>(ring->schema_version_hash()),
        ring->payload_size(), ring->stride(), ring->wire_format());

    for (uint64_t i = 0; i < count; ++i) {
        T rec{};
        if (!ring->pop_spin(rec)) {
            std::fprintf(stderr, "[consumer:%s] timed out waiting for record %llu\n",
                animus::schema::Traits<T>::kName, static_cast<unsigned long long>(i));
            return 1;
        }
        if (!verify_record<T>(rec, i)) {
            std::fprintf(stderr, "[consumer:%s] record %llu did not round-trip byte-for-byte\n",
                animus::schema::Traits<T>::kName, static_cast<unsigned long long>(i));
            return 1;
        }
    }
    std::printf("[consumer:%s] all %llu records verified byte-for-byte\n",
        animus::schema::Traits<T>::kName, static_cast<unsigned long long>(count));

    // The actual safety guarantee this milestone adds: attaching to this
    // exact segment with the WRONG record type must be rejected (nullptr),
    // not silently succeed and misread the bytes as WrongT.
    auto wrong = animus::sys::ipc::ShmRing<WrongT>::open(name.c_str());
    if (wrong) {
        std::fprintf(stderr,
            "[consumer:%s] BUG: ShmRing<%s>::open('%s') should have been rejected (segment holds %s, not %s)\n",
            animus::schema::Traits<T>::kName, animus::schema::Traits<WrongT>::kName, name.c_str(),
            animus::schema::Traits<T>::kName, animus::schema::Traits<WrongT>::kName);
        return 1;
    }
    std::printf("[consumer:%s] cross-schema open() with %s correctly rejected\n",
        animus::schema::Traits<T>::kName, animus::schema::Traits<WrongT>::kName);

    Ring::unlink(name.c_str());
    return 0;
}

template <typename T>
int run_producer(const std::string& name, uint64_t count) {
    using Ring = animus::sys::ipc::ShmRing<T>;
    std::unique_ptr<Ring> ring;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        ring = Ring::open(name.c_str());
        if (ring) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!ring) {
        std::fprintf(stderr, "[producer:%s] could not open '%s' within 10s -- start the consumer first\n",
            animus::schema::Traits<T>::kName, name.c_str());
        return 1;
    }
    ring->mark_producer_attached();
    for (uint64_t i = 0; i < count; ++i) {
        const T rec = make_record<T>(i);
        if (!ring->push_spin(rec)) {
            std::fprintf(stderr, "[producer:%s] push_spin failed at record %llu (consumer gone?)\n",
                animus::schema::Traits<T>::kName, static_cast<unsigned long long>(i));
            return 1;
        }
    }
    std::printf("[producer:%s] sent %llu records\n", animus::schema::Traits<T>::kName,
        static_cast<unsigned long long>(count));
    return 0;
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s --consumer <execution_event|orderbook_l2> <ring_name> <count>\n"
        "  %s --producer <execution_event|orderbook_l2> <ring_name> <count>\n"
        "Launch as two SEPARATE OS processes, consumer first (it owns ring\n"
        "creation/unlink) -- see this file's own header comment.\n",
        argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && (std::strcmp(argv[1], "--consumer") == 0 || std::strcmp(argv[1], "--producer") == 0)) {
        const bool is_consumer = std::strcmp(argv[1], "--consumer") == 0;
        const std::string schema = argv[2];
        const std::string name = argv[3];
        const uint64_t count = std::strtoull(argv[4], nullptr, 10);

        if (schema == "execution_event") {
            return is_consumer ? run_consumer<ExecutionEvent, OrderBookL2>(name, count)
                                : run_producer<ExecutionEvent>(name, count);
        }
        if (schema == "orderbook_l2") {
            return is_consumer ? run_consumer<OrderBookL2, ExecutionEvent>(name, count)
                                : run_producer<OrderBookL2>(name, count);
        }
        std::fprintf(stderr, "unknown schema '%s' (expected 'execution_event' or 'orderbook_l2')\n", schema.c_str());
        return 2;
    }
    print_usage(argv[0]);
    return 2;
}
