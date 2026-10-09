// Regression + stress test for ShmRingMpmc<T> (include/animus/shm_ipc.hpp), the
// shared-memory multi-producer / multi-consumer ring.
//
// What it proves, in order:
//   1. Layout on the LIVE mapping: the producers' cursor, the consumers' cursor
//      and cell 0 each start on their own 64-byte boundary, and the two cursors
//      are at least a cache line apart (the compile-time static_asserts in the
//      header say it should be so; this checks the bytes the OS actually mapped).
//   2. Single-thread semantics: capacity rounding, exact fullness, strict FIFO,
//      emptiness, and thousands of wrap-arounds at varying occupancy.
//   3. A rejection matrix: wrong record size, same size but different schema,
//      every other ring kind, a nonexistent name, a name collision, a segment
//      whose create() has not finished, a header that claims more cells than the
//      mapping holds, and absurd capacities -- all refused, none crash.
//   4. AnimusGetMetrics() reports the ring (kind, capacity, head, tail, lag).
//   5. Concurrency in one process: P producers and C consumers, each with its OWN
//      mapping of the segment (a different virtual address for the same pages,
//      as separate processes would have), exactly-once delivery checked with a
//      bitmap, per-(consumer, producer) order, and per-frame integrity.
//   6. Concurrency across OS processes: the same, with P producer processes and
//      C consumer processes launched by re-executing this binary.
//
// No framework (CLAUDE.md's zero-dependency rule): PASS/FAIL lines and a
// non-zero exit code on failure.
//
// Build: g++ -std=c++17 -O2 -pthread -Iinclude tests/test_shm_mpmc.cpp -o test_shm_mpmc -lrt
//        cl /std:c++17 /O2 /EHsc /Iinclude tests\test_shm_mpmc.cpp
// Usage: test_shm_mpmc [--skip-xproc] [--frames N]
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/telemetry.hpp"
#include "frame_check.hpp"

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

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <signal.h>
    #include <sys/mman.h>
    #include <sys/resource.h>
    #include <sys/stat.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using animus::ExecutionEvent;
using animus::sys::ipc::MpmcRingHeader;
using animus::sys::ipc::RingKind;
using animus::sys::ipc::SharedMemoryRegion;
using Ring = animus::sys::ipc::ShmRingMpmc<ExecutionEvent>;

// Same size as ExecutionEvent (40 bytes) and trivially copyable, but a different
// registered schema name: the version hash must tell them apart.
struct OtherSchema40 {
    uint64_t a, b;
    int64_t c, d;
    uint32_t e, f;
};
static_assert(sizeof(OtherSchema40) == sizeof(ExecutionEvent), "must collide on size on purpose");
ANIMUS_DEFINE_SCHEMA(OtherSchema40, "<QQqqII")

struct Wide64 {
    uint64_t w[8];
};

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

std::string unique_name(const char* tag) {
    static std::atomic<unsigned> counter{ 0 };
    return std::string("animus_mpmc_") + tag + "_" +
           std::to_string(animus::sys::lifecycle::current_process_id()) + "_" +
           std::to_string(counter.fetch_add(1)) + "_" +
           std::to_string(static_cast<unsigned long long>(
               std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFu));
}

// Unlinks the segment name when it leaves scope, so a failed check cannot leak
// a /dev/shm entry into the next run.
struct NameGuard {
    std::string name;
    explicit NameGuard(std::string n) : name(std::move(n)) {}
    ~NameGuard() { SharedMemoryRegion::unlink(name.c_str()); }
};

