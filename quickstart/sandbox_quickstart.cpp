// sandbox_quickstart.cpp -- AnimusCore drop-in integration harness
//
// One file, standard library + POSIX/glibc only. Demonstrates the complete producer -> consumer hot path:
//
//   * lock-free SPSC ring, power-of-two capacity, statically allocated (no heap, no placement tricks)
//   * every shared index on its own alignas(64) cacheline; one 64-byte slot = one cacheline
//   * acquire/release only (plain MOVs on x86-64; no LOCK-prefixed RMW, no seq_cst fences on the hot path)
//   * invariant-TSC timestamps: producer stamps immediately before publish, consumer reads RDTSCP after dequeue
//   * producer and consumer pinned with pthread_setaffinity_np, on DIFFERENT PHYSICAL CORES (checked via sysfs)
//   * glibc allocator interposer that counts malloc-family / free calls made by the two hot threads
//   * log-linear histogram (<= 1.6% relative error above 127 cycles, exact below) so a 30-minute run
//     needs no sample buffer
//
// Build:   g++ -O3 -march=native -std=c++17 -pthread sandbox_quickstart.cpp -o sandbox_quickstart
//          clang++ -O3 -march=native -std=c++17 -pthread sandbox_quickstart.cpp -o sandbox_quickstart
// Run:     ./sandbox_quickstart                       (auto-picks two distinct physical cores)
//          ./sandbox_quickstart --producer-cpu 2 --consumer-cpu 4 --messages 5000000 --interval-ns 500
//          ./sandbox_quickstart --duration-s 1800 --report-interval-s 60     (soak mode)
//
// What is measured: ONE-WAY handoff latency = (consumer RDTSCP after dequeue) - (producer TSC stamped just
// before the release-store that publishes the slot). It is not a round trip and it includes the timer's own
// cost (reported as "timer floor"). Pacing matters: with --interval-ns 0 the producer floods the ring and the
// number you get is queueing delay, not transport latency.
//
// Not built for: non-glibc libc (the allocator audit is compiled out), non-x86-64, sanitizer builds (they
// provide their own malloc; the audit self-test will then report FAIL rather than silently pass).

#if !defined(__linux__) || !defined(__x86_64__)
#error "sandbox_quickstart.cpp targets Linux x86-64 (invariant TSC, pthread_setaffinity_np)."
#endif

#include <cpuid.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <type_traits>
#include <utility>

// =====================================================================================================
// 1. INTEGRATION POINT -- your payload goes here
// =====================================================================================================
// Requirements for Payload: trivially copyable, no pointers into memory the consumer cannot see.
// The slot carries a 16-byte header (sequence number + TSC stamp), so a payload of <= 48 bytes keeps the
// whole message on one cacheline; larger payloads are rounded up to the next multiple of 64 bytes.

struct MarketTick {                  // demo payload: 48 bytes
    std::int64_t  px_e8;             // price * 1e8
    std::int64_t  qty;
    std::uint32_t instrument_id;
    std::uint8_t  side;              // 0 = bid, 1 = ask
    std::uint8_t  venue;
    std::uint16_t flags;
    std::uint64_t exch_ts_ns;        // exchange timestamp (synthetic here)
    std::uint8_t  reserved[16];
};

using Payload = MarketTick;          // <-- swap for your own struct

// Producer-side hook: write one message into `p`. Runs BEFORE the TSC stamp, so its cost is not measured.
inline void fill_payload(Payload& p, std::uint64_t seq) noexcept {
    p.px_e8         = 100'00000000LL + static_cast<std::int64_t>(seq % 1000) * 1'000000LL;
    p.qty           = 1 + static_cast<std::int64_t>(seq % 97);
    p.instrument_id = static_cast<std::uint32_t>(seq % 512);
    p.side          = static_cast<std::uint8_t>(seq & 1u);
    p.venue         = 3;
    p.flags         = 0;
    p.exch_ts_ns    = seq * 1000u;
    std::memset(p.reserved, 0, sizeof p.reserved);
}

// Consumer-side hook: your decode / book-update / strategy call. Runs AFTER the RX stamp, so it is not
// measured either. Return false if the message is corrupt (counted as payload_errors; fails the run).
inline bool consume_payload(const Payload& p, std::uint64_t seq) noexcept {
    return p.qty == 1 + static_cast<std::int64_t>(seq % 97) &&
           p.px_e8 == 100'00000000LL + static_cast<std::int64_t>(seq % 1000) * 1'000000LL &&
           p.exch_ts_ns == seq * 1000u;
}

