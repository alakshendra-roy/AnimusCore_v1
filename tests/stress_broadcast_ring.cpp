// Hostile multi-core stress harness for include/animus/broadcast_ring.hpp.
//
// One producer publishes kTotalEvents sequentially numbered, self-checking
// 64-byte TickEvents into a BroadcastRing<TickEvent> (capacity 4096) backed by
// a named OS shared-memory segment, as fast as it can. Two reader threads,
// each with its own view of the ring (its own cursor and drop counter, exactly
// as two processes would have), race it:
//
//   A (greedy)  polls in a tight loop and keeps up as long as the CPU allows.
//   B (lagging) takes kLaggingBatch records per poll and injects a 1 us sleep
//               every kLaggingSleepEvery polls, stretched to a kLagFloorNs stall
//               (see inject_lag), so it can never keep pace: the writer laps it
//               again and again and it lives in the snap-forward path.
//
// Every record either reader accepts is checked for:
//   - torn reads:      price_sum == bid + ask
//   - mixed-lap reads: bid/ask are a pure function of seq, so a record stitched
//                      from two different writes cannot pass
//   - position:        a delivered batch must be the contiguous run of ring
//                      indices ending at the reader's cursor (seq == index)
//   - ordering:        seq strictly increasing, timestamp_ns non-decreasing
//   - accounting:      received + dropped == the reader's cursor after every
//                      poll, and == kTotalEvents once the producer is done
// Exit code 0 only if all of that holds for both readers AND reader B really
// was lapped (dropped > 0) -- a run that never exercised the wrap-around path
// proves nothing, so it is reported as a failure, not a pass.
//
// Note for sanitizer runs: BroadcastRing copies slots with a plain memcpy and
// documents that formal data race on purpose (see broadcast_ring.hpp), so
// ThreadSanitizer is expected to flag it; this harness is what checks that the
// protocol nonetheless never lets a torn record through.
//
// Build:  g++ -std=c++17 -O3 -pthread tests/stress_broadcast_ring.cpp -o stress_broadcast_ring
//         cl /std:c++17 /O2 /EHsc tests\stress_broadcast_ring.cpp
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>

#include "../include/animus/broadcast_ring.hpp"
#include "../include/animus/shm_ipc.hpp" // SharedMemoryRegion only

namespace {

struct alignas(64) TickEvent {
    uint64_t seq;
    uint64_t timestamp_ns;
    double bid;
    double ask;
    double price_sum; // invariant: exactly bid + ask
    uint8_t padding[64 - sizeof(uint64_t) * 2 - sizeof(double) * 3];
};
static_assert(sizeof(TickEvent) == 64, "must be 64B");
static_assert(std::is_trivially_copyable<TickEvent>::value, "lives in raw shared memory");

using Ring = animus::sys::ipc::BroadcastRing<TickEvent>;

constexpr uint64_t kTotalEvents = 10'000'000;
constexpr size_t kCapacity = 4096;
constexpr size_t kBatch = 256;        // greedy reader's poll size (also the scratch size)
constexpr size_t kLaggingBatch = 8;   // small on purpose: <= 500 * 8 records consumed per sleep
constexpr uint64_t kLaggingSleepEvery = 500;
// A 4096-slot ring is lapped in ~0.3 ms at 12 M events/s, so a 1 ms stall
// guarantees a lap at any producer rate above ~4 M events/s.
constexpr uint64_t kLagFloorNs = 1'000'000;

// Every field of a record is a pure function of its seq, so the checker
// needs no side channel to know what a correct record looks like.
inline double bid_for(uint64_t seq) { return 100.0 + static_cast<double>(seq) * 0.25; }
inline double ask_for(uint64_t seq) { return bid_for(seq) + 0.5; }

inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The requested 1 us sleep, stretched to at least kLagFloorNs. sleep_for(1us)
// alone is not a reliable stall: with millisecond timer granularity (Windows,
// MinGW) it degrades to a yield and the reader never falls behind at all.
void inject_lag() {
    const uint64_t t0 = now_ns();
    std::this_thread::sleep_for(std::chrono::microseconds(1));
    while (now_ns() - t0 < kLagFloorNs) {}
}

struct ReaderStats {
    uint64_t received = 0;
    uint64_t dropped = 0;
    uint64_t polls = 0;
    uint64_t empty_polls = 0;
    uint64_t torn_reads = 0;        // price_sum != bid + ask
    uint64_t field_mismatches = 0;  // bid/ask inconsistent with seq (mixed-lap record)
    uint64_t position_errors = 0;   // batch is not the run of indices ending at the cursor
    uint64_t order_errors = 0;      // seq not strictly increasing
    uint64_t timestamp_errors = 0;  // timestamp_ns went backwards
    uint64_t accounting_errors = 0; // received + dropped != cursor
    uint64_t max_batch = 0;