// ---------------------------------------------------------------------------
void test_layout_on_live_mapping() {
    std::printf("test_layout_on_live_mapping\n");
    NameGuard g(unique_name("layout"));
    auto ring = Ring::create(g.name.c_str(), 64);
    check(ring != nullptr, "create(64) succeeds");
    if (!ring) return;

    SharedMemoryRegion raw;
    check(SharedMemoryRegion::open(g.name.c_str(), raw, /*prefault=*/false), "raw attach to the same segment");
    if (!raw.valid()) return;
    const auto* h = reinterpret_cast<const MpmcRingHeader*>(raw.data());
    const auto base = reinterpret_cast<uintptr_t>(h);
    const auto enq = reinterpret_cast<uintptr_t>(&h->enqueue_pos);
    const auto deq = reinterpret_cast<uintptr_t>(&h->dequeue_pos);
    const auto cell0 = reinterpret_cast<uintptr_t>(h + 1);

    std::printf("    cache line %zu B | sizeof(header) %zu | enqueue_pos @+%zu | dequeue_pos @+%zu | cell stride %zu B\n",
        static_cast<size_t>(animus::cache_line_size), sizeof(MpmcRingHeader),
        static_cast<size_t>(enq - base), static_cast<size_t>(deq - base), ring->stride());
    check(base % 64 == 0, "the mapping (header) starts on a 64-byte boundary");
    check(enq % 64 == 0, "enqueue_pos (producers' cursor) is 64-byte aligned");
    check(deq % 64 == 0, "dequeue_pos (consumers' cursor) is 64-byte aligned");
    check(cell0 % 64 == 0, "cell 0 is 64-byte aligned");
    check(deq - enq >= 64, "the two cursors are at least one cache line apart");
    check(enq - base >= 64 && (enq - base) + 8 <= sizeof(MpmcRingHeader), "the producers' cursor is outside the read-only descriptor line(s)");
    check(ring->stride() % 64 == 0 && ring->stride() >= ring->payload_size() + 8, "cells are padded to whole cache lines");
    check(h->ring_kind == static_cast<uint64_t>(RingKind::Mpmc), "ring_kind == Mpmc on the wire");
    check(std::strcmp(ring->wire_format(), animus::kExecutionEventWireFormat) == 0, "wire_format round-trips");
}

// ---------------------------------------------------------------------------
void test_single_thread_semantics() {
    std::printf("test_single_thread_semantics\n");
    {
        NameGuard g(unique_name("cap"));
        auto r0 = Ring::create(g.name.c_str(), 0);
        check(r0 && r0->capacity() == 2, "capacity 0 rounds up to the minimum, 2");
        r0.reset();
        SharedMemoryRegion::unlink(g.name.c_str());
        auto r5 = Ring::create(g.name.c_str(), 5);
        check(r5 && r5->capacity() == 8, "capacity 5 rounds up to 8");
        r5.reset();
        SharedMemoryRegion::unlink(g.name.c_str());
        auto r1000 = Ring::create(g.name.c_str(), 1000);
        check(r1000 && r1000->capacity() == 1024, "capacity 1000 rounds up to 1024");
    }

    NameGuard g(unique_name("sem"));
    auto a = Ring::create(g.name.c_str(), 8);
    auto b = Ring::open(g.name.c_str());
    check(a && b, "create + independent open() views");
    if (!a || !b) return;

    ExecutionEvent out{};
    check(!a->try_pop(out), "pop from an empty ring fails");
    check(a->size_approx() == 0, "size_approx() == 0 when empty");

    size_t pushed = 0;
    for (uint64_t i = 0; i < 20; ++i) {
        if (a->try_push(animus_test::make_frame(0, i))) ++pushed;
    }
    check(pushed == 8, "exactly capacity pushes succeed; the rest are refused (full)");
    check(b->size_approx() == 8, "the second view sees 8 in flight");
    check(!b->try_push(animus_test::make_frame(0, 99)), "push to a full ring fails through either view");

    bool fifo = true;
    for (uint64_t i = 0; i < 8; ++i) {
        fifo = fifo && b->try_pop(out) && out.sequence == i && animus_test::frame_ok(out, 1);
    }
    check(fifo, "pops come back strictly FIFO, intact, through the OTHER view");
    check(!a->try_pop(out), "ring is empty again after draining");

    // Many laps at varying occupancy: push k, pop j, so the cursors cross the
    // mask boundary at every phase.
    uint64_t next_push = 100, next_pop = 100, in_flight = 0;
    bool ok = true;
    for (uint64_t round = 0; round < 20000 && ok; ++round) {
        const uint64_t k = 1 + round % 7;
        for (uint64_t i = 0; i < k; ++i) {
            if (a->try_push(animus_test::make_frame(0, next_push))) { ++next_push; ++in_flight; }
        }
        const uint64_t j = 1 + (round * 3) % 7;
        for (uint64_t i = 0; i < j; ++i) {
            if (b->try_pop(out)) {
                ok = ok && out.sequence == next_pop && animus_test::frame_ok(out, 1);
                ++next_pop;
                --in_flight;
            }
        }
        ok = ok && (a->size_approx() == in_flight);
    }
    check(ok, "20,000 uneven push/pop rounds: FIFO order, integrity and occupancy exact across many laps");
    check(next_push > 8 * 1000, "the test really did wrap the 8-cell ring thousands of times");
}