// =====================================================================================================
// 2. Platform primitives (x86-64)
// =====================================================================================================
namespace animus {

constexpr std::size_t kCacheLine = 64;

inline void cpu_relax() noexcept {
#if defined(ANIMUS_SPIN_PAUSE) && ANIMUS_SPIN_PAUSE
    _mm_pause();   // off by default: PAUSE costs up to ~140 cycles of wake-up latency on Skylake-family cores
#endif
}

// Serialised TSC read for the producer: LFENCE keeps RDTSC from being hoisted above the slot writes and
// keeps the release-store below from being hoisted above the read.
inline std::uint64_t tsc_stamp() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

// Consumer side: RDTSCP does not execute until all prior loads (the slot read) are complete.
inline std::uint64_t tsc_rx() noexcept {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

inline std::uint64_t tsc_plain() noexcept { return __rdtsc(); }   // pacing only, not a measurement

// =====================================================================================================
// 3. Slot + SPSC ring (single header pattern: copy this section into your tree)
// =====================================================================================================
template <class P>
struct alignas(kCacheLine) Slot {
    std::uint64_t seq;
    std::uint64_t tsc;     // producer TSC at publish
    P             payload;
};

template <class T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable<T>::value, "slot type must be trivially copyable");
    static_assert(alignof(T) >= kCacheLine, "slot type must start on a cacheline");
    static_assert(sizeof(T) % kCacheLine == 0, "slot size must be a whole number of cachelines");

public:
    static constexpr std::size_t kMask = N - 1;

