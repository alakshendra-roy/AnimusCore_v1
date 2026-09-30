// Regression + stress harness for the seqlock guarding ShmRing<T>::
// push_overwrite() and SpmcRing<T>::broadcast() (include/animus/shm_ipc.hpp).
//
// What it proves:
//   1. Wire compatibility: try_push() and push_overwrite()'s free-slot branch
//      never touch write_seq, so the strict-backpressure hot path is
//      unchanged; only a reclaiming write takes the write section.
//   2. Deterministic detection: a write that is "in flight" on a slot (the
//      state a producer killed mid-write leaves behind) is never returned
//      as data, and never hangs a reader.
//   3. Under a producer hammering a tiny ring flat out, no reader -- the SPSC
//      consumer or any of N concurrent SPMC readers -- ever returns a frame
//      stitched from two writes. Frames are self-verifying (every word is a
//      function of the sequence number), so a torn frame cannot validate.
//   4. Losses are accounted exactly: SPMC delivered + overrun_count() ==
//      broadcasts, per reader; SPSC every undelivered record is covered by
//      dropped_count().
//   5. The harness has teeth: an UNVALIDATED reader against the same
//      producer does observe torn frames (reported, not asserted -- it is a
//      race, so a clean run is possible in principle). Build with
//      -DANIMUS_TEST_AGAINST_LEGACY_HEADER against a pre-seqlock copy of
//      shm_ipc.hpp to run only the API-level stress and watch it fail.
//
// Usage: test_shm_seqlock [--seconds N]   (stress duration per scenario, default 2)
// No test framework, per CLAUDE.md's zero-dependency rule: asserts-by-hand,
// a PASS/FAIL line per check, non-zero exit on any failure. Run it under
// ThreadSanitizer too (-fsanitize=thread): the slot copies are relaxed
// atomics precisely so this is race-free by the C++ memory model, not just
// "works on x86". The negative control is compiled out under TSan, since
// its whole point is an unsynchronized read.
#include "../include/animus/shm_ipc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
    #define ANIMUS_TEST_UNDER_TSAN 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define ANIMUS_TEST_UNDER_TSAN 1
    #endif
#endif

using namespace animus::sys::ipc;

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// ---- self-verifying 64-byte frame ------------------------------------------
constexpr uint64_t mix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
struct Frame {
    uint64_t seq;
    uint64_t word[7]; // each a function of seq: a frame spliced from two writes cannot match
};
static_assert(sizeof(Frame) == 64, "one frame == one cache line; keeps the tear window realistic");

Frame make_frame(uint64_t seq) noexcept {
    Frame f;
    f.seq = seq;
    for (uint64_t i = 0; i < 7; ++i) f.word[i] = mix64(seq * 7 + i);
    return f;
}
bool frame_intact(const Frame& f) noexcept {
    for (uint64_t i = 0; i < 7; ++i) {
        if (f.word[i] != mix64(f.seq * 7 + i)) return false;
    }
    return true;
}

using SpscRing = ShmRing<Frame>;
using BroadcastRing = SpmcRing<Frame>;

std::string unique_name(const char* tag) {
    return std::string("animus_seqlock_") + std::to_string(animus::sys::lifecycle::current_process_id()) + "_" + tag;
}

// Unlinks on scope exit, and first clears any stale segment a crashed prior
// run left behind (create() refuses to reuse one, deliberately).
struct Segment {
    std::string name;
    explicit Segment(const char* tag) : name(unique_name(tag)) { SharedMemoryRegion::unlink(name.c_str()); }
    ~Segment() { SharedMemoryRegion::unlink(name.c_str()); }
};

// Raw second mapping of a segment, to read/poke the header exactly as an
// external process would.
struct RawHeader {
    SharedMemoryRegion region;
    RingHeader* get() { return static_cast<RingHeader*>(region.data()); }
    explicit RawHeader(const std::string& name) { SharedMemoryRegion::open(name.c_str(), region); }
};
struct RawSpmcHeader {
    SharedMemoryRegion region;
    SpmcRingHeader* get() { return static_cast<SpmcRingHeader*>(region.data()); }
    explicit RawSpmcHeader(const std::string& name) { SharedMemoryRegion::open(name.c_str(), region); }
};