// ---------------------------------------------------------------------------
void test_rejection_matrix() {
    std::printf("test_rejection_matrix\n");
    NameGuard g(unique_name("rej"));
    auto ring = Ring::create(g.name.c_str(), 16);
    check(ring != nullptr, "create");
    if (!ring) return;

    check(Ring::open("animus_mpmc_definitely_not_a_segment") == nullptr, "open() of a nonexistent segment fails");
    check(Ring::create(g.name.c_str(), 16) == nullptr, "create() over an existing name fails (never reuses a segment)");
    check(animus::sys::ipc::ShmRingMpmc<OtherSchema40>::open(g.name.c_str()) == nullptr,
        "same record size, different registered schema: refused");
    check(animus::sys::ipc::ShmRingMpmc<Wide64>::open(g.name.c_str()) == nullptr, "different record size: refused");
    check(animus::sys::ipc::ShmRing<ExecutionEvent>::open(g.name.c_str()) == nullptr, "ShmRing (SPSC) refuses an MPMC segment");
    check(animus::sys::ipc::SpmcRing<ExecutionEvent>::open(g.name.c_str()) == nullptr, "SpmcRing refuses an MPMC segment");
    check(animus::sys::ipc::RawSchemaView::open(g.name.c_str()) == nullptr, "RawSchemaView refuses an MPMC segment");

    {
        NameGuard s(unique_name("spsc"));
        auto spsc = animus::sys::ipc::ShmRing<ExecutionEvent>::create(s.name.c_str(), 16);
        check(spsc != nullptr, "create an SPSC ring");
        check(Ring::open(s.name.c_str()) == nullptr, "ShmRingMpmc refuses an SPSC segment");
    }
    {
        NameGuard s(unique_name("spmc"));
        auto spmc = animus::sys::ipc::SpmcRing<ExecutionEvent>::create(s.name.c_str(), 16);
        check(spmc != nullptr, "create an SPMC ring");
        check(Ring::open(s.name.c_str()) == nullptr, "ShmRingMpmc refuses an SPMC segment");
    }
    {
        // A zero-filled mapping of plausible size: create() has "not finished".
        NameGuard s(unique_name("uninit"));
        SharedMemoryRegion raw;
        check(SharedMemoryRegion::create(s.name.c_str(), 4096 * 4, raw), "create a raw zero-filled segment");
        check(Ring::open(s.name.c_str()) == nullptr, "a segment whose `ready` word is unset (create in progress) is declined");
    }
    {
        // A fully "ready" header that claims far more cells than the mapping holds.
        NameGuard s(unique_name("trunc"));
        SharedMemoryRegion raw;
        check(SharedMemoryRegion::create(s.name.c_str(), sizeof(MpmcRingHeader) + 4096, raw), "create a small raw segment");
        if (raw.valid()) {
            auto* h = new (raw.data()) MpmcRingHeader();
            h->capacity = uint64_t{ 1 } << 20;
            h->mask = h->capacity - 1;
            h->schema_version_hash = animus::schema::Traits<ExecutionEvent>::kVersionHash;
            h->payload_size = sizeof(ExecutionEvent);
            h->stride = sizeof(Ring::Cell);
            h->ready.store(MpmcRingHeader::kReadyMagic, std::memory_order_release);
            check(Ring::open(s.name.c_str()) == nullptr, "a header claiming more cells than the mapping holds is refused");
        }
    }
    check(Ring::create(unique_name("huge").c_str(), SIZE_MAX) == nullptr, "capacity SIZE_MAX is refused, not wrapped");
    check(Ring::create(unique_name("huge").c_str(), size_t{ 1 } << 62) == nullptr, "capacity 2^62 is refused, not wrapped");
    check(Ring::create(nullptr, 8) == nullptr, "a null name is refused");
}

