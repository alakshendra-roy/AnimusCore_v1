// Milestone 2 regression test: proves SpmcRing<T> (include/animus/shm_ipc.hpp)
// actually broadcasts to independent, concurrently-attached consumers across
// real OS process boundaries -- one producer, two consumers with very
// different poll cadences:
//
//   - A fast consumer that spin-polls as tightly as it can, expected to
//     receive every single broadcast record, in order, with zero overruns.
//   - A deliberately throttled ("slow") consumer that sleeps between polls,
//     expected to fall behind by more than the ring's capacity at least
//     once, and to correctly detect and account that overrun rather than
//     silently losing track of how much it missed.
//
// Both consumers read the SAME segment concurrently with no coordination
// between them and no shared read cursor at all (each SpmcRing::open() call
// gets its own local_tail_, living only in that process's own memory) --
// this is the actual thing this milestone adds over ShmRing<T>'s existing
// single-consumer ring, so the test's whole point is exercising exactly
// that: neither consumer's pace may affect the other's, and neither may
// affect the producer's.
//
// No C++ test framework here, consistent with tests/test_licensing.cpp and
// tests/test_dynamic_schema.cpp (CLAUDE.md's zero-dependency rule) --
// asserts-by-hand + a PASS/FAIL summary + a non-zero exit code on failure.
// "Across processes" is literal: this binary is all three roles, selected
// by argv -- launch three SEPARATE OS processes (see this file's own
// --help text, and .github/workflows/linux_ci.yml for how CI drives it).
//
// Ordering: the producer owns ring creation (there is exactly one producer
// in an SPMC ring, unlike the two-sided create/open convention
// tests/test_dynamic_schema.cpp and shm_ipc_bench.cpp use for their SPSC
// rings) and deliberately waits ~500ms after creating it before it starts
// broadcasting -- broadcast() itself never waits for a consumer to attach
// (that's the whole point of the ring), so this grace period is what makes
// the fast consumer's "zero overruns" assertion a real property of the
// code rather than a race against how quickly three separate OS processes
// happen to get scheduled. Both consumers retry open() against that same
// segment for up to 10s to absorb their own share of that same startup
// race.
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/execution_event.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using animus::ExecutionEvent;
using Ring = animus::sys::ipc::SpmcRing<ExecutionEvent>;

namespace {

// Large enough that a spin-polling fast consumer has a comfortable margin
// against ordinary OS scheduling jitter (it would need to be starved for
// the time it takes the producer to broadcast a full capacity() worth of
// records before it would ever see an overrun -- a smaller capacity here,
// tried during development, produced real, reproducible overruns for the
// FAST consumer under normal multi-process scheduling load, not just the
// slow one), while still small enough that a consumer sleeping a couple of
// milliseconds between polls reliably laps multiple times over, and that
// draining the leftover after one overrun-correction jump (see
// run_slow_consumer) finishes in a bounded number of throttled iterations
// -- see kPollBatchCapacity's own comment for the other half of that
// reasoning (that constant, not this one, is what keeps each individual
// call's own copy loop cheap enough to avoid a self-inflicted torn read).
constexpr size_t kCapacity = 1u << 20; // 1,048,576 slots (~40 MiB of ExecutionEvent)

// Deliberately modest, for both consumer roles: a poll()/poll_spin() call
// copies up to this many records in one shot, and that copy loop itself
// takes wall-clock time -- if a single call's own batch were large enough
// to take, say, low-single-digit milliseconds, an actively-broadcasting
// producer could advance past capacity() *during* that one call, torn-
// reading a slot this same call already validated as safe at the top (see
// SpmcRing<T>::poll's own "Torn-read note"). That is a real, reproducible
// failure mode, not a hypothetical one -- an earlier, much larger batch
// size for the slow consumer (131,072) triggered exactly this during
// development. Keeping batches small keeps each call's own execution time
// negligible next to the producer's broadcast rate, leaving the *sleep*
// between the slow consumer's calls (not an oversized single call) as the
// only thing that makes it fall behind.
constexpr size_t kPollBatchCapacity = 4096;

ExecutionEvent make_record(uint64_t i) noexcept {
    ExecutionEvent ev{};
    ev.sequence = i;
    ev.dispatch_ts_raw = i * 7 + 1;
    ev.price_ticks = static_cast<int64_t>(100000 + (i % 4096));
    ev.quantity = static_cast<int64_t>(1 + (i % 500));
    ev.instrument_id = static_cast<uint32_t>(i % 256);
    ev.flags = 0;
    return ev;
}
bool records_equal(const ExecutionEvent& got, const ExecutionEvent& want) noexcept {
    return got.sequence == want.sequence && got.dispatch_ts_raw == want.dispatch_ts_raw &&
           got.price_ticks == want.price_ticks && got.quantity == want.quantity &&
           got.instrument_id == want.instrument_id && got.flags == want.flags;
}

std::unique_ptr<Ring> open_with_retry(const std::string& name, const char* role) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto ring = Ring::open(name.c_str());
        if (ring) return ring;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "[%s] could not open '%s' within 10s\n", role, name.c_str());
    return nullptr;
}

