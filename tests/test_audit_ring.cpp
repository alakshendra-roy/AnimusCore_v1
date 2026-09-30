// Regression test for the bounded audit trail in AnimusCore_v1/animus_security.hpp
// (BoundedAuditLog), which replaced an unbounded std::deque<AuditEvent>.
//
// Proves:
//   1. Zero heap allocation: a replaced global operator new counts every
//      allocation; appending and draining millions of entries (directly, and
//      through both gateways) must not move the counter.
//   2. Bounded memory: with nobody polling, the trail holds exactly
//      AUDIT_LOG_CAPACITY entries and dropped_count() accounts for the rest.
//   3. Drop-oldest semantics: what survives is the NEWEST capacity entries,
//      in order.
//   4. Conservation under concurrency: drained + dropped + unread == appended,
//      each producer's entries stay in order, nothing is duplicated.
//
// No framework (CLAUDE.md zero-dependency rule): PASS/FAIL lines, non-zero
// exit on failure. The counting operator new lives in this file only, which
// is why this is its own executable.
#include "animus_security.hpp"

// GCC flags the replaced operator new/delete pair below (malloc/free under
// the hood) as mismatched when std::vector inlines them; it is a known false
// positive for exactly this counting-allocator idiom.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

namespace {
std::atomic<uint64_t> g_allocations{ 0 };
int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}
} // namespace

void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace animus::security;

namespace {

AccessToken token_for(uint64_t principal) { return AccessToken{ 1, principal, Role::Operator }; }

void test_counter_sanity() {
    std::printf("test_counter_sanity\n");
    const uint64_t before = g_allocations.load();
    int* p = new int(7);
    check(g_allocations.load() == before + 1, "the allocation counter sees a deliberate new");
    delete p;
}

void test_no_allocation_and_bounded() {
    std::printf("test_no_allocation_and_bounded\n");
    auto log = std::make_unique<BoundedAuditLog>(); // allocation happens here, before the window
    const uint64_t total = 5'000'000;
    const uint64_t before = g_allocations.load();
    for (uint64_t i = 0; i < total; ++i) log->append(token_for(i), Permission::RecordEvent, AuditOutcome::Allowed);
    check(g_allocations.load() == before, "5,000,000 appends with no consumer: zero heap allocations");
    check(log->size() == AUDIT_LOG_CAPACITY, "trail holds exactly AUDIT_LOG_CAPACITY entries (memory is bounded)");
    check(log->dropped_count() == total - AUDIT_LOG_CAPACITY, "dropped_count() accounts for every overwritten entry");

    std::vector<AuditEvent> out(AUDIT_LOG_CAPACITY + 16);
    const uint64_t before_drain = g_allocations.load();
    const size_t n = log->drain(out.data(), out.size());
    check(g_allocations.load() == before_drain, "drain allocates nothing");
    bool newest_in_order = n == AUDIT_LOG_CAPACITY;
    for (size_t i = 0; newest_in_order && i < n; ++i) newest_in_order = out[i].principal_id == total - AUDIT_LOG_CAPACITY + i;
    check(newest_in_order, "survivors are the NEWEST capacity entries, oldest-first (drop-oldest policy)");
    check(log->size() == 0 && log->drain(out.data(), out.size()) == 0, "drained trail is empty");
    check(log->drain(nullptr, 4) == 0, "null output buffer is rejected, not dereferenced");
}

void test_gateways_bounded() {
    std::printf("test_gateways_bounded\n");
    TenantRegistry registry;
    SecureTelemetryGateway telemetry(registry);
    SecureExecutionGateway execution(registry);
    const AccessToken admin{ 0, 1, Role::Admin };
    const AccessToken viewer{ 10, 2, Role::Viewer };
    check(telemetry.create_tenant(admin, 10, 1024), "admin creates tenant 10");
    check(!telemetry.record(viewer, 1, 1, 1), "viewer record() is denied");

    AuditEvent ev[2];
    check(telemetry.poll_audit_log(ev, 2) == 2, "create + denied record were both audited");
    check(ev[0].outcome == AuditOutcome::Allowed && ev[0].permission == Permission::ManageTenants, "entry 0: allowed ManageTenants");
    check(ev[1].outcome == AuditOutcome::Denied && ev[1].permission == Permission::RecordEvent, "entry 1: denied RecordEvent");

    // Denied calls never touch an Engine, so any allocation here is the audit path's.
    const uint64_t before = g_allocations.load();
    const uint64_t calls = 2'000'000;
    for (uint64_t i = 0; i < calls; ++i) telemetry.record(viewer, 1, 1, i);
    for (uint64_t i = 0; i < calls; ++i) execution.create_execution_tenant(viewer, 10);
    check(g_allocations.load() == before, "4,000,000 gateway calls with no poller: zero heap allocations");
    check(telemetry.audit_dropped_count() == calls - AUDIT_LOG_CAPACITY, "telemetry gateway reports its drops");
    check(execution.audit_dropped_count() == calls - AUDIT_LOG_CAPACITY, "execution gateway reports its drops");
    std::vector<AuditEvent> out(AUDIT_LOG_CAPACITY + 1);
    check(telemetry.poll_audit_log(out.data(), out.size()) == AUDIT_LOG_CAPACITY, "telemetry trail is capped at capacity");
    check(execution.poll_execution_audit_log(out.data(), out.size()) == AUDIT_LOG_CAPACITY, "execution trail is capped at capacity");
}

void test_concurrent_conservation() {
    std::printf("test_concurrent_conservation\n");
    auto log = std::make_unique<BoundedAuditLog>();
    constexpr int kProducers = 4;
    constexpr uint64_t kPerProducer = 500'000;
    std::atomic<bool> producers_done{ false };
    std::atomic<uint64_t> drained{ 0 };
    std::atomic<bool> order_ok{ true };

    std::thread consumer([&] {
        std::vector<AuditEvent> buf(256);
        uint64_t last[kProducers];
        bool seen[kProducers] = {};
        auto take = [&](size_t n) {
            for (size_t i = 0; i < n; ++i) {
                const int p = static_cast<int>(buf[i].principal_id >> 32);
                const uint64_t seq = buf[i].principal_id & 0xFFFFFFFFull;
                if (seen[p] && seq <= last[p]) order_ok = false; // duplicate or reordered
                seen[p] = true;
                last[p] = seq;
            }
            drained += n;
        };
        while (!producers_done.load()) take(log->drain(buf.data(), buf.size()));
        for (size_t n; (n = log->drain(buf.data(), buf.size())) > 0;) take(n);
    });
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (uint64_t i = 0; i < kPerProducer; ++i)
                log->append(token_for((static_cast<uint64_t>(p) << 32) | i), Permission::PollSignals, AuditOutcome::Allowed);
        });
    }
    for (auto& t : producers) t.join();
    producers_done = true;
    consumer.join();

    const uint64_t appended = kProducers * kPerProducer;
    check(drained.load() + log->dropped_count() == appended, "drained + dropped == appended (exact conservation)");
    check(order_ok.load(), "each producer's entries arrive in order with no duplicates");
    check(log->size() == 0, "trail fully drained");
}

} // namespace

int main() {
    std::printf("Animus bounded audit trail (animus_security.hpp), capacity %zu, sizeof(BoundedAuditLog)=%zu\n\n",
        AUDIT_LOG_CAPACITY, sizeof(BoundedAuditLog));
    test_counter_sanity();
    test_no_allocation_and_bounded();
    test_gateways_bounded();
    test_concurrent_conservation();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