    // ---- producer thread only -------------------------------------------------------------------
    // Zero-copy: returns the next free slot to write into, or nullptr if the ring is full.
    T* claim() noexcept {
        const std::size_t h = head_.value.load(std::memory_order_relaxed);
        if (h - prod_.cached_tail >= N) {
            prod_.cached_tail = tail_.value.load(std::memory_order_acquire);
            if (h - prod_.cached_tail >= N) return nullptr;
        }
        return &slots_[h & kMask];
    }
    // Makes the slot returned by the last claim() visible to the consumer.
    void publish() noexcept {
        head_.value.store(head_.value.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    bool try_push(const T& v) noexcept {
        T* s = claim();
        if (s == nullptr) return false;
        *s = v;
        publish();
        return true;
    }

    // ---- consumer thread only -------------------------------------------------------------------
    // Zero-copy: returns the oldest unread slot, or nullptr if empty. Valid until pop().
    const T* front() noexcept {
        const std::size_t t = tail_.value.load(std::memory_order_relaxed);
        if (t == cons_.cached_head) {
            cons_.cached_head = head_.value.load(std::memory_order_acquire);
            if (t == cons_.cached_head) return nullptr;
        }
        return &slots_[t & kMask];
    }
    void pop() noexcept {
        tail_.value.store(tail_.value.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    bool try_pop(T& out) noexcept {
        const T* s = front();
        if (s == nullptr) return false;
        out = *s;
        pop();
        return true;
    }

    // Touch every slot once so first-use page faults happen before the run. Call before threads start.
    void prefault() noexcept {
        std::memset(static_cast<void*>(slots_), 0, sizeof slots_);
        __asm__ volatile("" : : "r"(slots_) : "memory");
    }

private:
    struct alignas(kCacheLine) Index { std::atomic<std::size_t> value{0}; };
    struct alignas(kCacheLine) ProducerCache { std::size_t cached_tail{0}; };
    struct alignas(kCacheLine) ConsumerCache { std::size_t cached_head{0}; };

    Index         head_;    // written by producer, read by consumer
    Index         tail_;    // written by consumer, read by producer
    ProducerCache prod_;    // producer-private
    ConsumerCache cons_;    // consumer-private
    alignas(kCacheLine) T slots_[N];
};

// =====================================================================================================
// 4. Log-linear latency histogram (single writer, any number of relaxed readers)
// =====================================================================================================
// Values < 128 are exact; above that each power-of-two range is split into 64 buckets (<= 1/64 error).
// Percentiles are reported as the bucket's upper bound, i.e. they never understate.
constexpr std::size_t kHistBuckets = 59 * 64;

inline std::size_t hist_index(std::uint64_t v) noexcept {
    if (v < 128) return static_cast<std::size_t>(v);
    const unsigned msb   = 63u - static_cast<unsigned>(__builtin_clzll(v));
    const unsigned shift = msb - 6u;
    return static_cast<std::size_t>(64u * shift + (v >> shift));
}
inline std::uint64_t hist_upper(std::size_t idx) noexcept {
    if (idx < 128) return idx;
    const unsigned shift = static_cast<unsigned>(idx >> 6) - 1u;
    const std::uint64_t m = idx - 64u * shift;
    return ((m + 1u) << shift) - 1u;
}

struct alignas(kCacheLine) Histogram {
    std::atomic<std::uint64_t> bucket[kHistBuckets];
    std::atomic<std::uint64_t> max_v;
    std::atomic<std::uint64_t> min_v;

    void reset() noexcept {
        for (auto& b : bucket) b.store(0, std::memory_order_relaxed);
        max_v.store(0, std::memory_order_relaxed);
        min_v.store(~std::uint64_t{0}, std::memory_order_relaxed);
    }
    // Writer thread only: load+store, no LOCK prefix.
    void record(std::uint64_t v) noexcept {
        auto& b = bucket[hist_index(v)];
        b.store(b.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (v > max_v.load(std::memory_order_relaxed)) max_v.store(v, std::memory_order_relaxed);
        if (v < min_v.load(std::memory_order_relaxed)) min_v.store(v, std::memory_order_relaxed);
    }
};

struct Counts {
    std::uint64_t c[kHistBuckets];
    std::uint64_t total;
};

inline void snapshot(const Histogram& h, Counts& out) noexcept {
    out.total = 0;
    for (std::size_t i = 0; i < kHistBuckets; ++i) {
        out.c[i] = h.bucket[i].load(std::memory_order_relaxed);
        out.total += out.c[i];
    }
}
inline void delta(const Counts& now, const Counts& before, Counts& out) noexcept {
    out.total = 0;
    for (std::size_t i = 0; i < kHistBuckets; ++i) {
        out.c[i] = now.c[i] - before.c[i];
        out.total += out.c[i];
    }
}
inline std::uint64_t percentile(const Counts& k, double q) noexcept {
    if (k.total == 0) return 0;
    std::uint64_t rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(k.total)));
    if (rank < 1) rank = 1;
    if (rank > k.total) rank = k.total;
    std::uint64_t cum = 0;
    for (std::size_t i = 0; i < kHistBuckets; ++i) {
        cum += k.c[i];
        if (cum >= rank) return hist_upper(i);
    }
    return hist_upper(kHistBuckets - 1);
}
inline std::uint64_t highest(const Counts& k) noexcept {
    for (std::size_t i = kHistBuckets; i-- > 0;)
        if (k.c[i] != 0) return hist_upper(i);
    return 0;
}

}  // namespace animus

// =====================================================================================================
// 5. Hot-thread allocator audit (glibc interposer)
// =====================================================================================================
// Only calls made by threads that set t_hot are counted, so setup, printing and thread creation are
// excluded and the number is exactly "allocator calls on the producer / consumer hot loops".
namespace audit {
inline thread_local bool t_hot = false;                 // constant-initialised: reading it never allocates
inline std::atomic<std::uint64_t> g_allocs{0};
inline std::atomic<std::uint64_t> g_frees{0};
inline void note_alloc() noexcept { if (t_hot) g_allocs.fetch_add(1, std::memory_order_relaxed); }
inline void note_free() noexcept  { if (t_hot) g_frees.fetch_add(1, std::memory_order_relaxed); }
}  // namespace audit

#if defined(__GLIBC__)
#define ANIMUS_ALLOC_AUDIT 1
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void* __libc_valloc(std::size_t);
void* __libc_pvalloc(std::size_t);
void __libc_free(void*);

void* malloc(std::size_t n) noexcept { audit::note_alloc(); return __libc_malloc(n); }
void* calloc(std::size_t a, std::size_t b) noexcept { audit::note_alloc(); return __libc_calloc(a, b); }
void* realloc(void* p, std::size_t n) noexcept { audit::note_alloc(); return __libc_realloc(p, n); }
void free(void* p) noexcept { if (p != nullptr) audit::note_free(); __libc_free(p); }
void* memalign(std::size_t al, std::size_t n) noexcept { audit::note_alloc(); return __libc_memalign(al, n); }
void* aligned_alloc(std::size_t al, std::size_t n) noexcept { audit::note_alloc(); return __libc_memalign(al, n); }
// glibc implements valloc/pvalloc below the public memalign symbol; without these two they would be invisible.
void* valloc(std::size_t n) noexcept { audit::note_alloc(); return __libc_valloc(n); }
void* pvalloc(std::size_t n) noexcept { audit::note_alloc(); return __libc_pvalloc(n); }
int posix_memalign(void** out, std::size_t al, std::size_t n) noexcept {
    audit::note_alloc();
    void* p = __libc_memalign(al, n);
    if (p == nullptr) return ENOMEM;
    *out = p;
    return 0;
}
}
#else
#define ANIMUS_ALLOC_AUDIT 0
#endif

// Control for the "0 allocations" claim: prove the interposer sees malloc and operator new in THIS process.
// The pointers are laundered through empty asm so the compiler cannot delete the pairs.
static bool audit_selftest() noexcept {
#if ANIMUS_ALLOC_AUDIT
    audit::t_hot = true;
    void* a = std::malloc(24);
    __asm__ volatile("" : : "r"(a) : "memory");
    std::free(a);
    const bool malloc_seen = audit::g_allocs.load() >= 1 && audit::g_frees.load() >= 1;
    const std::uint64_t before = audit::g_allocs.load();
    void* b = ::operator new(24);
    __asm__ volatile("" : : "r"(b) : "memory");
    ::operator delete(b);
    const bool new_seen = audit::g_allocs.load() > before;
    audit::t_hot = false;
    audit::g_allocs.store(0);
    audit::g_frees.store(0);
    return malloc_seen && new_seen;
#else
    return false;
#endif
}

// =====================================================================================================
// 6. Harness state (all static storage -- nothing on the heap)
// =====================================================================================================
namespace {

using namespace animus;
using Msg = Slot<Payload>;
constexpr std::size_t kRingCapacity = std::size_t{1} << 14;   // 16384 slots

struct Config {
    int           producer_cpu    = -1;
    int           consumer_cpu    = -1;
    int           housekeep_cpu   = -1;
    std::uint64_t messages        = 2'000'000;
    double        duration_s      = 0.0;       // > 0 selects time-bounded mode (overrides --messages)
    double        interval_ns     = 1000.0;    // producer pacing; 0 = flood
    std::uint64_t warmup          = 100'000;   // leading messages excluded from the histogram
    double        report_every_s  = 0.0;       // > 0 prints INTERVAL lines in duration mode
};

SpscRing<Msg, kRingCapacity> g_ring;
Histogram g_hist;
Config    g_cfg;
double    g_tsc_hz = 0.0;

struct alignas(kCacheLine) Control {
    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> pin_failed{false};
};
struct alignas(kCacheLine) ProducerStats {
    std::atomic<std::uint64_t> sent{0};
    std::atomic<std::uint64_t> full_stalls{0};
    std::atomic<bool>          done{false};
    std::atomic<int>           cpu_seen{-1};
};
struct alignas(kCacheLine) ConsumerStats {
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> seq_errors{0};
    std::atomic<std::uint64_t> payload_errors{0};
    std::atomic<std::uint64_t> tsc_regressions{0};
    std::atomic<bool>          done{false};
    std::atomic<int>           cpu_seen{-1};
};
Control       g_ctl;
ProducerStats g_prod;
ConsumerStats g_cons;
Histogram     g_floor;
Counts        g_snap_prev, g_snap_now, g_snap_delta;

// ---- small utilities ------------------------------------------------------------------------------
bool pin_self(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
}

double mono_s() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void sleep_ms(long ms) noexcept {
    timespec ts{ms / 1000, (ms % 1000) * 1'000'000L};
    nanosleep(&ts, nullptr);
}

double calibrate_tsc_hz() noexcept {   // median of three 100 ms windows against CLOCK_MONOTONIC_RAW
    double r[3];
    for (double& hz : r) {
        const double t0 = mono_s();
        const std::uint64_t c0 = tsc_plain();
        while (mono_s() - t0 < 0.100) {}
        const std::uint64_t c1 = tsc_plain();
        const double t1 = mono_s();
        hz = static_cast<double>(c1 - c0) / (t1 - t0);
    }
    if (r[0] > r[1]) std::swap(r[0], r[1]);
    if (r[1] > r[2]) std::swap(r[1], r[2]);
    if (r[0] > r[1]) std::swap(r[0], r[1]);
    return r[1];
}

double cyc_to_ns(std::uint64_t c) noexcept { return static_cast<double>(c) * 1e9 / g_tsc_hz; }

int read_int_file(const char* path, int fallback) noexcept {
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return fallback;
    int v = fallback;
    if (std::fscanf(f, "%d", &v) != 1) v = fallback;
    std::fclose(f);
    return v;
}

bool read_line_file(const char* path, char* buf, std::size_t len) noexcept {
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    const bool ok = std::fgets(buf, static_cast<int>(len), f) != nullptr;
    std::fclose(f);
    if (ok) buf[std::strcspn(buf, "\r\n")] = '\0';
    return ok;
}

struct CoreId { int pkg; int core; };
CoreId core_of(int cpu) noexcept {
    char p[160];
    std::snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
    const int pkg = read_int_file(p, -1);
    std::snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
    return {pkg, read_int_file(p, -1)};
}
// 1 = same physical core (SMT siblings), 0 = different cores, -1 = topology unreadable
int smt_siblings(int a, int b) noexcept {
    if (a == b) return 1;
    const CoreId x = core_of(a), y = core_of(b);
    if (x.pkg < 0 || x.core < 0 || y.pkg < 0 || y.core < 0) return -1;
    return (x.pkg == y.pkg && x.core == y.core) ? 1 : 0;
}

// Picks producer/consumer on two different physical cores from the CPUs this process may run on, leaving the
// lowest-numbered CPU for housekeeping (OS tasks, the reporting thread) when there are at least three.
bool auto_select_cpus(Config& c) noexcept {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed) != 0) return false;
    int cpus[CPU_SETSIZE];
    int n = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &allowed)) cpus[n++] = i;
    if (n < 2) return false;
    const int first = (n >= 3) ? 1 : 0;
    int p = -1, q = -1;
    for (int i = first; i < n && q < 0; ++i) {
        if (p < 0) { p = cpus[i]; continue; }
        if (smt_siblings(p, cpus[i]) == 0) q = cpus[i];
    }
    if (q < 0) {   // topology unreadable or every candidate is a sibling: fall back, caller will warn
        p = cpus[first];
        q = cpus[first + 1 < n ? first + 1 : 0];
    }
    if (c.producer_cpu < 0) c.producer_cpu = p;
    if (c.consumer_cpu < 0) c.consumer_cpu = (q != c.producer_cpu) ? q : cpus[0];
    if (c.housekeep_cpu < 0 && n >= 3 && cpus[0] != c.producer_cpu && cpus[0] != c.consumer_cpu)
        c.housekeep_cpu = cpus[0];
    return true;
}