    uint64_t corruptions() const {
        return torn_reads + field_mismatches + position_errors + order_errors + timestamp_errors + accounting_errors;
    }
};

struct Shared {
    std::atomic<bool> start{false};
    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> producer_ns{0};
};

void producer(Ring ring, Shared& shared) {
    while (!shared.start.load(std::memory_order_acquire)) {}
    TickEvent ev{}; // padding stays zero
    const uint64_t t0 = now_ns();
    for (uint64_t seq = 0; seq < kTotalEvents; ++seq) {
        ev.seq = seq;
        ev.timestamp_ns = now_ns();
        ev.bid = bid_for(seq);
        ev.ask = ask_for(seq);
        ev.price_sum = ev.bid + ev.ask;
        ring.publish(ev);
    }
    shared.producer_ns.store(now_ns() - t0, std::memory_order_relaxed);
    shared.producer_done.store(true, std::memory_order_release);
}

void reader(Ring ring, Shared& shared, size_t batch_size, uint64_t sleep_every, ReaderStats& st) {
    TickEvent batch[kBatch];
    uint64_t last_seq = 0;
    uint64_t last_ts = 0;
    bool have_last = false;

    while (!shared.start.load(std::memory_order_acquire)) {}
    for (;;) {
        // Sampled BEFORE the poll: if it is already set, every record the
        // writer will ever publish is visible to that poll.
        const bool done = shared.producer_done.load(std::memory_order_acquire);
        const size_t n = ring.poll(batch, batch_size);
        ++st.polls;

        if (n == 0) {
            if (done && ring.read_cursor() >= ring.writer_cursor()) break;
            ++st.empty_polls;
        } else {
            const uint64_t cursor = ring.read_cursor();
            if (n > st.max_batch) st.max_batch = n;
            for (size_t i = 0; i < n; ++i) {
                const TickEvent& e = batch[i];
                if (e.price_sum != e.bid + e.ask) ++st.torn_reads;
                if (e.bid != bid_for(e.seq) || e.ask != ask_for(e.seq)) ++st.field_mismatches;
                if (e.seq != cursor - n + i) ++st.position_errors;
                if (have_last && e.seq <= last_seq) ++st.order_errors;
                if (have_last && e.timestamp_ns < last_ts) ++st.timestamp_errors;
                last_seq = e.seq;
                last_ts = e.timestamp_ns;
                have_last = true;
            }
            st.received += n;
            if (st.received + ring.dropped() != cursor) ++st.accounting_errors;
        }

        if (sleep_every != 0 && st.polls % sleep_every == 0) {
            inject_lag();
        }
    }
    st.dropped = ring.dropped();
}

bool report_reader(const char* name, const ReaderStats& st, bool must_have_dropped) {
    const bool totals_ok = st.received + st.dropped == kTotalEvents;
    const bool clean = st.corruptions() == 0;
    const bool exercised = !must_have_dropped || st.dropped > 0;
    std::printf("  %-9s received %10llu   dropped %10llu   sum %10llu  %s\n", name,
        static_cast<unsigned long long>(st.received), static_cast<unsigned long long>(st.dropped),
        static_cast<unsigned long long>(st.received + st.dropped),
        totals_ok ? "(== produced)" : "(MISMATCH vs produced)");
    std::printf("            polls %llu (%llu empty), largest batch %llu\n",
        static_cast<unsigned long long>(st.polls), static_cast<unsigned long long>(st.empty_polls),
        static_cast<unsigned long long>(st.max_batch));
    std::printf("            torn %llu, mixed-lap %llu, position %llu, order %llu, timestamp %llu, accounting %llu  -> %s\n",
        static_cast<unsigned long long>(st.torn_reads), static_cast<unsigned long long>(st.field_mismatches),
        static_cast<unsigned long long>(st.position_errors), static_cast<unsigned long long>(st.order_errors),
        static_cast<unsigned long long>(st.timestamp_errors), static_cast<unsigned long long>(st.accounting_errors),
        clean ? "clean" : "CORRUPTION");
    if (!exercised) std::printf("            FAIL: never lapped -- the wrap-around path was not exercised\n");
    return totals_ok && clean && exercised;
}

} // namespace

