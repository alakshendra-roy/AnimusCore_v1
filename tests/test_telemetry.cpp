// Milestone 3 regression test: verifies include/animus/telemetry.hpp's
// AnimusGetMetrics() reads both ring kinds' headers correctly and never
// attaches as a consumer (no cursor registration, no side effects on the
// segment). Single-process, unlike tests/test_dynamic_schema.cpp and
// tests/test_spmc.cpp -- AnimusGetMetrics() is explicitly a "sample
// without attaching" read, so there is nothing here that needs a second
// OS process to exercise: this test creates a ring, pushes/broadcasts
// into it directly, and samples it from the SAME process, which reads
// through the identical SharedMemoryRegion::open() code path a genuinely
// separate process would (there is no in-process fast path anywhere in
// this file).
//
// No C++ test framework here, consistent with tests/test_licensing.cpp
// (CLAUDE.md's zero-dependency rule) -- asserts-by-hand + a PASS/FAIL
// summary + a non-zero exit code on any failure.
#include "../include/animus/telemetry.hpp"
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/execution_event.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using animus::AnimusGetMetrics;
using animus::TelemetrySnapshot;
using animus::ExecutionEvent;
using SpscRing = animus::sys::ipc::ShmRing<ExecutionEvent>;
using SpmcRing = animus::sys::ipc::SpmcRing<ExecutionEvent>;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  [PASS] %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

ExecutionEvent make_record(uint64_t i) noexcept {
    ExecutionEvent ev{};
    ev.sequence = i;
    ev.dispatch_ts_raw = i;
    ev.price_ticks = static_cast<int64_t>(i);
    ev.quantity = 1;
    ev.instrument_id = 1;
    ev.flags = 0;
    return ev;
}

void test_nonexistent_segment() {
    std::printf("test_nonexistent_segment\n");
    TelemetrySnapshot snap;
    const bool ok = AnimusGetMetrics("animus_telemetry_test_does_not_exist", &snap);
    check(!ok, "AnimusGetMetrics() returns false for a nonexistent segment");
    check(!snap.valid, "snapshot is left invalid on failure");
}

void test_null_out_pointer_is_safe() {
    std::printf("test_null_out_pointer_is_safe\n");
    check(!AnimusGetMetrics("whatever", nullptr), "AnimusGetMetrics() returns false (not a crash) for a null out pointer");
}

void test_spsc_ring() {
    std::printf("test_spsc_ring (ShmRing<ExecutionEvent>)\n");
    const char* name = "animus_telemetry_test_spsc";
    auto ring = SpscRing::create(name, /*requested_capacity=*/64);
    check(ring != nullptr, "ShmRing::create succeeded");
    if (!ring) return;
    ring->mark_producer_attached();

    // 40 successful pushes, then force capacity() (64) more through
    // push_overwrite() with the ring already full so some are reclaimed --
    // gives dropped_count a real nonzero value to verify against.
    for (uint64_t i = 0; i < 40; ++i) {
        check(ring->try_push(make_record(i)), "try_push succeeds while below capacity");
    }
    for (uint64_t i = 40; i < 40 + 64; ++i) {
        ring->push_overwrite(make_record(i));
    }
    const uint64_t expected_head = 40 + 64;
    const uint64_t expected_dropped = ring->dropped_count(); // ground truth from the ring's own accessor
    // push_overwrite() advances tail itself when reclaiming a full ring's
    // oldest slot (shm_ipc.hpp's own doc comment on that method) -- every
    // reclaim is exactly one dropped_count increment, so the two are equal
    // here (no separate consumer ever popped anything in this test).
    const uint64_t expected_tail = expected_dropped;

    TelemetrySnapshot snap;
    const bool ok = AnimusGetMetrics(name, &snap);
    check(ok, "AnimusGetMetrics() succeeds for a live SPSC segment");
    check(snap.valid, "snapshot marked valid");
    check(snap.ring_kind == animus::sys::ipc::RingKind::Spsc, "ring_kind reported as Spsc");
    check(snap.capacity == ring->capacity(), "capacity matches");
    check(snap.payload_size == sizeof(ExecutionEvent), "payload_size == sizeof(ExecutionEvent)");
    check(std::strcmp(snap.wire_format, animus::kExecutionEventWireFormat) == 0, "wire_format matches ExecutionEvent's registered format");
    check(snap.current_write_head == expected_head, "current_write_head matches actual push count");
    check(snap.total_pushed_events == expected_head, "total_pushed_events == current_write_head (same counter)");
    check(snap.has_dropped_count, "has_dropped_count is true for an SPSC ring");
    check(snap.total_overruns == expected_dropped, "total_overruns matches ShmRing<T>::dropped_count()");
    check(snap.dropped_events == snap.total_overruns, "dropped_events aliases total_overruns");
    check(snap.has_consumer_lag, "has_consumer_lag is true for an SPSC ring");
    check(snap.consumer_tail == expected_tail, "consumer_tail matches (advanced by push_overwrite's own reclaims, not by a consumer)");
    check(snap.consumer_lag == expected_head - expected_tail, "consumer_lag == head - tail");
    check(snap.producer_pid != 0, "producer_pid was published by mark_producer_attached()");
    check(snap.producer_alive, "producer_alive is true (this same process is still running)");
    check(snap.sampled_at_epoch_ns != 0, "sampled_at_epoch_ns was stamped");

    // The defining property this milestone asks for: sampling must not
    // attach as a consumer. Confirm the ring's own tail/capacity are
    // completely unaffected by having been sampled.
    check(ring->capacity() == 64, "ring capacity unaffected by AnimusGetMetrics()");
    ExecutionEvent popped{};
    uint64_t real_count = 0;
    while (ring->try_pop(popped)) ++real_count;
    check(real_count == expected_head - expected_dropped,
        "every record the ring actually still holds is still poppable after sampling (nothing consumed by AnimusGetMetrics)");

    SpscRing::unlink(name);
}