// ---- hot threads ----------------------------------------------------------------------------------
void* producer_main(void*) {
    if (!pin_self(g_cfg.producer_cpu)) g_ctl.pin_failed.store(true);
    g_prod.cpu_seen.store(sched_getcpu());
    g_ctl.ready.fetch_add(1);
    while (!g_ctl.go.load(std::memory_order_acquire)) cpu_relax();

    audit::t_hot = true;
    const std::uint64_t interval = static_cast<std::uint64_t>(g_cfg.interval_ns * g_tsc_hz / 1e9);
    const bool timed = g_cfg.duration_s > 0.0;
    const std::uint64_t t_begin = tsc_plain();
    const std::uint64_t t_end = timed ? t_begin + static_cast<std::uint64_t>(g_cfg.duration_s * g_tsc_hz) : 0;
    std::uint64_t next = t_begin;
    std::uint64_t seq = 0, stalls = 0;

    for (;;) {
        std::uint64_t now = tsc_plain();
        if (timed ? (now >= t_end) : (seq >= g_cfg.messages)) break;
        if (interval != 0) {
            while (now < next) { cpu_relax(); now = tsc_plain(); }
            // If we are more than one interval behind, resync instead of bursting to catch up.
            next = (now > next + interval) ? now + interval : next + interval;
        }
        Msg* s;
        while ((s = g_ring.claim()) == nullptr) { ++stalls; cpu_relax(); }
        s->seq = seq;
        fill_payload(s->payload, seq);
        s->tsc = tsc_stamp();                 // last thing before the release-store
        g_ring.publish();
        ++seq;
        if ((seq & 0xFFFFu) == 0) {           // plain stores keep the main thread's view fresh
            g_prod.sent.store(seq, std::memory_order_relaxed);
            g_prod.full_stalls.store(stalls, std::memory_order_relaxed);
        }
    }
    g_prod.sent.store(seq, std::memory_order_relaxed);
    g_prod.full_stalls.store(stalls, std::memory_order_relaxed);
    audit::t_hot = false;
    g_prod.done.store(true, std::memory_order_release);
    return nullptr;
}