// ---- compile-time truth table for the validity predicate -------------------
#ifndef ANIMUS_TEST_AGAINST_LEGACY_HEADER
using animus::sys::ipc::seqlock::begin_value;
using animus::sys::ipc::seqlock::end_value;
using animus::sys::ipc::seqlock::lapped;
static_assert(begin_value(5) == 11 && end_value(5) == 12, "odd == in flight, even == complete");
static_assert(!lapped(0, 0, 8), "a fresh ring (counter 0) invalidates nothing");
static_assert(!lapped(end_value(7), 0, 8), "write 7 done: record 0 (recycled by write 8) is still intact");
static_assert(lapped(begin_value(8), 0, 8), "write 8 in flight: it targets record 0's slot");
static_assert(lapped(end_value(8), 0, 8), "write 8 done: record 0 is gone");
static_assert(!lapped(begin_value(8), 1, 8), "write 8 in flight does NOT invalidate record 1 (different slot)");
static_assert(lapped(begin_value(9), 1, 8), "write 9 in flight targets record 1's slot");
static_assert(lapped(end_value(100), 50, 8), "a long-lapped record is invalid whatever the counter's parity");
#endif

// =============================================================================
// Deterministic cases (new protocol only)
// =============================================================================
#ifndef ANIMUS_TEST_AGAINST_LEGACY_HEADER

void test_only_reclaiming_writes_take_the_write_section() {
    std::printf("test_only_reclaiming_writes_take_the_write_section\n");
    Segment seg("layout");
    auto ring = SpscRing::create(seg.name.c_str(), 8);
    check(ring != nullptr, "ring created");
    if (!ring) return;
    RawHeader raw(seg.name);
    check(raw.region.valid(), "second mapping (an 'external reader' view) opened");
    if (!raw.region.valid()) return;
    RingHeader* h = raw.get();

    for (uint64_t i = 0; i < 6; ++i) ring->try_push(make_frame(i));
    check(h->write_seq.load() == 0, "try_push never touches write_seq (strict-backpressure path unchanged)");
    for (uint64_t i = 6; i < 8; ++i) ring->push_overwrite(make_frame(i)); // fills the last two free slots
    check(h->write_seq.load() == 0, "push_overwrite into a free slot takes no write section");
    check(ring->dropped_count() == 0, "nothing dropped while slots were free");

    ring->push_overwrite(make_frame(8)); // ring full: first reclaim
    check(h->write_seq.load() == end_value(8), "first reclaim leaves write_seq == end_value(8) (even: complete)");
    check((h->write_seq.load() & 1u) == 0, "counter is even when no write is in flight");
    check(ring->dropped_count() == 1, "reclaim counted exactly once in dropped_count");
    check(h->tail.load() == 1, "reclaim advanced the raw tail itself (test_telemetry.cpp relies on this)");
    check(h->head.load() == 9, "head advanced");
}

void test_fifo_and_accounting_single_thread() {
    std::printf("test_fifo_and_accounting_single_thread\n");
    Segment seg("fifo");
    auto ring = SpscRing::create(seg.name.c_str(), 8);
    if (!ring) { check(false, "ring created"); return; }

    Frame sentinel = make_frame(0xDEADBEEF);
    Frame out = sentinel;
    check(!ring->try_pop(out), "try_pop on an empty ring returns false");
    check(std::memcmp(&out, &sentinel, sizeof(Frame)) == 0, "...and leaves `out` untouched");

    for (uint64_t i = 0; i < 20; ++i) ring->push_overwrite(make_frame(i)); // no consumer: 12 reclaims
    check(ring->dropped_count() == 12, "20 pushes into capacity 8 with no consumer: dropped_count == 12");
    bool in_order = true, all_intact = true;
    uint64_t expect = 12, popped = 0;
    Frame f{};
    while (ring->try_pop(f)) {
        in_order = in_order && f.seq == expect++;
        all_intact = all_intact && frame_intact(f);
        ++popped;
    }
    check(popped == 8, "exactly the newest 8 records survive");
    check(in_order && expect == 20, "...in FIFO order (12..19)");
    check(all_intact, "...every one intact");
    check(ring->dropped_count() == 12, "popping does not change dropped_count");

    // Interleaved pop/push around the full boundary: a pop frees a slot, so
    // the next push_overwrite must NOT reclaim (and must not count a drop).
    for (uint64_t i = 20; i < 28; ++i) ring->push_overwrite(make_frame(i)); // refill to exactly full
    const uint64_t dropped_before = ring->dropped_count();
    check(ring->try_pop(f) && f.seq == 20, "pop the oldest");
    ring->push_overwrite(make_frame(28));
    check(ring->dropped_count() == dropped_before, "push after a pop used the freed slot: no drop counted");
}