// ---------------------------------------------------------------------------
void test_telemetry() {
    std::printf("test_telemetry\n");
    NameGuard g(unique_name("tele"));
    auto ring = Ring::create(g.name.c_str(), 32);
    if (!ring) { check(false, "create"); return; }
    for (uint64_t i = 0; i < 10; ++i) ring->try_push(animus_test::make_frame(0, i));
    ExecutionEvent out{};
    for (int i = 0; i < 4; ++i) ring->try_pop(out);

    animus::TelemetrySnapshot snap;
    check(animus::AnimusGetMetrics(g.name.c_str(), &snap) && snap.valid, "AnimusGetMetrics() reads the MPMC ring");
    check(snap.ring_kind == RingKind::Mpmc, "ring_kind reported as Mpmc");
    check(snap.capacity == 32, "capacity reported");
    check(snap.payload_size == sizeof(ExecutionEvent) && snap.stride == ring->stride(), "payload_size / stride reported");
    check(snap.current_write_head == 10 && snap.total_pushed_events == 10, "write head == 10 pushes");
    check(snap.has_consumer_lag && snap.consumer_tail == 4 && snap.consumer_lag == 6, "consumer tail 4, lag 6");
    check(!snap.has_dropped_count && snap.dropped_events == 0, "lossless ring reports no drop counter");
    check(std::strcmp(snap.wire_format, animus::kExecutionEventWireFormat) == 0, "wire_format reported");
}

// ---------------------------------------------------------------------------
// Linux POSIX shared memory: the two properties shm_ipc.hpp's SharedMemoryRegion
// adds over a bare shm_open/ftruncate/mmap, checked against the kernel's own
// accounting rather than assumed:
//   1. posix_fallocate reserved real backing at create() -- st_blocks covers the
//      whole segment even though nothing has touched it. (With ftruncate alone the
//      file is sparse, st_blocks is ~0, and a full /dev/shm shows up later as a
//      SIGBUS in a hot loop instead of as a create() failure.)
//   2. prefault=true takes the first-touch page faults inside create(); a lazy
//      mapping takes one minor fault per 4 KiB page on the first write to it.
#if defined(__linux__)
// A sanitizer's shadow memory takes its own first-touch faults (about one per
// 32 KiB of user memory under ASan), which swamps the count being measured.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

long minor_faults() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

long faults_writing_every_page(void* base, size_t size, size_t page) {
    auto* bytes = static_cast<volatile unsigned char*>(base);
    const long before = minor_faults();
    for (size_t off = 0; off < size; off += page) bytes[off] = 1;
    return minor_faults() - before;
}

