// Sustained multi-producer / multi-consumer saturation soak for ShmRingMpmc<T>.
//
// P producer threads and C consumer threads (default 8 / 8) run flat out against
// ONE named shared-memory ring until a fixed number of frames has been pushed
// and popped (default 64,000,000). Each thread attaches its own mapping of the
// segment, so the cursors and cells are reached through different virtual
// addresses exactly as they would be from separate processes.
//
// A frame is a real animus::ExecutionEvent whose price and checksum are pure
// functions of (producer, sequence) -- tests/frame_check.hpp -- so a consumer
// can tell, with no side channel, whether every record it receives is intact.
//
// What it measures and verifies:
//   Sustained ops/sec      frames delivered per second over the whole run, plus
//                          the per-interval rate (min / median / max) so a run
//                          that starts fast and decays is visible, not averaged away.
//   Packet drop rate       (frames produced - distinct frames delivered) / produced.
//                          Must be exactly 0.0000 %: the ring is lossless, so any
//                          drop is a defect. Exactness comes from per-consumer
//                          bitmaps over (producer, sequence), merged after the run.
//   Sequence corruption    frames whose checksum fails, whose sequence is out of
//                          range, that arrive out of order for a given
//                          (consumer, producer) pair, or that are delivered more
//                          than once across the consumers. Must be exactly 0.
//   Transit latency        ingress-to-egress: the producer stamps the cycle counter
//                          immediately before the push that succeeds, the consumer
//                          stamps it immediately after the pop; the difference is
//                          the time the frame spent in the ring. Both stamps use
//                          the serialized counter read (cycle_timer.hpp), and the
//                          ratio to nanoseconds is measured at start-up. Recorded in
//                          a fixed log-linear histogram (3.1 % worst-case bucket
//                          width), exact min/max, so the hot path never allocates.
//                          NOTE: with the ring saturated the ring is full almost
//                          all the time, so transit is queueing delay -- roughly
//                          capacity / throughput (Little's law) -- and says nothing
//                          about the cost of the handoff itself. --rate runs the same
//                          rig paced below saturation, where transit is the real
//                          producer-to-consumer latency.
//   Heap allocations       operator new/delete (and malloc/free where the build wraps
//                          them) are counted across the entire run and the allocator is
//                          ARMED for the whole window: any allocation from any thread
//                          aborts the process at the call. Threads, ring views, bitmaps
//                          and histograms are all created before the window opens, and
//                          workers park at an exit gate so thread-teardown frees land
//                          after it closes.
//
// Verification overhead is part of what is measured: per frame a consumer
// recomputes the checksum and sets a bit, and with latency on, each side takes a
// serialized counter read. --no-latency --verify count strips both to get a
// throughput figure with only the ring on the hot path.
//
// Output: a report on stdout and, with --json PATH, the same numbers as JSON
// (schema "animus.soak_shm_mpmc.v1") for scripts/generate_verification_report.py.
// Exit code 0 only if every check passes.
//
// Build (Linux):  g++ -std=c++17 -O3 -march=native -DNDEBUG -pthread -I include tests/soak_shm_mpmc.cpp -o soak_shm_mpmc -lrt
//       (MinGW):  g++ -std=c++17 -O3 -march=native -DNDEBUG -static -I include tests/soak_shm_mpmc.cpp -o soak_shm_mpmc.exe
//         (MSVC):  cl /std:c++17 /O2 /EHsc /wd4324 /Iinclude tests\soak_shm_mpmc.cpp
//         (see tests/eval_harness.cpp for why MinGW needs -static here, and for the
//          optional -DANIMUS_WRAP_MALLOC / -Wl,--wrap=... malloc-level counter)
// Usage:  soak_shm_mpmc [--producers P] [--consumers C] [--frames TOTAL] [--capacity N]
//                       [--rate FRAMES_PER_SEC] [--no-latency] [--verify full|count]
//                       [--pin] [--cores a,b,c,...] [--interval-ms N] [--label TEXT] [--json PATH]
#include "../include/animus/shm_ipc.hpp"
#include "../include/animus/cycle_timer.hpp"
#include "../include/animus/thread_affinity.hpp"
#include "alloc_guard.hpp"
#include "frame_check.hpp"
#include "report_util.hpp"

#include <algorithm>
#include <array>
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