void test_in_flight_write_is_never_returned_spsc() {
    std::printf("test_in_flight_write_is_never_returned_spsc (producer killed mid-write)\n");
    Segment seg("inflight");
    auto ring = SpscRing::create(seg.name.c_str(), 8);
    if (!ring) { check(false, "ring created"); return; }
    for (uint64_t i = 0; i < 8; ++i) ring->try_push(make_frame(i)); // full, indices 0..7
    RawHeader raw(seg.name);
    if (!raw.region.valid()) { check(false, "raw mapping"); return; }
    // Freeze the producer exactly where kill -9 could: write of index 8
    // announced (odd counter), tail not yet reclaimed, head not published.
    // A real producer sets overwrite_active before its first odd store, so
    // the forged state must too (it is what routes try_pop to the seqlock).
    raw.get()->overwrite_active.store(1);
    raw.get()->write_seq.store(begin_value(8));

    Frame f{};
    check(ring->try_pop(f), "try_pop returns (does not hang on an odd counter)");
    check(f.seq == 1 && frame_intact(f), "record 0 -- the slot write 8 is overwriting -- is skipped; record 1 is returned intact");
    uint64_t next = 2;
    bool in_order = true;
    while (ring->try_pop(f)) in_order = in_order && f.seq == next++ && frame_intact(f);
    check(in_order && next == 8, "the remaining records 2..7 drain intact and in order");
    check(!ring->try_pop(f), "then empty (head was never published past 8)");
}

void test_in_flight_write_is_never_returned_spmc() {
    std::printf("test_in_flight_write_is_never_returned_spmc\n");
    Segment seg("inflight_spmc");
    auto producer = BroadcastRing::create(seg.name.c_str(), 8);
    if (!producer) { check(false, "ring created"); return; }
    for (uint64_t i = 0; i < 8; ++i) producer->broadcast(make_frame(i));
    RawSpmcHeader raw(seg.name);
    auto reader = BroadcastRing::open(seg.name.c_str());
    if (!raw.region.valid() || !reader) { check(false, "raw mapping / reader open"); return; }
    raw.get()->write_seq.store(begin_value(8)); // write of index 8 announced, never finished

    Frame buf[16];
    const size_t n = reader->poll(buf, 16);
    bool ok = n == 7;
    for (size_t i = 0; ok && i < n; ++i) ok = buf[i].seq == i + 1 && frame_intact(buf[i]);
    check(n == 7, "poll returns the 7 records not under the in-flight write");
    check(ok, "...records 1..7, all intact, in order");
    check(reader->overrun_count() == 1 && reader->last_poll_overran(), "the skipped record is a counted overrun, not silent");
    check(n + reader->overrun_count() + (reader->head() - reader->local_tail()) == reader->head(), "conservation holds");
}

#endif // !ANIMUS_TEST_AGAINST_LEGACY_HEADER

// =============================================================================
// Stress
// =============================================================================
struct SpscResult {
    uint64_t delivered = 0, torn = 0, reordered = 0;
    bool drain_terminated = false; // consumer eventually saw "empty" once the producer stopped
};