void test_linux_posix_backing_and_prefault() {
    std::printf("test_linux_posix_backing_and_prefault\n");
    const size_t size = size_t{ 64 } << 20;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));

    NameGuard lazy_name(unique_name("pf_lazy"));
    SharedMemoryRegion lazy;
    check(SharedMemoryRegion::create(lazy_name.name.c_str(), size, lazy, /*prefault=*/false), "create 64 MiB, prefault=false");
    if (!lazy.valid()) return;
    {
        const int fd = shm_open(lazy_name.name.c_str(), O_RDWR, 0600);
        struct stat st{};
        const bool ok = fd >= 0 && fstat(fd, &st) == 0;
        if (fd >= 0) close(fd);
        std::printf("    st_blocks * 512 = %lld MiB of %zu MiB reserved, nothing touched yet\n",
            ok ? static_cast<long long>(st.st_blocks) * 512 / (1 << 20) : -1LL, size >> 20);
        check(ok && static_cast<size_t>(st.st_blocks) * 512 >= size,
            "posix_fallocate reserved backing for the whole segment at create() (not a sparse file)");
    }
    const long lazy_faults = faults_writing_every_page(lazy.data(), size, page);

    NameGuard warm_name(unique_name("pf_warm"));
    SharedMemoryRegion warm;
    check(SharedMemoryRegion::create(warm_name.name.c_str(), size, warm, /*prefault=*/true), "create 64 MiB, prefault=true");
    if (!warm.valid()) return;
    const long warm_faults = faults_writing_every_page(warm.data(), size, page);

    std::printf("    minor faults writing every page once: lazy mapping %ld, prefaulted mapping %ld (%zu pages)\n",
        lazy_faults, warm_faults, size / page);
    if (kSanitized) {
        std::printf("    (fault-count assertions skipped under a sanitizer: shadow-memory faults dominate the count)\n");
    } else {
        check(warm_faults <= 16, "prefaulted mapping: first write to every page takes (almost) no minor faults");
        check(lazy_faults > warm_faults + 16, "lazy mapping: the same writes DO fault -- the prefault is what removed them");
    }

    // A second view of the same pages, attached with prefault=true, must be hot too.
    SharedMemoryRegion view;
    check(SharedMemoryRegion::open(warm_name.name.c_str(), view, /*prefault=*/true), "attach a second view with prefault=true");
    if (view.valid() && !kSanitized) {
        check(faults_writing_every_page(view.data(), size, page) <= 16, "an attached, prefaulted view is also fault-free on first write");
    }
}
#endif

// ---------------------------------------------------------------------------
// Concurrency in one process.
struct ConsumerLocal {
    std::vector<std::vector<uint64_t>> seen; // [producer][word]
    std::vector<uint64_t> last_seq;          // [producer], UINT64_MAX == none yet
    uint64_t received = 0, corrupt = 0, order_errors = 0;
};