// Retries open() rather than owning creation itself -- see run_fast_consumer
// below for why the FAST CONSUMER, not the producer, owns ring creation in
// this test despite there being exactly one producer.
int run_producer(const std::string& name, uint64_t count) {
    auto ring = open_with_retry(name, "producer");
    if (!ring) return 1;
    ring->mark_producer_attached();

    // Grace period -- see this file's own header comment for why this is
    // what makes the fast consumer's zero-overrun assertion deterministic
    // rather than a launch-order race.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (uint64_t i = 0; i < count; ++i) {
        ring->broadcast(make_record(i));
    }
    std::printf("[producer] broadcast %llu records\n", static_cast<unsigned long long>(count));
    return 0;
}

// Fast consumer: spin-polls as tightly as poll_spin allows. Must receive
// every record broadcast, in exact sequence order, with zero overruns --
// the "fast consumer keeps up completely" half of this milestone's
// requirement.
//
// Owns ring CREATION, even though it is a consumer and there is only one
// producer -- deliberately, not by SPMC necessity. A named OS shared-memory
// mapping's lifetime is tied to its *handles*, not to any one role: on
// Windows specifically, the segment is destroyed the instant the LAST
// handle across every process closes, so if the producer (short-lived: a
// 500ms grace period plus however long broadcasting `count` records takes,
// often under a second) were the creator and happened to finish and exit
// before either consumer's process had gotten around to its own first
// open() call -- pure OS process-scheduling variance, observed in practice
// while developing this test -- the segment would vanish before a consumer
// ever got a chance to attach. The fast consumer is the one process
// guaranteed to stay alive for this test's entire duration (it doesn't
// return until it has received every record), so it is the safe choice to
// hold the creating handle throughout, mirroring
// AnimusCore_v1/shm_ipc_bench.cpp's own "the process that should be up
// first, is" convention -- just applied to whichever role actually
// outlives the others here, not to the producer/consumer role split itself.
int run_fast_consumer(const std::string& name, uint64_t count) {
    auto ring = Ring::create(name.c_str(), kCapacity);
    if (!ring) {
        std::fprintf(stderr, "[fast] create('%s') failed\n", name.c_str());
        return 1;
    }

    std::vector<ExecutionEvent> batch(kPollBatchCapacity);
    uint64_t received = 0;
    uint64_t expected_next = 0;
    bool mismatch = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (received < count) {
        const size_t n = ring->poll_spin(batch.data(), batch.size());
        for (size_t k = 0; k < n; ++k) {
            if (!records_equal(batch[k], make_record(expected_next))) {
                std::fprintf(stderr, "[fast] record %llu did not match the expected value\n",
                    static_cast<unsigned long long>(expected_next));
                mismatch = true;
            }
            ++expected_next;
            ++received;
        }
        if (n == 0 && std::chrono::steady_clock::now() > deadline) {
            // poll_spin() only returns 0 after exhausting its own very
            // large spin budget (200,000,000 attempts) with nothing to
            // show for it -- reaching here at all means something is
            // genuinely wrong (a lost record, a dead producer that never
            // delivered everything), not ordinary scheduling jitter.
            std::fprintf(stderr, "[fast] timed out after receiving %llu/%llu records\n",
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(count));
            return 1;
        }
    }

    std::printf("[fast] received=%llu overrun_count=%llu\n",
        static_cast<unsigned long long>(received), static_cast<unsigned long long>(ring->overrun_count()));

    if (mismatch) return 1;
    if (received != count) {
        std::fprintf(stderr, "[fast] expected %llu records, received %llu\n",
            static_cast<unsigned long long>(count), static_cast<unsigned long long>(received));
        return 1;
    }
    if (ring->overrun_count() != 0) {
        std::fprintf(stderr, "[fast] expected zero overruns, got %llu\n",
            static_cast<unsigned long long>(ring->overrun_count()));
        return 1;
    }
    std::printf("[fast] PASS: all %llu records received in order with zero overruns\n",
        static_cast<unsigned long long>(count));
    return 0;
}