void* consumer_main(void*) {
    if (!pin_self(g_cfg.consumer_cpu)) g_ctl.pin_failed.store(true);
    g_cons.cpu_seen.store(sched_getcpu());
    g_ctl.ready.fetch_add(1);
    while (!g_ctl.go.load(std::memory_order_acquire)) cpu_relax();

    audit::t_hot = true;
    std::uint64_t expected = 0, seen = 0, seq_err = 0, pay_err = 0, regress = 0;
    for (;;) {
        const Msg* s = g_ring.front();
        if (s == nullptr) {
            if (!g_prod.done.load(std::memory_order_acquire)) { cpu_relax(); continue; }
            s = g_ring.front();               // producer finished: drain whatever it published last
            if (s == nullptr) break;
        }
        const std::uint64_t sent = s->tsc;
        const std::uint64_t rx = tsc_rx();    // RDTSCP retires after the slot loads above
        const std::uint64_t seq = s->seq;
        if (seq != expected) ++seq_err;
        expected = seq + 1;
        if (!consume_payload(s->payload, seq)) ++pay_err;
        g_ring.pop();
        if (seen >= g_cfg.warmup) {
            if (rx >= sent) g_hist.record(rx - sent);
            else { ++regress; g_hist.record(0); }
        }
        if ((++seen & 0xFFFFu) == 0) {
            g_cons.received.store(seen, std::memory_order_relaxed);
            g_cons.seq_errors.store(seq_err, std::memory_order_relaxed);
            g_cons.payload_errors.store(pay_err, std::memory_order_relaxed);
            g_cons.tsc_regressions.store(regress, std::memory_order_relaxed);
        }
    }
    g_cons.received.store(seen, std::memory_order_relaxed);
    g_cons.seq_errors.store(seq_err, std::memory_order_relaxed);
    g_cons.payload_errors.store(pay_err, std::memory_order_relaxed);
    g_cons.tsc_regressions.store(regress, std::memory_order_relaxed);
    audit::t_hot = false;
    g_cons.done.store(true, std::memory_order_release);
    return nullptr;
}

// ---- reporting ------------------------------------------------------------------------------------
constexpr double kQuantiles[] = {0.50, 0.90, 0.99, 0.999};
constexpr const char* kQuantileNames[] = {"p50", "p90", "p99", "p99.9"};