void test_concurrent_in_process(uint32_t producers, uint32_t consumers, uint64_t frames_per_producer, size_t capacity) {
    std::printf("test_concurrent_in_process (%u producers x %u consumers x %llu frames, capacity %zu)\n",
        producers, consumers, static_cast<unsigned long long>(frames_per_producer), capacity);
    NameGuard g(unique_name("mt"));
    auto owner = Ring::create(g.name.c_str(), capacity);
    if (!owner) { check(false, "create"); return; }

    const uint64_t words = (frames_per_producer + 63) / 64;
    std::vector<ConsumerLocal> locals(consumers);
    for (auto& c : locals) {
        c.seen.assign(producers, std::vector<uint64_t>(words, 0));
        c.last_seq.assign(producers, UINT64_MAX);
    }
    std::atomic<uint32_t> producers_done{ 0 };
    std::atomic<bool> go{ false };
    std::atomic<uint32_t> bad_open{ 0 };

    std::vector<std::thread> threads;
    for (uint32_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            auto view = Ring::open(g.name.c_str()); // this thread's OWN mapping
            if (!view) { bad_open.fetch_add(1); producers_done.fetch_add(1); return; }
            while (!go.load(std::memory_order_acquire)) {}
            for (uint64_t s = 0; s < frames_per_producer; ++s) {
                const ExecutionEvent e = animus_test::make_frame(p, s);
                while (!view->try_push(e)) animus::cpu_relax();
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (uint32_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            auto view = Ring::open(g.name.c_str());
            if (!view) { bad_open.fetch_add(1); return; }
            ConsumerLocal& me = locals[c];
            while (!go.load(std::memory_order_acquire)) {}
            ExecutionEvent e;
            for (;;) {
                const bool done = producers_done.load(std::memory_order_acquire) == producers;
                if (!view->try_pop(e)) {
                    if (done) break; // every push had completed before `done` was read, and the ring is still empty
                    animus::cpu_relax();
                    continue;
                }
                ++me.received;
                if (!animus_test::frame_ok(e, producers)) { ++me.corrupt; continue; }
                if (e.sequence >= frames_per_producer) { ++me.corrupt; continue; }
                uint64_t& last = me.last_seq[e.instrument_id];
                if (last != UINT64_MAX && e.sequence <= last) ++me.order_errors;
                last = e.sequence;
                me.seen[e.instrument_id][e.sequence >> 6] |= uint64_t{ 1 } << (e.sequence & 63);
            }
        });
    }
    const auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    uint64_t received = 0, corrupt = 0, order_errors = 0, duplicates = 0, covered = 0;
    for (const auto& c : locals) { received += c.received; corrupt += c.corrupt; order_errors += c.order_errors; }
    for (uint32_t p = 0; p < producers; ++p) {
        for (uint64_t w = 0; w < words; ++w) {
            uint64_t acc = 0;
            for (const auto& c : locals) {
                duplicates += animus_test::popcount64(acc & c.seen[p][w]);
                acc |= c.seen[p][w];
            }
            covered += animus_test::popcount64(acc);
        }
    }
    const uint64_t expected = static_cast<uint64_t>(producers) * frames_per_producer;
    std::printf("    %llu frames in %.2f s (%.2f M frames/s)\n", static_cast<unsigned long long>(received), secs,
        static_cast<double>(received) / secs * 1e-6);
    check(bad_open.load() == 0, "every thread attached its own view");
    check(received == expected, "frames received == frames produced (nothing lost, nothing extra)");
    check(covered == expected && duplicates == 0, "every (producer, sequence) delivered exactly once across all consumers");
    check(corrupt == 0, "zero corrupt frames (integrity checksum)");
    check(order_errors == 0, "per-(consumer, producer) sequence strictly increasing");
    check(owner->size_approx() == 0, "ring drained: enqueued == dequeued");
}

// ---------------------------------------------------------------------------
// Cross-process.
struct XprocShared {
    std::atomic<uint64_t> consumed_total;
    std::atomic<uint64_t> duplicates;
    std::atomic<uint64_t> corrupt;
    std::atomic<uint64_t> order_errors;
    std::atomic<uint32_t> producers_done;
    uint32_t producers;
    uint64_t frames_per_producer;
    uint64_t words_per_producer;
    // 64-byte aligned bitmap of std::atomic<uint64_t> follows
    static constexpr size_t kBitmapOffset = 128;
    std::atomic<uint64_t>* bitmap() { return reinterpret_cast<std::atomic<uint64_t>*>(reinterpret_cast<unsigned char*>(this) + kBitmapOffset); }
};
static_assert(sizeof(XprocShared) <= XprocShared::kBitmapOffset, "header must fit before the bitmap");

size_t xproc_bytes(uint32_t producers, uint64_t frames) {
    return XprocShared::kBitmapOffset + static_cast<size_t>(producers) * static_cast<size_t>((frames + 63) / 64) * sizeof(uint64_t);
}