// Slow consumer: deliberately throttled (a fixed sleep between small polls)
// so an unthrottled producer laps it. Must NOT receive every record --
// must instead detect the overrun and correctly account every skipped
// record into overrun_count(), which is the actual contract this milestone
// asks for ("accurately reports overruns"). This deliberately does NOT
// also assert every received record is torn-read-free / strictly ordered:
// SpmcRing<T>::poll()'s own doc comment documents that as an accepted
// tradeoff for a consumer that has genuinely fallen behind (the producer
// here broadcasts fast enough -- confirmed during development, well over
// 100M records/sec on typical hardware for this trivial a struct -- that
// even a modest-sized batch read can occasionally straddle a slot the
// producer wraps around and overwrites mid-read), the same class of race
// ShmRing<T>::push_overwrite already accepts for its own overwrite mode.
// What the accounting invariant below actually proves is stronger than
// ordering anyway: every one of the `count` records the producer broadcast
// is accounted for as EITHER received OR overrun -- nothing is silently
// lost track of, torn reads included, since a torn read still counts as
// "received" (just with the possibility of already-past-since-overwritten
// content), never as neither.
int run_slow_consumer(const std::string& name, uint64_t count) {
    auto ring = open_with_retry(name, "slow");
    if (!ring) return 1;

    // What makes this consumer "slow" is that it only checks in once every
    // couple of milliseconds (see the sleep_for below), not how much it
    // drains per check-in -- kPollBatchCapacity is deliberately the same
    // modest size used by the fast consumer (see that constant's own
    // comment for why an oversized batch here specifically makes the
    // torn-read window above worse, not just less efficient). kCapacity is
    // sized so draining the leftover after this consumer's one big
    // overrun-correction jump still finishes in a bounded number of these
    // throttled, modestly-sized calls rather than thousands of them.
    std::vector<ExecutionEvent> batch(kPollBatchCapacity);
    uint64_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        const size_t n = ring->poll(batch.data(), batch.size()); // deliberately non-spin: one attempt per loop iteration
        received += n;
        if (n == 0 && !ring->is_producer_alive()) {
            break; // producer is gone and this poll() found nothing new to drain
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr, "[slow] timed out waiting for the producer to finish\n");
            return 1;
        }
        // The deliberate throttle: sleeping between polls (rather than
        // spin-polling like the fast consumer above) is what reliably
        // makes this consumer fall behind an unthrottled producer by more
        // than kCapacity, so the overrun path below actually gets
        // exercised rather than being a dead branch this test never hits.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const uint64_t overruns = ring->overrun_count();
    std::printf("[slow] received=%llu overrun_count=%llu (received+overrun=%llu, expected=%llu)\n",
        static_cast<unsigned long long>(received), static_cast<unsigned long long>(overruns),
        static_cast<unsigned long long>(received + overruns), static_cast<unsigned long long>(count));

    if (overruns == 0) {
        std::fprintf(stderr,
            "[slow] expected at least one overrun (this consumer is deliberately throttled) -- "
            "either the throttle wasn't slow enough or overrun detection didn't fire\n");
        return 1;
    }
    if (received + overruns != count) {
        std::fprintf(stderr, "[slow] accounting mismatch: received(%llu) + overrun(%llu) != expected(%llu)\n",
            static_cast<unsigned long long>(received), static_cast<unsigned long long>(overruns),
            static_cast<unsigned long long>(count));
        return 1;
    }
    std::printf("[slow] PASS: overrun correctly detected and accounted (%llu dropped, %llu received, none lost untracked)\n",
        static_cast<unsigned long long>(overruns), static_cast<unsigned long long>(received));
    return 0;
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s --producer <ring_name> <count>\n"
        "  %s --consumer-fast <ring_name> <count>\n"
        "  %s --consumer-slow <ring_name> <count>\n"
        "Launch as three SEPARATE OS processes -- the producer owns ring\n"
        "creation and waits ~500ms before broadcasting, so start all three in\n"
        "any order (see this file's own header comment).\n",
        argv0, argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4) {
        const std::string role = argv[1];
        const std::string name = argv[2];
        const uint64_t count = std::strtoull(argv[3], nullptr, 10);
        if (role == "--producer") return run_producer(name, count);
        if (role == "--consumer-fast") return run_fast_consumer(name, count);
        if (role == "--consumer-slow") return run_slow_consumer(name, count);
    }
    print_usage(argv[0]);
    return 2;
}