// Saturating producer, one consumer, tiny ring => nearly every push is a
// reclaim of the exact slot the consumer is about to read.
void stress_spsc(size_t capacity, std::chrono::milliseconds budget) {
    std::printf("stress_spsc capacity=%zu, %lld ms\n", capacity, static_cast<long long>(budget.count()));
    Segment seg("spsc");
    auto ring = SpscRing::create(seg.name.c_str(), capacity);
    if (!ring) { check(false, "ring created"); return; }

    std::atomic<bool> go{ false }, producer_done{ false };
    uint64_t total = 0;
    SpscResult r;

    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire)) {}
        const auto deadline = std::chrono::steady_clock::now() + budget;
        uint64_t i = 0;
        do {
            for (int k = 0; k < 1024; ++k) ring->push_overwrite(make_frame(i++));
        } while (std::chrono::steady_clock::now() < deadline);
        total = i;
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        while (!go.load(std::memory_order_acquire)) {}
        Frame f{};
        bool have_last = false;
        uint64_t last = 0;
        auto consume = [&](const Frame& fr) {
            if (!frame_intact(fr)) { ++r.torn; return; }
            if (have_last && fr.seq <= last) ++r.reordered;
            last = fr.seq;
            have_last = true;
            ++r.delivered;
        };
        while (!producer_done.load(std::memory_order_acquire)) {
            if (ring->try_pop(f)) consume(f);
        }
        // Producer has stopped: a healthy ring holds at most `capacity`
        // records, so a false return must arrive within that many pops. The
        // bound is what lets this report a wedged ring (cursors corrupted so
        // try_pop never sees "empty") as a FAIL instead of hanging the run.
        for (size_t guard = 0; guard < capacity * 4 + 64; ++guard) {
            if (!ring->try_pop(f)) { r.drain_terminated = true; break; }
            consume(f);
        }
    });
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    const uint64_t dropped = ring->dropped_count();
    auto view = RawSchemaView::open(seg.name.c_str());
    const bool cursors_sane = view && view->tail() <= view->head() && view->head() - view->tail() <= capacity;
    if (view) {
        std::printf("    quiesced cursors: head=%llu tail=%llu\n",
            static_cast<unsigned long long>(view->head()), static_cast<unsigned long long>(view->tail()));
    }
    const uint64_t lost = total - r.delivered;
    std::printf("    pushed=%llu delivered=%llu lost=%llu dropped_count=%llu torn=%llu reordered=%llu\n",
        static_cast<unsigned long long>(total), static_cast<unsigned long long>(r.delivered),
        static_cast<unsigned long long>(lost), static_cast<unsigned long long>(dropped),
        static_cast<unsigned long long>(r.torn), static_cast<unsigned long long>(r.reordered));
    check(r.torn == 0, "zero torn frames returned by try_pop");
    check(r.reordered == 0, "zero duplicate / out-of-order deliveries");
    check(r.delivered > 0, "the consumer made progress (was not starved)");
    check(r.drain_terminated, "once the producer stops, try_pop reports empty (ring not wedged)");
    check(cursors_sane, "quiesced cursors are sane: tail <= head and head - tail <= capacity");
    check(dropped >= lost, "every undelivered record is covered by dropped_count() (it may over-count; it never under-counts)");
}

struct SpmcResult {
    uint64_t delivered = 0, torn = 0, reordered = 0, overruns = 0;
    bool conserved = false;
};

void stress_spmc(size_t capacity, int readers, std::chrono::milliseconds budget) {
    std::printf("stress_spmc capacity=%zu readers=%d (one deliberately slow), %lld ms\n",
        capacity, readers, static_cast<long long>(budget.count()));
    Segment seg("spmc");
    auto producer = BroadcastRing::create(seg.name.c_str(), capacity);
    if (!producer) { check(false, "ring created"); return; }

    std::vector<std::unique_ptr<BroadcastRing>> views;
    for (int i = 0; i < readers; ++i) {
        views.push_back(BroadcastRing::open(seg.name.c_str()));
        if (!views.back()) { check(false, "reader open"); return; }
    }

    std::atomic<bool> go{ false }, producer_done{ false };
    uint64_t total = 0;
    std::vector<SpmcResult> results(static_cast<size_t>(readers));

    std::thread prod([&] {
        while (!go.load(std::memory_order_acquire)) {}
        const auto deadline = std::chrono::steady_clock::now() + budget;
        uint64_t i = 0;
        do {
            for (int k = 0; k < 1024; ++k) producer->broadcast(make_frame(i++));
        } while (std::chrono::steady_clock::now() < deadline);
        total = i;
        producer_done.store(true, std::memory_order_release);
    });
    std::vector<std::thread> threads;
    for (int idx = 0; idx < readers; ++idx) {
        threads.emplace_back([&, idx] {
            BroadcastRing& ring = *views[static_cast<size_t>(idx)];
            SpmcResult& r = results[static_cast<size_t>(idx)];
            const bool slow = idx == readers - 1;
            while (!go.load(std::memory_order_acquire)) {}
            Frame buf[64];
            bool have_last = false;
            uint64_t last = 0;
            auto drain = [&](size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    if (!frame_intact(buf[i])) { ++r.torn; continue; }
                    if (have_last && buf[i].seq <= last) ++r.reordered;
                    last = buf[i].seq;
                    have_last = true;
                    ++r.delivered;
                }
            };
            while (!producer_done.load(std::memory_order_acquire)) {
                drain(ring.poll(buf, 64));
                if (slow) std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            for (size_t n; (n = ring.poll(buf, 64)) > 0;) drain(n); // producer stopped: drain to head
            r.overruns = ring.overrun_count();
            r.conserved = r.delivered + r.torn + r.overruns == ring.head() && ring.local_tail() == ring.head();
        });
    }
    go.store(true, std::memory_order_release);
    prod.join();
    for (auto& t : threads) t.join();

    for (int idx = 0; idx < readers; ++idx) {
        const SpmcResult& r = results[static_cast<size_t>(idx)];
        std::printf("    reader %d%s: delivered=%llu overruns=%llu torn=%llu reordered=%llu (of %llu broadcast)\n",
            idx, idx == readers - 1 ? " (slow)" : "", static_cast<unsigned long long>(r.delivered),
            static_cast<unsigned long long>(r.overruns), static_cast<unsigned long long>(r.torn),
            static_cast<unsigned long long>(r.reordered), static_cast<unsigned long long>(total));
    }
    uint64_t torn = 0, reordered = 0, min_delivered = ~0ull;
    bool all_conserved = true;
    for (const SpmcResult& r : results) {
        torn += r.torn;
        reordered += r.reordered;
        all_conserved = all_conserved && r.conserved && r.delivered + r.overruns == total;
        if (r.delivered < min_delivered) min_delivered = r.delivered;
    }
    check(torn == 0, "zero torn frames across all concurrent readers");
    check(reordered == 0, "zero duplicate / out-of-order deliveries on every reader");
    check(all_conserved, "per reader: delivered + overrun_count() == records broadcast (exact)");
    check(min_delivered > 0, "every reader, including the slow one, made progress");
}