void test_spmc_ring() {
    std::printf("test_spmc_ring (SpmcRing<ExecutionEvent>)\n");
    const char* name = "animus_telemetry_test_spmc";
    // Capacity comfortably larger than the 100 broadcasts below, so the
    // "real consumer" opened after sampling (below) sees the full backlog
    // rather than legitimately overrunning before it ever gets to read --
    // that overrun behavior is already covered by tests/test_spmc.cpp;
    // this test's only job is confirming AnimusGetMetrics() didn't
    // register as a consumer.
    auto ring = SpmcRing::create(name, /*requested_capacity=*/256);
    check(ring != nullptr, "SpmcRing::create succeeded");
    if (!ring) return;
    ring->mark_producer_attached();

    for (uint64_t i = 0; i < 100; ++i) {
        ring->broadcast(make_record(i));
    }

    TelemetrySnapshot snap;
    const bool ok = AnimusGetMetrics(name, &snap);
    check(ok, "AnimusGetMetrics() succeeds for a live SPMC segment");
    check(snap.valid, "snapshot marked valid");
    check(snap.ring_kind == animus::sys::ipc::RingKind::SpmcBroadcast, "ring_kind reported as SpmcBroadcast");
    check(snap.capacity == ring->capacity(), "capacity matches");
    check(snap.current_write_head == 100, "current_write_head matches actual broadcast count");
    check(snap.total_pushed_events == 100, "total_pushed_events == current_write_head");
    check(!snap.has_dropped_count, "has_dropped_count is false for an SPMC ring (no shared drop counter)");
    check(snap.total_overruns == 0, "total_overruns stays 0 for an SPMC ring");
    check(!snap.has_consumer_lag, "has_consumer_lag is false for an SPMC ring (no shared consumer cursor)");
    check(snap.consumer_lag == 0, "consumer_lag stays 0 for an SPMC ring");
    check(snap.producer_pid != 0, "producer_pid was published by mark_producer_attached()");
    check(snap.producer_alive, "producer_alive is true (this same process is still running)");

    // Sampling must not register as a consumer -- open a REAL consumer
    // afterward and confirm it still starts from local_tail_==0 seeing
    // the full backlog, exactly as if no one had sampled in between.
    auto consumer = SpmcRing::open(name);
    check(consumer != nullptr, "a real consumer can still open() normally after being sampled");
    if (consumer) {
        std::vector<ExecutionEvent> batch(200);
        const size_t n = consumer->poll(batch.data(), batch.size());
        check(n == 100, "the real consumer still sees the full backlog -- AnimusGetMetrics() consumed nothing");
    }

    SpmcRing::unlink(name);
}

} // namespace

int main() {
    std::printf("=== test_telemetry ===\n");
    test_nonexistent_segment();
    test_null_out_pointer_is_safe();
    test_spsc_ring();
    test_spmc_ring();

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