using animus::ExecutionEvent;
using animus::sys::ipc::ShmRingMpmc;
using animus_test::arch_name;
using animus_test::compiler_string;
using animus_test::json_str;
using animus_test::os_name;
using animus_test::thousands;
using animus_test::u;
using Ring = ShmRingMpmc<ExecutionEvent>;
namespace timing = animus::timing;

namespace {

// ---------------------------------------------------------------------------
// Fixed-size log-linear latency histogram (HdrHistogram-style): 32 sub-buckets
// per power of two, so any value is recorded to within 1/32 = 3.1 %, across the
// whole uint64 range, in 15 KB, with no allocation per sample.
// ---------------------------------------------------------------------------
inline int msb64(uint64_t v) noexcept {
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse64(&idx, v);
    return static_cast<int>(idx);
#else
    return 63 - __builtin_clzll(v);
#endif
}

struct Histogram {
    static constexpr int kSubBits = 5;
    static constexpr uint64_t kSub = uint64_t{ 1 } << kSubBits;
    static constexpr int kBuckets = (64 - kSubBits + 1) * static_cast<int>(kSub); // 1920

    uint64_t counts[kBuckets] = {};
    uint64_t total = 0;
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;
    double sum = 0.0;

    static int index(uint64_t v) noexcept {
        if (v < kSub) return static_cast<int>(v);
        const int shift = msb64(v) - kSubBits;
        return (shift + 1) * static_cast<int>(kSub) + static_cast<int>((v >> shift) & (kSub - 1));
    }
    // Largest value that maps to bucket `idx`.
    static uint64_t upper(int idx) noexcept {
        if (idx < static_cast<int>(kSub)) return static_cast<uint64_t>(idx);
        const int shift = idx / static_cast<int>(kSub) - 1;
        const uint64_t sub = static_cast<uint64_t>(idx) % kSub;
        return ((kSub + sub + 1) << shift) - 1;
    }