#if !defined(ANIMUS_TEST_UNDER_TSAN)
// Negative control: what the pre-fix consumer did -- copy the oldest slot
// with no validation -- against the same saturating producer. Reports how
// many torn frames that observes; deliberately not asserted (see header).
void negative_control_unvalidated_reader(std::chrono::milliseconds budget) {
    std::printf("negative_control_unvalidated_reader (the pre-fix read, %lld ms)\n", static_cast<long long>(budget.count()));
    Segment seg("negctl");
    auto ring = SpscRing::create(seg.name.c_str(), 8);
    if (!ring) { check(false, "ring created"); return; }
    auto view = RawSchemaView::open(seg.name.c_str());
    if (!view) { check(false, "RawSchemaView open"); return; }

    std::atomic<bool> go{ false }, stop{ false };
    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire)) {}
        uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (int k = 0; k < 256; ++k) ring->push_overwrite(make_frame(i++));
        }
    });
    uint64_t reads = 0, torn = 0;
    go.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const uint64_t mask = view->capacity() - 1;
    const auto* base = static_cast<const unsigned char*>(view->slots_base());
    while (std::chrono::steady_clock::now() < deadline) {
        for (int k = 0; k < 4096; ++k) {
            const uint64_t t = view->tail();
            if (t == view->head()) continue;
            Frame f;
            std::memcpy(&f, base + (t & mask) * sizeof(Frame), sizeof(Frame)); // no validation
            ++reads;
            if (!frame_intact(f)) ++torn;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    std::printf("    unvalidated reads=%llu, torn frames observed=%llu\n",
        static_cast<unsigned long long>(reads), static_cast<unsigned long long>(torn));
    std::printf("    %s\n", torn > 0 ? "=> the harness DOES detect tearing when the reader is unprotected"
                                      : "=> no tear observed this run (inconclusive; it is a race -- rerun or lengthen --seconds)");
}
#endif

} // namespace

int main(int argc, char** argv) {
    int seconds = 2;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0) seconds = std::atoi(argv[i + 1]);
    }
    if (seconds < 1) seconds = 1;
    const std::chrono::milliseconds budget(seconds * 1000);

    std::printf("Animus seqlock regression + stress (shm_ipc.hpp)\n");
    std::printf("cache line %d bytes, sizeof(RingHeader)=%zu, sizeof(SpmcRingHeader)=%zu\n\n",
        ANIMUS_CACHE_LINE_SIZE, sizeof(RingHeader), sizeof(SpmcRingHeader));

#ifndef ANIMUS_TEST_AGAINST_LEGACY_HEADER
    test_only_reclaiming_writes_take_the_write_section();
    test_fifo_and_accounting_single_thread();
    test_in_flight_write_is_never_returned_spsc();
    test_in_flight_write_is_never_returned_spmc();
#endif
    stress_spsc(8, budget);
    stress_spsc(4096, budget);
    stress_spmc(64, 4, budget);
    stress_spmc(1u << 16, 4, budget);
#if !defined(ANIMUS_TEST_UNDER_TSAN)
    negative_control_unvalidated_reader(budget);
#else
    std::printf("negative_control_unvalidated_reader: skipped under ThreadSanitizer (it is an intentional data race)\n");
#endif

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