void print_interval(double t_s, const Counts& d) {
    std::printf("INTERVAL t=%.0fs n=%llu", t_s, static_cast<unsigned long long>(d.total));
    const char* tag[] = {"p50", "p90", "p99", "p99_9"};
    for (int i = 0; i < 4; ++i)
        std::printf(" %s_ns=%.1f", tag[i], cyc_to_ns(percentile(d, kQuantiles[i])));
    std::printf(" max_ns=%.1f\n", cyc_to_ns(highest(d)));
    std::fflush(stdout);
}

void usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "  --producer-cpu N       CPU for the producer thread   (default: auto, distinct physical core)\n"
        "  --consumer-cpu N       CPU for the consumer thread   (default: auto)\n"
        "  --housekeeping-cpu N   CPU for the main/reporting thread (default: auto / unpinned)\n"
        "  --messages N           messages to send            (default 2000000)\n"
        "  --duration-s S         run for S seconds instead of N messages (soak mode)\n"
        "  --interval-ns X        producer pacing, ns per message (default 1000; 0 = flood, measures queueing)\n"
        "  --warmup N             leading messages excluded from statistics (default 100000)\n"
        "  --report-every-s S     print an INTERVAL line every S seconds (duration mode)\n",
        argv0);
}

bool parse_args(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); return nullptr; }
            return argv[++i];
        };
        const char* v = nullptr;
        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) { usage(argv[0]); std::exit(0); }
        else if (!std::strcmp(a, "--producer-cpu"))     { if (!(v = need(a))) return false; c.producer_cpu = std::atoi(v); }
        else if (!std::strcmp(a, "--consumer-cpu"))     { if (!(v = need(a))) return false; c.consumer_cpu = std::atoi(v); }
        else if (!std::strcmp(a, "--housekeeping-cpu")) { if (!(v = need(a))) return false; c.housekeep_cpu = std::atoi(v); }
        else if (!std::strcmp(a, "--messages"))         { if (!(v = need(a))) return false; c.messages = std::strtoull(v, nullptr, 10); }
        else if (!std::strcmp(a, "--duration-s"))       { if (!(v = need(a))) return false; c.duration_s = std::atof(v); }
        else if (!std::strcmp(a, "--interval-ns"))      { if (!(v = need(a))) return false; c.interval_ns = std::atof(v); }
        else if (!std::strcmp(a, "--warmup"))           { if (!(v = need(a))) return false; c.warmup = std::strtoull(v, nullptr, 10); }
        else if (!std::strcmp(a, "--report-every-s"))   { if (!(v = need(a))) return false; c.report_every_s = std::atof(v); }
        else { std::fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return false; }
    }
    if (c.interval_ns < 0.0 || c.duration_s < 0.0 || c.report_every_s < 0.0) {
        std::fprintf(stderr, "negative values are not allowed\n");
        return false;
    }
    return true;
}

}  // namespace