int main() {
    const std::string name = "animus_stress_broadcast_" + std::to_string(now_ns());

    animus::sys::ipc::SharedMemoryRegion region;
    if (!animus::sys::ipc::SharedMemoryRegion::create(name.c_str(), Ring::required_bytes(kCapacity), region)) {
        std::fprintf(stderr, "error: could not create shared-memory segment '%s'\n", name.c_str());
        return 2;
    }
    Ring writer_view, reader_a_view, reader_b_view;
    if (!Ring::init(region.data(), region.size(), kCapacity, writer_view) ||
        !Ring::attach(region.data(), region.size(), reader_a_view) ||
        !Ring::attach(region.data(), region.size(), reader_b_view)) {
        std::fprintf(stderr, "error: could not initialise/attach BroadcastRing<TickEvent>\n");
        animus::sys::ipc::SharedMemoryRegion::unlink(name.c_str());
        return 2;
    }

    std::printf("BroadcastRing<TickEvent> stress: %llu events, capacity %zu, %zu-byte slots, %u hardware threads\n",
        static_cast<unsigned long long>(kTotalEvents), kCapacity, sizeof(TickEvent),
        std::thread::hardware_concurrency());

    Shared shared;
    ReaderStats stats_a, stats_b;
    std::thread tp(producer, writer_view, std::ref(shared));
    std::thread ta(reader, reader_a_view, std::ref(shared), kBatch, uint64_t{0}, std::ref(stats_a));
    std::thread tb(reader, reader_b_view, std::ref(shared), kLaggingBatch, kLaggingSleepEvery, std::ref(stats_b));

    const uint64_t t0 = now_ns();
    shared.start.store(true, std::memory_order_release);
    tp.join();
    ta.join();
    tb.join();
    const uint64_t wall_ns = now_ns() - t0;

    const double wall_s = static_cast<double>(wall_ns) * 1e-9;
    const double prod_s = static_cast<double>(shared.producer_ns.load()) * 1e-9;
    std::printf("\nTotal runtime:        %.3f s (start to last reader joined)\n", wall_s);
    std::printf("Producer throughput:  %.2f M events/s (%.3f s for %llu events, %.1f ns/event incl. clock read)\n\n",
        static_cast<double>(kTotalEvents) / prod_s * 1e-6, prod_s,
        static_cast<unsigned long long>(kTotalEvents), prod_s * 1e9 / static_cast<double>(kTotalEvents));

    const bool ok_a = report_reader("A greedy", stats_a, /*must_have_dropped=*/false);
    const bool ok_b = report_reader("B lagging", stats_b, /*must_have_dropped=*/true);

    const bool pass = ok_a && ok_b;
    std::printf("\nVerification: %s\n", pass
        ? "PASS -- zero torn reads, zero corruptions, received + dropped == produced for both readers"
        : "FAIL");

    region.close();
    animus::sys::ipc::SharedMemoryRegion::unlink(name.c_str());
    return pass ? 0 : 1;
}