    void record(uint64_t v) noexcept {
        ++counts[index(v)];
        ++total;
        sum += static_cast<double>(v);
        if (v < min) min = v;
        if (v > max) max = v;
    }
    void merge(const Histogram& o) noexcept {
        for (int i = 0; i < kBuckets; ++i) counts[i] += o.counts[i];
        total += o.total;
        sum += o.sum;
        if (o.min < min) min = o.min;
        if (o.max > max) max = o.max;
    }
    // Conservative: the upper edge of the bucket holding the p-th percentile,
    // clamped into the exact observed [min, max].
    uint64_t percentile(double pct) const noexcept {
        if (total == 0) return 0;
        uint64_t target = static_cast<uint64_t>(pct / 100.0 * static_cast<double>(total));
        if (target < 1) target = 1;
        if (target > total) target = total;
        uint64_t cum = 0;
        for (int i = 0; i < kBuckets; ++i) {
            cum += counts[i];
            if (cum >= target) return std::min(max, std::max(min, upper(i)));
        }
        return max;
    }
};

// ---------------------------------------------------------------------------
struct Options {
    uint32_t producers = 8;
    uint32_t consumers = 8;
    uint64_t frames_total = 64'000'000;
    uint64_t capacity = 65'536;
    uint64_t rate = 0; // total frames/s across all producers; 0 = saturate
    bool latency = true;
    bool verify_full = true;
    bool pin = false;
    std::vector<int> cores;
    uint64_t interval_ms = 500;
    std::string label;
    std::string json_path;
};

struct alignas(64) PaddedCounter {
    std::atomic<uint64_t> v{ 0 };
};

struct ConsumerResult {
    uint64_t received = 0;
    uint64_t corrupt = 0;
    uint64_t out_of_range = 0;
    uint64_t order_violations = 0;
    uint64_t negative_transit = 0;
    std::chrono::steady_clock::time_point finished;
};

constexpr size_t kMaxIntervals = 20'000;
struct IntervalSample {
    double t_s;
    uint64_t consumed;
};

struct Shared {
    std::atomic<uint32_t> ready{ 0 };
    std::atomic<bool> go{ false };
    std::atomic<uint32_t> producers_done{ 0 };
    std::atomic<uint32_t> workers_finished{ 0 }; // hot loop over, thread still parked
    std::atomic<bool> exit_gate{ false };        // opened by main once the allocator is disarmed
    std::atomic<uint32_t> open_failures{ 0 };
    std::vector<PaddedCounter> consumed_progress;
};

// Spin with the CPU's pause hint, and yield the core now and then so an
// oversubscribed host (more threads than cores) still makes progress.
inline void backoff(uint64_t& spins) noexcept {
    if ((++spins & 1023u) != 0) animus::cpu_relax();
    else std::this_thread::yield();
}

void pin_if_requested(const Options& opt, size_t slot) {
    if (!opt.pin) return;
    const int core = opt.cores.empty() ? static_cast<int>(slot) : opt.cores[slot % opt.cores.size()];
    animus::sys::pin_current_thread_to_core(static_cast<size_t>(core));
}

void producer_main(const Options& opt, const std::string& name, Shared& sh, uint32_t index, uint64_t frames,
                   double ticks_per_ns) {
    pin_if_requested(opt, index);
    std::unique_ptr<Ring> ring = Ring::open(name.c_str());
    if (!ring) sh.open_failures.fetch_add(1);
    sh.ready.fetch_add(1, std::memory_order_acq_rel);
    while (!sh.go.load(std::memory_order_acquire)) animus::cpu_relax();

    if (ring) {
        // Pacing: each producer offers frames at rate/P per second, on a fixed
        // schedule (a late frame does not push later ones back).
        const bool paced = opt.rate > 0;
        const uint64_t period_ticks = paced
            ? static_cast<uint64_t>(ticks_per_ns * 1e9 * static_cast<double>(opt.producers) / static_cast<double>(opt.rate))
            : 0;
        uint64_t deadline = timing::read_cycles();
        for (uint64_t seq = 0; seq < frames; ++seq) {
            if (paced) {
                deadline += period_ticks;
                while (timing::read_cycles() < deadline) animus::cpu_relax();
            }
            ExecutionEvent e = animus_test::make_frame(index, seq);
            uint64_t spins = 0;
            for (;;) {
                // Stamped immediately before the attempt that can succeed, so a
                // full ring's wait is not counted as transit.
                if (opt.latency) e.dispatch_ts_raw = timing::read_cycles_serialized();
                if (ring->try_push(e)) break;
                backoff(spins);
            }
        }
    }
    sh.producers_done.fetch_add(1, std::memory_order_release);
    sh.workers_finished.fetch_add(1, std::memory_order_acq_rel);
    while (!sh.exit_gate.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void consumer_main(const Options& opt, const std::string& name, Shared& sh, uint32_t index, uint64_t frames_per_producer,
                   std::vector<uint64_t>& bitmap, uint64_t words_per_producer, Histogram& hist, ConsumerResult& out) {
    pin_if_requested(opt, opt.producers + index);
    std::unique_ptr<Ring> ring = Ring::open(name.c_str());
    if (!ring) sh.open_failures.fetch_add(1);
    std::vector<uint64_t> last(opt.producers, UINT64_MAX); // per-producer last sequence; allocated before the window opens
    sh.ready.fetch_add(1, std::memory_order_acq_rel);
    while (!sh.go.load(std::memory_order_acquire)) animus::cpu_relax();

    ConsumerResult me;
    if (ring) {
        ExecutionEvent e;
        uint64_t spins = 0;
        PaddedCounter& progress = sh.consumed_progress[index];
        const bool latency = opt.latency;
        const bool full = opt.verify_full;
        uint64_t* const bits = bitmap.data();
        for (;;) {
            // Read BEFORE the pop: if every producer had already finished, every
            // push is visible to this pop, so a failed pop then means "drained".
            const bool done = sh.producers_done.load(std::memory_order_acquire) == opt.producers;
            if (!ring->try_pop(e)) {
                if (done) break;
                backoff(spins);
                continue;
            }
            spins = 0;
            const uint64_t now = latency ? timing::read_cycles_serialized() : 0;
            ++me.received;
            if (full) {
                if (!animus_test::frame_ok(e, opt.producers)) { ++me.corrupt; continue; }
                if (e.sequence >= frames_per_producer) { ++me.out_of_range; continue; }
                uint64_t& prev = last[e.instrument_id];
                if (prev != UINT64_MAX && e.sequence <= prev) ++me.order_violations;
                prev = e.sequence;
                bits[e.instrument_id * words_per_producer + (e.sequence >> 6)] |= uint64_t{ 1 } << (e.sequence & 63);
            }
            if (latency) {
                if (now >= e.dispatch_ts_raw) hist.record(now - e.dispatch_ts_raw);
                else ++me.negative_transit; // counter skew between cores: counted, never silently dropped
            }
            if ((me.received & 4095u) == 0) progress.v.store(me.received, std::memory_order_relaxed);
        }
        progress.v.store(me.received, std::memory_order_relaxed);
    }
    me.finished = std::chrono::steady_clock::now();
    out = me;
    sh.workers_finished.fetch_add(1, std::memory_order_acq_rel);
    while (!sh.exit_gate.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

bool parse_cores(const std::string& list, std::vector<int>& out) {
    size_t pos = 0;
    while (pos <= list.size()) {
        const size_t comma = list.find(',', pos);
        const std::string tok = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        char* end = nullptr;
        const long v = std::strtol(tok.c_str(), &end, 10);
        if (tok.empty() || !end || *end != '\0' || v < 0) return false;
        out.push_back(static_cast<int>(v));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return !out.empty();
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_val = i + 1 < argc;
        uint64_t v = 0;
        if (a == "--producers" && has_val && animus_test::parse_u64(argv[++i], v)) opt.producers = static_cast<uint32_t>(v);
        else if (a == "--consumers" && has_val && animus_test::parse_u64(argv[++i], v)) opt.consumers = static_cast<uint32_t>(v);
        else if (a == "--frames" && has_val && animus_test::parse_u64(argv[++i], v)) opt.frames_total = v;
        else if (a == "--capacity" && has_val && animus_test::parse_u64(argv[++i], v)) opt.capacity = v;
        else if (a == "--rate" && has_val && animus_test::parse_u64(argv[++i], v)) opt.rate = v;
        else if (a == "--interval-ms" && has_val && animus_test::parse_u64(argv[++i], v)) opt.interval_ms = v ? v : 500;
        else if (a == "--no-latency") opt.latency = false;
        else if (a == "--verify" && has_val) { const std::string m = argv[++i]; if (m == "full") opt.verify_full = true; else if (m == "count") opt.verify_full = false; else { std::fprintf(stderr, "error: --verify full|count\n"); return 2; } }
        else if (a == "--pin") opt.pin = true;
        else if (a == "--cores" && has_val) { if (!parse_cores(argv[++i], opt.cores)) { std::fprintf(stderr, "error: bad --cores list\n"); return 2; } opt.pin = true; }
        else if (a == "--label" && has_val) opt.label = argv[++i];
        else if (a == "--json" && has_val) opt.json_path = argv[++i];
        else {
            std::fprintf(stderr,
                "usage: %s [--producers P] [--consumers C] [--frames TOTAL] [--capacity N] [--rate FRAMES_PER_SEC]\n"
                "          [--no-latency] [--verify full|count] [--pin] [--cores a,b,c] [--interval-ms N] [--label TEXT] [--json PATH]\n", argv[0]);
            return 2;
        }
    }
    if (opt.producers == 0 || opt.consumers == 0 || opt.frames_total < opt.producers) { std::fprintf(stderr, "error: need >= 1 producer, >= 1 consumer, and at least one frame each\n"); return 2; }
    const uint64_t fpp = opt.frames_total / opt.producers;      // frames per producer
    const uint64_t total = fpp * opt.producers;                  // what is actually produced
    const uint64_t words = (fpp + 63) / 64;
    const char* unit = timing::tick_unit_name(timing::kCounterKind);

    std::printf("================================================================================\n");
    std::printf("   ANIMUS CORE -- SHM MPMC SATURATION SOAK  (ShmRingMpmc<ExecutionEvent>)\n");
    std::printf("================================================================================\n");
    std::printf("  Platform             : %s / %s / %s, %u hardware threads\n", os_name(), arch_name(), compiler_string().c_str(),
        std::thread::hardware_concurrency());
    if (!opt.label.empty()) std::printf("  Label                : %s\n", opt.label.c_str());

    const timing::Calibration cal = timing::calibrate();
    if (!cal.valid) { std::fprintf(stderr, "FATAL: counter calibration failed\n"); return 1; }
    std::printf("  Clock source         : %s, invariant: %s\n", timing::counter_kind_name(timing::kCounterKind),
        timing::counter_is_invariant() ? "yes" : "NO");
    std::printf("  Calibration          : %.5f %s/ns (median of %u windows, spread %.0f ppm; 1 tick = %.3f ns)\n", cal.ticks_per_ns, unit,
        cal.trials, cal.spread_ppm, cal.resolution_ns());

    // The counters must be live, or "0 allocations" below would mean nothing.
    {
        const auto a0 = animus_test::snapshot();
        int* p = new int(1);
        animus_test::escape(p); // else clang may legally elide this new/delete pair, and the check would fail for the wrong reason
        const auto a1 = animus_test::snapshot();
        delete p;
        if (a1.new_calls != a0.new_calls + 1) { std::fprintf(stderr, "FATAL: allocation counters are not live\n"); return 1; }
    }

    // ---- setup: everything that allocates, before the armed window ----
    const std::string name = "animus_soak_mpmc_" + std::to_string(animus::sys::lifecycle::current_process_id());
    Ring::unlink(name.c_str());
    std::unique_ptr<Ring> owner = Ring::create(name.c_str(), static_cast<size_t>(opt.capacity));
    if (!owner) { std::fprintf(stderr, "FATAL: ShmRingMpmc::create('%s', %llu) failed\n", name.c_str(), u(opt.capacity)); return 1; }
    const uint64_t ring_bytes = sizeof(animus::sys::ipc::MpmcRingHeader) + owner->capacity() * owner->stride();

    std::printf("  Primitive            : ShmRingMpmc<ExecutionEvent>, %s cells x %zu B (%.1f MiB), cursors on separate %zu-B lines\n",
        thousands(owner->capacity()).c_str(), owner->stride(), static_cast<double>(ring_bytes) / (1024.0 * 1024.0),
        static_cast<size_t>(animus::cache_line_size));
    std::printf("  Producers/consumers  : %u / %u%s\n", opt.producers, opt.consumers, opt.pin ? " (pinned)" : "");
    std::printf("  Frames               : %s total = %u x %s  |  offered load: %s  |  latency stamps: %s  |  verification: %s\n",
        thousands(total).c_str(), opt.producers, thousands(fpp).c_str(),
        opt.rate ? (thousands(opt.rate) + " frames/s (paced)").c_str() : "saturation (unpaced)",
        opt.latency ? "on" : "off", opt.verify_full ? "full (checksum + exactly-once bitmap)" : "count only");

    Shared sh;
    sh.consumed_progress = std::vector<PaddedCounter>(opt.consumers);
    std::vector<std::vector<uint64_t>> bitmaps(opt.consumers);
    std::vector<std::unique_ptr<Histogram>> hists;
    for (uint32_t c = 0; c < opt.consumers; ++c) {
        if (opt.verify_full) bitmaps[c].assign(static_cast<size_t>(opt.producers) * static_cast<size_t>(words), 0);
        hists.emplace_back(new Histogram());
    }
    std::vector<ConsumerResult> results(opt.consumers);
    std::vector<IntervalSample> intervals;
    intervals.reserve(kMaxIntervals);

    std::vector<std::thread> threads;
    threads.reserve(opt.producers + opt.consumers);
    for (uint32_t c = 0; c < opt.consumers; ++c) {
        threads.emplace_back(consumer_main, std::cref(opt), std::cref(name), std::ref(sh), c, fpp, std::ref(bitmaps[c]), words,
                             std::ref(*hists[c]), std::ref(results[c]));
    }
    for (uint32_t p = 0; p < opt.producers; ++p) {
        threads.emplace_back(producer_main, std::cref(opt), std::cref(name), std::ref(sh), p, fpp, cal.ticks_per_ns);
    }
    while (sh.ready.load(std::memory_order_acquire) < opt.producers + opt.consumers) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (sh.open_failures.load() != 0) {
        std::fprintf(stderr, "FATAL: %u worker(s) could not attach to the ring\n", sh.open_failures.load());
        sh.go.store(true); sh.exit_gate.store(true);
        for (auto& t : threads) t.join();
        Ring::unlink(name.c_str());
        return 1;
    }

    // ---- the armed window ----
    std::printf("  Running...\n");
    const auto alloc_before = animus_test::snapshot();
    animus_test::arm();
    const auto t0 = std::chrono::steady_clock::now();
    sh.go.store(true, std::memory_order_release);

    const uint32_t workers = opt.producers + opt.consumers;
    intervals.push_back({ 0.0, 0 }); // anchor, so the first interval is a full one
    while (sh.workers_finished.load(std::memory_order_acquire) < workers) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));
        if (intervals.size() < kMaxIntervals) {
            uint64_t sum = 0;
            for (const auto& pc : sh.consumed_progress) sum += pc.v.load(std::memory_order_relaxed);
            intervals.push_back({ std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), sum });
        }
    }
    animus_test::disarm();
    const auto alloc_delta = animus_test::delta(alloc_before, animus_test::snapshot());
    sh.exit_gate.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    // ---- verification ----
    auto last_finish = t0;
    uint64_t received = 0, corrupt = 0, out_of_range = 0, order_violations = 0, negative_transit = 0;
    for (const auto& r : results) {
        received += r.received; corrupt += r.corrupt; out_of_range += r.out_of_range;
        order_violations += r.order_violations; negative_transit += r.negative_transit;
        if (r.finished > last_finish) last_finish = r.finished;
    }
    const double wall_s = std::chrono::duration<double>(last_finish - t0).count();

    uint64_t covered = 0, duplicates = 0;
    if (opt.verify_full) {
        for (uint32_t p = 0; p < opt.producers; ++p) {
            for (uint64_t w = 0; w < words; ++w) {
                uint64_t acc = 0;
                for (uint32_t c = 0; c < opt.consumers; ++c) {
                    const uint64_t word = bitmaps[c][static_cast<size_t>(p) * static_cast<size_t>(words) + static_cast<size_t>(w)];
                    duplicates += animus_test::popcount64(acc & word);
                    acc |= word;
                }
                covered += animus_test::popcount64(acc);
            }
        }
    }
    const uint64_t delivered_distinct = opt.verify_full ? covered : received;
    const uint64_t dropped = total > delivered_distinct ? total - delivered_distinct : 0;
    const double drop_pct = total ? static_cast<double>(dropped) / static_cast<double>(total) * 100.0 : 0.0;
    const uint64_t sequence_corruption = corrupt + out_of_range + order_violations + duplicates;
    const bool counts_match = received == total;

    Histogram merged;
    for (const auto& h : hists) merged.merge(*h);

    // Per-interval rate. The last sample is taken after the run has ended, so its
    // time is clamped to the true finish; only full-length intervals are used (a
    // short tail interval is mostly the drain and would add noise, not information).
    std::vector<double> rates;
    const double nominal_s = static_cast<double>(opt.interval_ms) * 1e-3;
    for (size_t i = 1; i < intervals.size(); ++i) {
        const double t1 = std::min(intervals[i].t_s, wall_s);
        const double dt = t1 - intervals[i - 1].t_s;
        if (dt >= 0.9 * nominal_s) rates.push_back(static_cast<double>(intervals[i].consumed - intervals[i - 1].consumed) / dt);
    }
    const double rate_min = rates.empty() ? 0.0 : *std::min_element(rates.begin(), rates.end());
    const double rate_max = rates.empty() ? 0.0 : *std::max_element(rates.begin(), rates.end());
    const double rate_med = median(rates);
    const double ops_per_sec = wall_s > 0 ? static_cast<double>(received) / wall_s : 0.0;

    const bool zero_alloc = alloc_delta.total() == 0;
    const bool pass = counts_match && dropped == 0 && sequence_corruption == 0 && zero_alloc &&
                      (opt.verify_full ? covered == total : true);

    // ---- report ----
    std::printf("\n  Throughput & Integrity\n  ------------------------------------------------------------------------------\n");
    std::printf("  Frames produced          : %20s\n", thousands(total).c_str());
    std::printf("  Frames consumed          : %20s\n", thousands(received).c_str());
    std::printf("  Wall time                : %20.3f s\n", wall_s);
    std::printf("  Sustained ops/sec        : %20s  (frames delivered per second, whole run)\n", thousands(static_cast<uint64_t>(ops_per_sec)).c_str());
    std::printf("  Interval ops/sec         : min %s | median %s | max %s   (%zu x %llu ms intervals)\n",
        thousands(static_cast<uint64_t>(rate_min)).c_str(), thousands(static_cast<uint64_t>(rate_med)).c_str(),
        thousands(static_cast<uint64_t>(rate_max)).c_str(), rates.size(), u(opt.interval_ms));
    std::printf("  Packet drop rate         : %20.4f %%   (%s dropped)\n", drop_pct, thousands(dropped).c_str());
    if (opt.verify_full) {
        std::printf("  Sequence corruption      : %20s frames  (checksum %s, range %s, order %s, duplicate %s)\n",
            thousands(sequence_corruption).c_str(), thousands(corrupt).c_str(), thousands(out_of_range).c_str(),
            thousands(order_violations).c_str(), thousands(duplicates).c_str());
        std::printf("  Exactly-once coverage    : %s of %s (producer, sequence) pairs delivered once\n",
            thousands(covered).c_str(), thousands(total).c_str());
    } else {
        std::printf("  Sequence corruption      : not checked (--verify count)\n");
    }
    std::printf("\n  Memory Safety (allocator armed for the whole window)\n  ------------------------------------------------------------------------------\n");
    std::printf("  operator new / delete    : %s / %s\n", thousands(alloc_delta.new_calls).c_str(), thousands(alloc_delta.delete_calls).c_str());
    std::printf("  malloc / free            : %s / %s\n",
        animus_test::malloc_instrumented() ? thousands(alloc_delta.malloc_calls).c_str() : "n/a (not instrumented on this toolchain)",
        animus_test::malloc_instrumented() ? thousands(alloc_delta.free_calls).c_str() : "n/a");
    if (opt.latency) {
        std::printf("\n  Latency Profile (ns) -- Ingress to Egress Transit  [%s]\n  ------------------------------------------------------------------------------\n",
            opt.rate ? "paced, below saturation" : "saturated: ring full, so this is queueing delay (~capacity / throughput)");
        const struct { const char* label; double pct; } rows[] = { { "p50 (median)", 50.0 }, { "p90", 90.0 }, { "p99", 99.0 }, { "p99.9", 99.9 }, { "p99.99", 99.99 } };
        std::printf("  Min                      : %20.1f ns\n", cal.ns(merged.total ? merged.min : 0));
        for (const auto& row : rows) std::printf("  %-24s : %20.1f ns\n", row.label, cal.ns(merged.percentile(row.pct)));
        std::printf("  Max (tail jitter)        : %20.1f ns\n", cal.ns(merged.max));
        std::printf("  Mean                     : %20.1f ns   (%s samples; bucket width <= 3.1%%; %s negative-skew samples)\n",
            merged.total ? cal.ns(1) * merged.sum / static_cast<double>(merged.total) : 0.0, thousands(merged.total).c_str(), thousands(negative_transit).c_str());
    }
    std::printf("================================================================================\n");
    std::printf("  %s\n", pass ? "VERIFIED: 0 dropped frames, 0 corrupted/duplicated/out-of-order frames, 0 hot-path heap allocations."
                               : "FAILED -- see the figures above.");
    std::printf("================================================================================\n");

    if (!opt.json_path.empty()) {
        FILE* f = animus_test::open_for_write(opt.json_path);
        if (!f) { std::fprintf(stderr, "warning: cannot write %s\n", opt.json_path.c_str()); }
        else {
            std::fprintf(f, "{\n  \"schema\": \"animus.soak_shm_mpmc.v1\",\n  \"label\": "); json_str(f, opt.label);
            std::fprintf(f, ",\n  \"platform\": {\"os\": "); json_str(f, os_name());
            std::fprintf(f, ", \"arch\": "); json_str(f, arch_name());
            std::fprintf(f, ", \"compiler\": "); json_str(f, compiler_string());
            std::fprintf(f, ", \"cache_line_bytes\": %zu, \"hardware_threads\": %u, \"pinned\": %s},\n",
                static_cast<size_t>(animus::cache_line_size), std::thread::hardware_concurrency(), opt.pin ? "true" : "false");
            std::fprintf(f, "  \"timing\": {\"counter\": "); json_str(f, timing::counter_kind_name(timing::kCounterKind));
            std::fprintf(f, ", \"unit\": "); json_str(f, unit);
            std::fprintf(f, ", \"invariant\": %s, \"ticks_per_ns\": %.6f, \"spread_ppm\": %.1f, \"resolution_ns\": %.4f},\n",
                timing::counter_is_invariant() ? "true" : "false", cal.ticks_per_ns, cal.spread_ppm, cal.resolution_ns());
            std::fprintf(f, "  \"config\": {\"producers\": %u, \"consumers\": %u, \"frames_per_producer\": %llu, \"total_frames\": %llu, "
                            "\"ring_capacity\": %llu, \"cell_bytes\": %zu, \"ring_bytes\": %llu, \"rate_limit_per_s\": %llu, "
                            "\"latency_instrumented\": %s, \"verify\": \"%s\", \"malloc_instrumented\": %s},\n",
                opt.producers, opt.consumers, u(fpp), u(total), u(owner->capacity()), owner->stride(), u(ring_bytes), u(opt.rate),
                opt.latency ? "true" : "false", opt.verify_full ? "full" : "count", animus_test::malloc_instrumented() ? "true" : "false");
            std::fprintf(f, "  \"results\": {\n    \"wall_seconds\": %.6f, \"frames_produced\": %llu, \"frames_consumed\": %llu, "
                            "\"sustained_ops_per_sec\": %.1f,\n    \"interval_ops_per_sec\": {\"samples\": %zu, \"interval_ms\": %llu, \"min\": %.1f, \"median\": %.1f, \"max\": %.1f, \"series\": [",
                wall_s, u(total), u(received), ops_per_sec, rates.size(), u(opt.interval_ms), rate_min, rate_med, rate_max);
            // The per-interval series, so a reader can see a ramp or a decay instead of trusting an average.
            for (size_t i = 0; i < rates.size(); ++i) std::fprintf(f, "%s%.0f", i ? ", " : "", rates[i]);
            std::fprintf(f, "]},\n");
            std::fprintf(f, "    \"dropped_frames\": %llu, \"drop_rate_percent\": %.6f, \"duplicates\": %llu, \"missing\": %llu, "
                            "\"corrupt_frames\": %llu, \"order_violations\": %llu, \"out_of_range\": %llu, \"sequence_corruption\": %llu, "
                            "\"exactly_once_pairs\": %llu, \"verified_exactly_once\": %s,\n",
                u(dropped), drop_pct, u(duplicates), u(dropped), u(corrupt), u(order_violations), u(out_of_range), u(sequence_corruption),
                u(covered), opt.verify_full ? "true" : "false");
            if (opt.latency) {
                std::fprintf(f, "    \"transit_ns\": {\"samples\": %llu, \"min\": %.2f, \"p50\": %.2f, \"p90\": %.2f, \"p99\": %.2f, \"p999\": %.2f, "
                                "\"p9999\": %.2f, \"max\": %.2f, \"mean\": %.2f, \"negative_skew_samples\": %llu, \"bucket_width_pct\": 3.1, \"saturated\": %s,\n"
                                "      \"histogram_ns\": [",
                    u(merged.total), cal.ns(merged.min), cal.ns(merged.percentile(50)), cal.ns(merged.percentile(90)),
                    cal.ns(merged.percentile(99)), cal.ns(merged.percentile(99.9)), cal.ns(merged.percentile(99.99)), cal.ns(merged.max),
                    merged.total ? cal.ns(1) * merged.sum / static_cast<double>(merged.total) : 0.0, u(negative_transit),
                    opt.rate ? "false" : "true");
                // Sparse histogram, [bucket upper edge in ns, count] for every non-empty bucket: enough to redraw the
                // whole latency distribution (a CDF) without shipping 64 million samples.
                bool first_bucket = true;
                for (int i = 0; i < Histogram::kBuckets; ++i) {
                    if (merged.counts[i] == 0) continue;
                    std::fprintf(f, "%s[%.1f, %llu]", first_bucket ? "" : ", ", cal.ns(Histogram::upper(i)), u(merged.counts[i]));
                    first_bucket = false;
                }
                std::fprintf(f, "]},\n");
            }
            std::fprintf(f, "    \"allocations\": {\"operator_new\": %llu, \"operator_delete\": %llu, \"malloc\": %llu, \"free\": %llu}, \"zero_alloc\": %s,\n",
                u(alloc_delta.new_calls), u(alloc_delta.delete_calls), u(alloc_delta.malloc_calls), u(alloc_delta.free_calls), zero_alloc ? "true" : "false");
            std::fprintf(f, "    \"per_consumer_frames\": [");
            for (size_t i = 0; i < results.size(); ++i) std::fprintf(f, "%s%llu", i ? ", " : "", u(results[i].received));
            std::fprintf(f, "]\n  },\n  \"verdict\": \"%s\"\n}\n", pass ? "PASS" : "FAIL");
            std::fclose(f);
        }
    }

    owner.reset();
    Ring::unlink(name.c_str());
    return pass ? 0 : 1;
}