// =====================================================================================================
// 7. main
// =====================================================================================================
int main(int argc, char** argv) {
    if (!parse_args(argc, argv, g_cfg)) return 2;
    if (!auto_select_cpus(g_cfg)) {
        std::fprintf(stderr, "need at least two CPUs in this process's affinity mask\n");
        return 2;
    }
    if (g_cfg.producer_cpu == g_cfg.consumer_cpu) {
        std::fprintf(stderr, "producer and consumer must be on different CPUs\n");
        return 2;
    }

    // ---- environment checks ----
    unsigned a = 0, b = 0, c = 0, d = 0;
    __get_cpuid(0x80000007u, &a, &b, &c, &d);
    const bool inv_tsc = (d & (1u << 8)) != 0;
    __get_cpuid(0x80000001u, &a, &b, &c, &d);
    const bool has_rdtscp = (d & (1u << 27)) != 0;
    __get_cpuid(1u, &a, &b, &c, &d);
    const bool hypervisor = (c & (1u << 31)) != 0;
    char clk[64] = "unknown";
    read_line_file("/sys/devices/system/clocksource/clocksource0/current_clocksource", clk, sizeof clk);
    const int sib = smt_siblings(g_cfg.producer_cpu, g_cfg.consumer_cpu);

    if (!has_rdtscp) { std::fprintf(stderr, "CPU does not report RDTSCP; cannot take RX stamps\n"); return 2; }

    g_tsc_hz = calibrate_tsc_hz();

    const bool audit_ok = audit_selftest();   // before anything hot, while the counters are still zero

    // ---- warm everything the hot loops will touch ----
    const bool mlocked = (mlockall(MCL_CURRENT | MCL_FUTURE) == 0);
    g_ring.prefault();
    g_hist.reset();
    g_floor.reset();

    // ---- timer floor: cost of (producer stamp + consumer RX stamp) back to back on the consumer CPU ----
    cpu_set_t saved_affinity;
    CPU_ZERO(&saved_affinity);
    pthread_getaffinity_np(pthread_self(), sizeof saved_affinity, &saved_affinity);
    pin_self(g_cfg.consumer_cpu);
    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t t0 = tsc_stamp();
        const std::uint64_t t1 = tsc_rx();
        g_floor.record(t1 - t0);
    }
    snapshot(g_floor, g_snap_now);
    const std::uint64_t floor_min = g_floor.min_v.load();
    const std::uint64_t floor_p50 = percentile(g_snap_now, 0.50);

    if (g_cfg.housekeep_cpu >= 0) pin_self(g_cfg.housekeep_cpu);
    else pthread_setaffinity_np(pthread_self(), sizeof saved_affinity, &saved_affinity);

    // ---- banner ----
    std::printf("AnimusCore sandbox quickstart\n");
    std::printf("  ring        : SPSC, %zu slots x %zu B (%zu cacheline%s/msg), payload %zu B, static storage\n",
                kRingCapacity, sizeof(Msg), sizeof(Msg) / kCacheLine, sizeof(Msg) == kCacheLine ? "" : "s",
                sizeof(Payload));
    std::printf("  placement   : producer CPU %d | consumer CPU %d | housekeeping %s%d | physical cores: %s\n",
                g_cfg.producer_cpu, g_cfg.consumer_cpu, g_cfg.housekeep_cpu < 0 ? "unpinned/" : "",
                g_cfg.housekeep_cpu < 0 ? -1 : g_cfg.housekeep_cpu,
                sib == 0 ? "DISTINCT" : (sib == 1 ? "SAME (SMT siblings!)" : "unknown (sysfs unreadable)"));
    std::printf("  tsc         : invariant=%s rdtscp=%s clocksource=%s freq=%.1f MHz  timer floor min/p50=%llu/%llu cycles (%.1f/%.1f ns)\n",
                inv_tsc ? "yes" : "NO", has_rdtscp ? "yes" : "no", clk, g_tsc_hz / 1e6,
                static_cast<unsigned long long>(floor_min), static_cast<unsigned long long>(floor_p50),
                cyc_to_ns(floor_min), cyc_to_ns(floor_p50));
    std::printf("  host        : hypervisor=%s mlockall=%s alloc-audit=%s\n", hypervisor ? "YES (virtualised)" : "no",
                mlocked ? "ok" : "failed (RLIMIT_MEMLOCK?)", audit_ok ? "armed" : "UNAVAILABLE");
    if (g_cfg.duration_s > 0.0)
        std::printf("  run         : %.0f s, pacing %.0f ns/msg, warm-up %llu msgs discarded\n", g_cfg.duration_s,
                    g_cfg.interval_ns, static_cast<unsigned long long>(g_cfg.warmup));
    else
        std::printf("  run         : %llu msgs, pacing %.0f ns/msg, warm-up %llu msgs discarded\n",
                    static_cast<unsigned long long>(g_cfg.messages), g_cfg.interval_ns,
                    static_cast<unsigned long long>(g_cfg.warmup));
    if (!inv_tsc)       std::printf("  WARNING     : CPU does not advertise an invariant TSC; cross-core stamps are not trustworthy\n");
    if (sib == 1)       std::printf("  WARNING     : producer/consumer share one physical core; this measures a same-core handoff, NOT cross-core\n");
    if (sib == -1)      std::printf("  WARNING     : could not read /sys topology; cannot confirm the CPUs are on different cores\n");
    if (hypervisor)     std::printf("  WARNING     : running under a hypervisor; vCPU pinning and TSC behaviour are not guaranteed\n");
    if (g_cfg.interval_ns == 0.0) std::printf("  WARNING     : flood mode; latencies below include ring queueing delay\n");
    std::fflush(stdout);

    // ---- run ----
    pthread_t tp, tc;
    if (pthread_create(&tc, nullptr, consumer_main, nullptr) != 0 ||
        pthread_create(&tp, nullptr, producer_main, nullptr) != 0) {
        std::fprintf(stderr, "pthread_create failed\n");
        return 2;
    }
    while (g_ctl.ready.load() < 2) sleep_ms(1);
    const double t_start = mono_s();
    g_ctl.go.store(true, std::memory_order_release);

    Counts& prev = g_snap_prev;
    prev.total = 0;
    std::memset(prev.c, 0, sizeof prev.c);
    double next_report = g_cfg.report_every_s;
    while (!g_cons.done.load(std::memory_order_acquire)) {
        sleep_ms(50);
        if (g_cfg.report_every_s > 0.0 && mono_s() - t_start >= next_report) {
            snapshot(g_hist, g_snap_now);
            delta(g_snap_now, prev, g_snap_delta);
            print_interval(mono_s() - t_start, g_snap_delta);
            prev = g_snap_now;
            next_report += g_cfg.report_every_s;
        }
    }
    pthread_join(tp, nullptr);
    pthread_join(tc, nullptr);
    const double elapsed = mono_s() - t_start;

    // ---- results ----
    snapshot(g_hist, g_snap_now);
    const std::uint64_t sent = g_prod.sent.load(), received = g_cons.received.load();
    const std::uint64_t stalls = g_prod.full_stalls.load();
    const std::uint64_t seq_err = g_cons.seq_errors.load(), pay_err = g_cons.payload_errors.load();
    const std::uint64_t regress = g_cons.tsc_regressions.load();
    const std::uint64_t allocs = audit::g_allocs.load(), frees = audit::g_frees.load();
    const std::uint64_t vmax = g_hist.max_v.load(), vmin = g_hist.min_v.load();

    std::printf("\nHandoff latency, producer stamp -> consumer dequeue (one-way), %llu samples, %.2f s\n",
                static_cast<unsigned long long>(g_snap_now.total), elapsed);
    std::printf("  %-6s %12s %12s\n", "", "cycles", "ns");
    std::printf("  %-6s %12llu %12.1f\n", "min", static_cast<unsigned long long>(vmin == ~std::uint64_t{0} ? 0 : vmin),
                cyc_to_ns(vmin == ~std::uint64_t{0} ? 0 : vmin));
    double pct_ns[4];
    std::uint64_t pct_cyc[4];
    for (int i = 0; i < 4; ++i) {
        pct_cyc[i] = percentile(g_snap_now, kQuantiles[i]);
        pct_ns[i] = cyc_to_ns(pct_cyc[i]);
        std::printf("  %-6s %12llu %12.1f\n", kQuantileNames[i], static_cast<unsigned long long>(pct_cyc[i]), pct_ns[i]);
    }
    std::printf("  %-6s %12llu %12.1f\n", "max", static_cast<unsigned long long>(vmax), cyc_to_ns(vmax));
    std::printf("  (percentiles are bucket upper bounds, <= 1.6%% above 127 cycles; timer floor %llu cycles is NOT subtracted)\n",
                static_cast<unsigned long long>(floor_p50));

    std::printf("\nIntegrity   : sent=%llu received=%llu seq_errors=%llu payload_errors=%llu tsc_regressions=%llu ring_full_stalls=%llu\n",
                static_cast<unsigned long long>(sent), static_cast<unsigned long long>(received),
                static_cast<unsigned long long>(seq_err), static_cast<unsigned long long>(pay_err),
                static_cast<unsigned long long>(regress), static_cast<unsigned long long>(stalls));
    std::printf("Hot-path heap: malloc-family calls=%llu free calls=%llu (producer+consumer threads, run window only; interposer self-test %s)\n",
                static_cast<unsigned long long>(allocs), static_cast<unsigned long long>(frees),
                audit_ok ? "ok" : "FAILED");

    const bool pass = audit_ok && allocs == 0 && frees == 0 && seq_err == 0 && pay_err == 0 && regress == 0 &&
                      sent == received && !g_ctl.pin_failed.load() && g_snap_now.total > 0;
    std::printf("VERDICT     : %s%s\n", pass ? "PASS" : "FAIL",
                g_ctl.pin_failed.load() ? " (pthread_setaffinity_np failed)" : "");

    std::printf("RESULT samples=%llu p50_cyc=%llu p90_cyc=%llu p99_cyc=%llu p99_9_cyc=%llu max_cyc=%llu "
                "p50_ns=%.1f p90_ns=%.1f p99_ns=%.1f p99_9_ns=%.1f max_ns=%.1f tsc_mhz=%.1f timer_floor_cyc=%llu "
                "allocs=%llu frees=%llu seq_errors=%llu payload_errors=%llu tsc_regressions=%llu ring_full_stalls=%llu "
                "sent=%llu received=%llu producer_cpu=%d consumer_cpu=%d smt_siblings=%d hypervisor=%d inv_tsc=%d "
                "alloc_audit=%d pin_ok=%d pass=%d\n",
                static_cast<unsigned long long>(g_snap_now.total), static_cast<unsigned long long>(pct_cyc[0]),
                static_cast<unsigned long long>(pct_cyc[1]), static_cast<unsigned long long>(pct_cyc[2]),
                static_cast<unsigned long long>(pct_cyc[3]), static_cast<unsigned long long>(vmax), pct_ns[0],
                pct_ns[1], pct_ns[2], pct_ns[3], cyc_to_ns(vmax), g_tsc_hz / 1e6,
                static_cast<unsigned long long>(floor_p50), static_cast<unsigned long long>(allocs),
                static_cast<unsigned long long>(frees), static_cast<unsigned long long>(seq_err),
                static_cast<unsigned long long>(pay_err), static_cast<unsigned long long>(regress),
                static_cast<unsigned long long>(stalls), static_cast<unsigned long long>(sent),
                static_cast<unsigned long long>(received), g_prod.cpu_seen.load(), g_cons.cpu_seen.load(), sib,
                hypervisor ? 1 : 0, inv_tsc ? 1 : 0, audit_ok ? 1 : 0, g_ctl.pin_failed.load() ? 0 : 1, pass ? 1 : 0);
    return pass ? 0 : 1;
}