template <typename Fn>
bool retry_open(Fn&& fn) {
    for (int i = 0; i < 400; ++i) {
        if (fn()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

int child_producer(const char* ring_name, const char* results_name, uint32_t index, uint64_t frames) {
    std::unique_ptr<Ring> ring;
    SharedMemoryRegion res;
    if (!retry_open([&] { ring = Ring::open(ring_name); return ring != nullptr; })) return 10;
    if (!retry_open([&] { return SharedMemoryRegion::open(results_name, res); }) || !res.valid()) return 11;
    auto* sh = static_cast<XprocShared*>(res.data());
    for (uint64_t s = 0; s < frames; ++s) {
        const ExecutionEvent e = animus_test::make_frame(index, s);
        while (!ring->try_push(e)) animus::cpu_relax();
    }
    sh->producers_done.fetch_add(1, std::memory_order_release);
    return 0;
}

int child_consumer(const char* ring_name, const char* results_name, uint32_t producers) {
    std::unique_ptr<Ring> ring;
    SharedMemoryRegion res;
    if (!retry_open([&] { ring = Ring::open(ring_name); return ring != nullptr; })) return 10;
    if (!retry_open([&] { return SharedMemoryRegion::open(results_name, res); }) || !res.valid()) return 11;
    auto* sh = static_cast<XprocShared*>(res.data());
    std::atomic<uint64_t>* bitmap = sh->bitmap();
    std::vector<uint64_t> last(producers, UINT64_MAX);
    ExecutionEvent e;
    for (;;) {
        const bool done = sh->producers_done.load(std::memory_order_acquire) == producers;
        if (!ring->try_pop(e)) {
            if (done) break;
            animus::cpu_relax();
            continue;
        }
        sh->consumed_total.fetch_add(1, std::memory_order_relaxed);
        if (!animus_test::frame_ok(e, producers) || e.sequence >= sh->frames_per_producer) {
            sh->corrupt.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (last[e.instrument_id] != UINT64_MAX && e.sequence <= last[e.instrument_id]) {
            sh->order_errors.fetch_add(1, std::memory_order_relaxed);
        }
        last[e.instrument_id] = e.sequence;
        const uint64_t bit = uint64_t{ 1 } << (e.sequence & 63);
        const uint64_t prev = bitmap[e.instrument_id * sh->words_per_producer + (e.sequence >> 6)].fetch_or(bit, std::memory_order_relaxed);
        if (prev & bit) sh->duplicates.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

#if defined(_WIN32)
using ChildHandle = HANDLE;
std::string self_path(const char* argv0) {
    char buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string(argv0);
}
bool spawn(const std::string& exe, const std::vector<std::string>& args, ChildHandle& out) {
    std::string cmd = "\"" + exe + "\"";
    for (const auto& a : args) cmd += " " + a;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    out = pi.hProcess;
    return true;
}
// Returns the exit code, or -1 on timeout (the child is terminated).
int wait_child(ChildHandle h, unsigned timeout_ms) {
    if (WaitForSingleObject(h, timeout_ms) != WAIT_OBJECT_0) { TerminateProcess(h, 99); CloseHandle(h); return -1; }
    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return static_cast<int>(code);
}
#else
using ChildHandle = pid_t;
std::string self_path(const char* argv0) {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string(argv0);
}
bool spawn(const std::string& exe, const std::vector<std::string>& args, ChildHandle& out) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    out = pid;
    return true;
}
int wait_child(ChildHandle pid, unsigned timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        if (r < 0) return -2;
        if (std::chrono::steady_clock::now() > deadline) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return -1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif

void test_cross_process(const char* argv0, uint32_t producers, uint32_t consumers, uint64_t frames_per_producer) {
    std::printf("test_cross_process (%u producer processes x %u consumer processes x %llu frames)\n",
        producers, consumers, static_cast<unsigned long long>(frames_per_producer));
    NameGuard ring_name(unique_name("xring"));
    NameGuard res_name(unique_name("xres"));
    auto ring = Ring::create(ring_name.name.c_str(), 1024);
    SharedMemoryRegion res;
    const size_t bytes = xproc_bytes(producers, frames_per_producer);
    if (!ring || !SharedMemoryRegion::create(res_name.name.c_str(), bytes, res)) { check(false, "create ring + results segment"); return; }
    auto* sh = static_cast<XprocShared*>(res.data());
    new (sh) XprocShared();
    sh->consumed_total.store(0); sh->duplicates.store(0); sh->corrupt.store(0); sh->order_errors.store(0); sh->producers_done.store(0);
    sh->producers = producers;
    sh->frames_per_producer = frames_per_producer;
    sh->words_per_producer = (frames_per_producer + 63) / 64;
    std::atomic<uint64_t>* bitmap = sh->bitmap();
    for (size_t i = 0; i < (bytes - XprocShared::kBitmapOffset) / sizeof(uint64_t); ++i) new (&bitmap[i]) std::atomic<uint64_t>(0);

    const std::string exe = self_path(argv0);
    std::vector<ChildHandle> handles;
    bool spawned = true;
    for (uint32_t c = 0; c < consumers; ++c) {
        ChildHandle h{};
        spawned = spawned && spawn(exe, { "--child-consumer", ring_name.name, res_name.name, std::to_string(producers) }, h);
        if (spawned) handles.push_back(h);
    }
    for (uint32_t p = 0; p < producers; ++p) {
        ChildHandle h{};
        spawned = spawned && spawn(exe, { "--child-producer", ring_name.name, res_name.name, std::to_string(p), std::to_string(frames_per_producer) }, h);
        if (spawned) handles.push_back(h);
    }
    check(spawned, "all child processes launched");
    bool exits_ok = true;
    for (auto h : handles) {
        const int code = wait_child(h, 120000);
        if (code != 0) std::printf("    child exit code %d (10/11 = could not attach, 127 = exec failed, -1 = timed out)\n", code);
        exits_ok = (code == 0) && exits_ok;
    }
    check(exits_ok, "every child process exited 0 (none timed out, crashed or failed to attach)");

    const uint64_t expected = static_cast<uint64_t>(producers) * frames_per_producer;
    uint64_t covered = 0;
    for (size_t i = 0; i < (bytes - XprocShared::kBitmapOffset) / sizeof(uint64_t); ++i) covered += animus_test::popcount64(bitmap[i].load());
    std::printf("    consumed %llu of %llu frames across %u consumer processes\n",
        static_cast<unsigned long long>(sh->consumed_total.load()), static_cast<unsigned long long>(expected), consumers);
    check(sh->consumed_total.load() == expected, "frames consumed across processes == frames produced");
    check(covered == expected && sh->duplicates.load() == 0, "every (producer, sequence) delivered exactly once across processes");
    check(sh->corrupt.load() == 0, "zero corrupt frames across processes");
    check(sh->order_errors.load() == 0, "per-(consumer process, producer) order preserved");
    check(ring->size_approx() == 0, "ring drained");
}

} // namespace

int main(int argc, char** argv) {
    // Child roles (launched by test_cross_process re-executing this binary).
    if (argc >= 2 && std::strcmp(argv[1], "--child-producer") == 0 && argc == 6) {
        return child_producer(argv[2], argv[3], static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)),
                              std::strtoull(argv[5], nullptr, 10));
    }
    if (argc >= 2 && std::strcmp(argv[1], "--child-consumer") == 0 && argc == 5) {
        return child_consumer(argv[2], argv[3], static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)));
    }

    bool skip_xproc = false;
    uint64_t frames = 500'000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--skip-xproc") == 0) skip_xproc = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "usage: %s [--skip-xproc] [--frames N]\n", argv[0]); return 2; }
    }

    std::printf("ShmRingMpmc<ExecutionEvent> regression (cache line %zu B)\n", static_cast<size_t>(animus::cache_line_size));
    test_layout_on_live_mapping();
    test_single_thread_semantics();
    test_rejection_matrix();
    test_telemetry();
#if defined(__linux__)
    test_linux_posix_backing_and_prefault();
#endif
    test_concurrent_in_process(1, 1, frames, 64);
    test_concurrent_in_process(4, 4, frames, 1024);
    test_concurrent_in_process(8, 2, frames / 2, 256);
    test_concurrent_in_process(2, 8, frames / 2, 256);
    if (!skip_xproc) test_cross_process(argv[0], 2, 2, frames);

    std::printf("\n%s -- %d checks, %d failed\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
