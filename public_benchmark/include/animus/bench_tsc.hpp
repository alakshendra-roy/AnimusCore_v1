// SPDX-License-Identifier: Apache-2.0
#pragma once
// bench_tsc.hpp -- invariant-TSC latency profiler: pinned thread, serialized reads, percentiles.
//
// Measures the latency of ONE invocation of a callable, in TSC ticks and in nanoseconds:
//
//     animus::bench::ProfilerConfig cfg;              // 1M samples, 100k warmup, pin to the CPU we are on
//     animus::bench::Profiler prof(cfg);              // setup (heap allowed): pin, calibrate, allocate
//     if (!prof.ok()) { std::fprintf(stderr, "%s\n", prof.error()); return 1; }
//     prof.run([&] { ring.push(x); ring.pop(y); });   // timed loop: no allocation, no syscall, no I/O
//     animus::bench::print_report(prof.finish("push+pop"));
//
// Build: x86-64 Linux, C++17; no dependency outside libc/libstdc++.
//     clang++ -std=c++17 -O3 -march=native -Wall -Wextra -Werror -pedantic -pthread -I include your_file.cpp
// Verified by src/bench_ring_tsc.cpp (self-check against a known 1 us spin) and by
// src/test_no_alloc.cpp, which runs Profiler::run() under the zero-allocation guard.
//
// What one sample is.   t0 = lfence; rdtsc; lfence      <- the timed code starts after this
//                       t1 = rdtscp; lfence             <- and has fully executed before this
//                       sample = t1 - t0
// The leading lfence keeps earlier work from leaking into the interval, the trailing lfence keeps the
// timed code from starting before the counter is read. rdtscp waits for all earlier instructions to
// execute, and the lfence after it keeps later instructions from starting before it. (Intel's
// white-paper recipe, with lfence in place of cpuid, which costs hundreds of cycles and under a
// hypervisor traps to the host.) The end read is rdtscp because it returns IA32_TSC_AUX with the stamp,
// which on Linux encodes the CPU: that is how a CPU change between samples is detected for free.
// The LFENCE-serializes-rdtsc guarantee is architectural on Intel; AMD parts have it by default or via
// an OS-set MSR depending on the generation. Confirm it on the exact part you deploy on.
//
// What the number means.  TSC ticks are REFERENCE cycles: an invariant TSC ticks at a fixed rate
// whatever the core's P-/C-state, so under turbo a tick is shorter than a core clock cycle. The ns
// columns are what is comparable across machines. ticks/ns is measured at run time against
// clock_gettime(CLOCK_MONOTONIC_RAW) (never NTP-slewed, unlike CLOCK_MONOTONIC / steady_clock), as the
// median of several spin windows with the clock read bracketing the TSC read, and the spread across
// windows is reported so a noisy host says so.
//
// Percentiles are nearest-rank on the sorted samples: p is the smallest sample such that at least p%
// of all samples are <= it. p99.9 is only resolved from 1000 samples up (below that it IS the max)
// and the report says so.
//
// Timer cost. An empty begin/end pair costs tens of ticks. Its median is measured at setup and
// reported; the "net" columns subtract it from each percentile, floored at 0. A net value near 0 means
// "below the resolution of this method", not "free". Batch the operation if you need a finer answer.
//
// Pinning.  The constructing thread is pinned with pthread_setaffinity_np, verified with
// sched_getcpu() and again after the run, and its old mask is restored on destruction. Construct,
// run() and destroy the Profiler on the same thread. On a hybrid CPU (P + E cores) the answer depends
// on which kind of core you pin to, and in a VM the guest-visible topology need not match the host.
// To measure cross-core latency pin each thread to a different PHYSICAL core, not an SMT sibling
// (cat /sys/devices/system/cpu/cpuN/topology/thread_siblings_list): siblings share an L1 and give
// hyperthread-local numbers.
//
// What the callable must do.  fn() is invoked once per sample and again for warmup. It must have an
// observable effect or consume loop-varying state: a pure function of constants can be hoisted out of
// the loop by the optimizer and then measures nothing. A returned value is kept alive for you.
// Do not allocate, print, or make syscalls inside it unless that is what you are measuring.
#if !defined(__x86_64__)
    #error "bench_tsc.hpp reads the x86-64 time-stamp counter (rdtsc/rdtscp/cpuid); no other target is supported"
#endif
#if !defined(__linux__)
    #error "bench_tsc.hpp pins with pthread_setaffinity_np and calibrates against clock_gettime(CLOCK_MONOTONIC_RAW): Linux only"
#endif

#include <algorithm>
#include <cerrno>
#include <cpuid.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <type_traits>
#include <vector>

namespace animus {
namespace bench {

// ---- CPU features ---------------------------------------------------------------------------------

struct CpuFeatures {
    bool invariant_tsc = false; // CPUID.80000007H:EDX[8]
    bool rdtscp = false;        // CPUID.80000001H:EDX[27]
};

// Leaves above the CPU's maximum extended leaf make __get_cpuid return 0, so an absent leaf reads as
// "feature absent". Hypervisors may mask these bits even when the host has them.
inline CpuFeatures detect_cpu_features() noexcept {
    CpuFeatures f;
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) f.rdtscp = (d & (1u << 27)) != 0;
    if (__get_cpuid(0x80000007u, &a, &b, &c, &d)) f.invariant_tsc = (d & (1u << 8)) != 0;
    return f;
}

// ---- Clock and counter reads ----------------------------------------------------------------------

// CLOCK_MONOTONIC_RAW through the vDSO: no syscall, and never rate-adjusted by NTP.
inline std::uint64_t mono_raw_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

// Start of a timed region. One asm statement, so the compiler cannot slot anything between the fences.
inline std::uint64_t tsc_begin() noexcept {
    std::uint32_t lo, hi;
    asm volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi) : : "memory");
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

// End of a timed region. `aux` receives IA32_TSC_AUX (Linux: node << 12 | cpu). Written as asm rather
// than __rdtscp so it builds without -mrdtscp.
inline std::uint64_t tsc_end(std::uint32_t& aux) noexcept {
    std::uint32_t lo, hi, c;
    asm volatile("rdtscp\n\tlfence" : "=a"(lo), "=d"(hi), "=c"(c) : : "memory");
    aux = c;
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

// ---- TSC -> ns calibration ------------------------------------------------------------------------

struct TscCalibration {
    bool valid = false;
    double ticks_per_ns = 0.0;     // median across windows; numerically the TSC frequency in GHz
    double min_ticks_per_ns = 0.0;
    double max_ticks_per_ns = 0.0;
    double spread_ppm = 0.0;       // (max - min) / median; a wide spread means a noisy host
    unsigned windows = 0;          // windows that produced a usable ratio
    std::uint64_t window_ns = 0;

    double ns(std::uint64_t ticks) const noexcept {
        return valid && ticks_per_ns > 0.0 ? static_cast<double>(ticks) / ticks_per_ns : 0.0;
    }
};

// Races the TSC against CLOCK_MONOTONIC_RAW over `trials` busy-wait windows. Each end of a window reads
// the clock, then the TSC, then the clock again, and uses the midpoint of the two clock reads as the
// time of the TSC read, so the cost of the clock read cancels. Spins on the calling thread, which
// keeps the core awake and (once pinned) un-migrated; this is the "calibrate during warmup" step.
inline TscCalibration calibrate_tsc(unsigned trials = 5, std::uint64_t window_ns = 50000000ull) noexcept {
    constexpr unsigned kMaxTrials = 15;
    if (trials == 0) trials = 1;
    if (trials > kMaxTrials) trials = kMaxTrials;

    double ratio[kMaxTrials];
    unsigned good = 0;
    for (unsigned i = 0; i < trials; ++i) {
        const std::uint64_t a0 = mono_raw_ns();
        const std::uint64_t c0 = tsc_begin();
        const std::uint64_t b0 = mono_raw_ns();
        while (mono_raw_ns() - b0 < window_ns) { /* spin */ }
        const std::uint64_t a1 = mono_raw_ns();
        const std::uint64_t c1 = tsc_begin();
        const std::uint64_t b1 = mono_raw_ns();
        if (c1 <= c0 || a1 <= a0 || b1 <= b0) continue; // counter or clock went backwards: discard the window
        // midpoint(1) - midpoint(0) == ((a1 - a0) + (b1 - b0)) / 2, kept in small unsigned differences so
        // nothing is lost to double rounding on a machine that has been up for months.
        const double dt_ns = 0.5 * (static_cast<double>(a1 - a0) + static_cast<double>(b1 - b0));
        ratio[good++] = static_cast<double>(c1 - c0) / dt_ns;
    }

    TscCalibration cal;
    cal.windows = good;
    cal.window_ns = window_ns;
    if (good == 0) return cal;
    // Insertion sort: at most 15 elements. (std::sort here draws a false -Warray-bounds from GCC 15
    // at -O2, which -Werror turns into a build failure.)
    for (unsigned i = 1; i < good; ++i) {
        const double v = ratio[i];
        unsigned j = i;
        while (j > 0 && ratio[j - 1] > v) { ratio[j] = ratio[j - 1]; --j; }
        ratio[j] = v;
    }
    cal.min_ticks_per_ns = ratio[0];
    cal.max_ticks_per_ns = ratio[good - 1];
    cal.ticks_per_ns = (good % 2 == 1) ? ratio[good / 2] : 0.5 * (ratio[good / 2 - 1] + ratio[good / 2]);
    cal.spread_ppm = cal.ticks_per_ns > 0.0 ? (cal.max_ticks_per_ns - cal.min_ticks_per_ns) / cal.ticks_per_ns * 1e6 : 0.0;
    cal.valid = cal.ticks_per_ns > 0.0;
    return cal;
}

// ---- Core pinning ---------------------------------------------------------------------------------

// Pins the CALLING thread to one logical CPU and restores its previous affinity mask on release or
// destruction. pin() succeeds only if the kernel accepted the mask AND sched_getcpu() confirms the
// thread is now on that CPU.
class ScopedPin {
public:
    static constexpr int kCurrentCpu = -1; // pin to the CPU the thread is running on right now
    static constexpr int kNoPin = -2;      // Profiler: leave the affinity alone

    ScopedPin() = default;
    ScopedPin(const ScopedPin&) = delete;
    ScopedPin& operator=(const ScopedPin&) = delete;
    ~ScopedPin() { release(); }

    bool pin(int cpu) noexcept {
        release();
        err_ = 0;
        if (cpu == kCurrentCpu) {
            cpu = ::sched_getcpu();
            if (cpu < 0) { err_ = errno != 0 ? errno : ENOSYS; return false; }
        }
        if (cpu < 0 || cpu >= CPU_SETSIZE) { err_ = EINVAL; return false; }

        const pthread_t self = ::pthread_self();
        cpu_set_t previous;
        CPU_ZERO(&previous);
        if (const int rc = ::pthread_getaffinity_np(self, sizeof previous, &previous)) { err_ = rc; return false; }
        cpu_set_t only;
        CPU_ZERO(&only);
        CPU_SET(cpu, &only);
        if (const int rc = ::pthread_setaffinity_np(self, sizeof only, &only)) { err_ = rc; return false; }

        saved_ = previous;
        owner_ = self;
        cpu_ = cpu;
        active_ = true;
        if (::sched_getcpu() != cpu) { release(); err_ = -1; return false; }
        return true;
    }

    void release() noexcept {
        if (!active_) return;
        ::pthread_setaffinity_np(owner_, sizeof saved_, &saved_);
        active_ = false;
    }

    bool active() const noexcept { return active_; }
    int cpu() const noexcept { return cpu_; }
    // 0: ok.  > 0: the errno-style code a pthread/sched call returned.  -1: the mask was accepted but
    // the thread is not running on the requested CPU.
    int error() const noexcept { return err_; }

private:
    cpu_set_t saved_{};
    pthread_t owner_{};
    int cpu_ = -1;
    int err_ = 0;
    bool active_ = false;
};

// ---- Configuration and result ---------------------------------------------------------------------

struct ProfilerConfig {
    std::size_t samples = 1000000;                   // timed invocations kept for the percentiles
    std::size_t warmup = 100000;                     // untimed invocations before the timed loop
    int cpu = ScopedPin::kCurrentCpu;                // a CPU id, kCurrentCpu, or kNoPin
    bool require_invariant_tsc = true;               // refuse to run if CPUID does not report one
    unsigned calibration_windows = 5;
    std::uint64_t calibration_window_ns = 50000000ull;
    std::size_t overhead_samples = 100000;           // empty begin/end pairs used to size the timer cost
};

struct LatencyReport {
    const char* label = "";

    // Environment.
    bool invariant_tsc = false;
    bool rdtscp = false;
    char clocksource[32] = {};      // kernel clocksource at setup, e.g. "tsc"
    bool pinned = false;
    int cpu = -1;                   // the pinned CPU, or the CPU the thread ended on if not pinned
    bool pin_held = false;          // pinned AND still on that CPU after the run
    std::uint64_t cpu_changes = 0;  // samples whose IA32_TSC_AUX differed from the previous sample's
    TscCalibration calibration;
    std::uint64_t overhead_ticks = 0;

    // Distribution of RAW per-invocation ticks (the timer's own cost is still inside these).
    std::size_t n = 0;
    std::uint64_t min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
    double mean = 0.0;
    bool p999_resolved = false;     // n >= 1000: at least one sample lies above the p99.9 rank

    std::uint64_t net(std::uint64_t raw_ticks) const noexcept { return raw_ticks > overhead_ticks ? raw_ticks - overhead_ticks : 0; }
    double ns(std::uint64_t ticks) const noexcept { return calibration.ns(ticks); }
};

namespace detail {

// Nearest rank, 1-based: ceil(n * permille / 1000), clamped to [1, n].
inline std::uint64_t rank_value(const std::uint64_t* sorted, std::size_t n, std::uint64_t permille) noexcept {
    std::uint64_t rank = (static_cast<std::uint64_t>(n) * permille + 999u) / 1000u;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}

inline void read_clocksource(char (&out)[32]) noexcept {
    std::snprintf(out, sizeof out, "unknown");
    if (std::FILE* f = std::fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r")) {
        if (std::fgets(out, sizeof out, f)) out[std::strcspn(out, "\r\n")] = '\0';
        std::fclose(f);
    }
}

// Median cost of an empty begin/end pair. Allocates: setup only.
inline std::uint64_t measure_timer_ticks(std::size_t samples) {
    std::vector<std::uint64_t> v(samples ? samples : 1);
    std::uint32_t aux = 0;
    for (int i = 0; i < 1000; ++i) { (void)tsc_begin(); (void)tsc_end(aux); } // warm the code path
    for (std::uint64_t& x : v) {
        const std::uint64_t t0 = tsc_begin();
        const std::uint64_t t1 = tsc_end(aux);
        x = t1 - t0;
    }
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

template <class Fn>
inline void invoke(Fn& fn) {
    if constexpr (std::is_void_v<decltype(fn())>) {
        fn();
    } else {
        auto&& result = fn();
        asm volatile("" : : "g"(&result) : "memory"); // the value is observed: the call cannot be dropped
    }
}

} // namespace detail

// ---- The profiler ---------------------------------------------------------------------------------

class Profiler {
public:
    // Setup. Everything that allocates, pins, or spins for calibration happens here, in this order:
    // check the CPU, pin, allocate and pre-fault the sample buffer, calibrate TSC -> ns (this is the
    // warmup), size the timer cost. On failure ok() is false and error() says why.
    explicit Profiler(const ProfilerConfig& cfg = ProfilerConfig()) : cfg_(cfg) {
        const CpuFeatures f = detect_cpu_features();
        features_ = f;
        if (cfg_.samples == 0) { err_ = "ProfilerConfig::samples must be > 0"; return; }
        if (!f.rdtscp) { err_ = "CPU does not report RDTSCP (CPUID.80000001H:EDX[27])"; return; }
        if (cfg_.require_invariant_tsc && !f.invariant_tsc) {
            err_ = "CPU does not report an invariant TSC (CPUID.80000007H:EDX[8]); set require_invariant_tsc = false "
                   "to measure anyway (a hypervisor may be masking the bit, but ticks are then not guaranteed constant-rate)";
            return;
        }
        if (cfg_.cpu != ScopedPin::kNoPin && !pin_.pin(cfg_.cpu)) {
            err_ = pin_.error() > 0 ? "could not pin the thread: pthread_getaffinity_np/pthread_setaffinity_np failed "
                                      "(CPU not in the allowed set, or affinity changes denied)"
                                    : "could not pin the thread: affinity accepted but sched_getcpu() disagrees";
            return;
        }
        detail::read_clocksource(clocksource_);
        try {
            samples_.assign(cfg_.samples, 0); // allocates AND touches every page, so run() never page-faults on it
        } catch (const std::bad_alloc&) {
            err_ = "could not allocate the sample buffer (ProfilerConfig::samples is too large)";
            return;
        }
        calibration_ = calibrate_tsc(cfg_.calibration_windows, cfg_.calibration_window_ns);
        if (!calibration_.valid) { err_ = "TSC calibration against CLOCK_MONOTONIC_RAW produced no usable window"; return; }
        try {
            timer_ticks_ = detail::measure_timer_ticks(cfg_.overhead_samples);
        } catch (const std::bad_alloc&) {
            err_ = "could not allocate the timer-cost buffer (ProfilerConfig::overhead_samples is too large)";
            return;
        }
    }

    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    bool ok() const noexcept { return err_ == nullptr; }
    const char* error() const noexcept { return err_ ? err_ : ""; }
    const TscCalibration& calibration() const noexcept { return calibration_; }

    // The timed loop. Performs no allocation, system call, or I/O of its own (test_no_alloc.cpp
    // enforces that); fn() is yours. May be called repeatedly, each call replaces the samples.
    template <class Fn>
    void run(Fn&& fn) {
        measured_ = 0;
        cpu_changes_ = 0;
        if (!ok()) return;
        for (std::size_t i = 0; i < cfg_.warmup; ++i) detail::invoke(fn);

        std::uint64_t* const out = samples_.data();
        const std::size_t n = samples_.size();
        std::uint32_t prev_aux = 0;
        (void)tsc_end(prev_aux);
        std::uint64_t changes = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t t0 = tsc_begin();
            detail::invoke(fn);
            std::uint32_t aux;
            const std::uint64_t t1 = tsc_end(aux);
            out[i] = t1 - t0;                           // the stores below sit outside the timed interval
            if (aux != prev_aux) { ++changes; prev_aux = aux; }
        }
        cpu_changes_ = changes;
        measured_ = n;
        end_cpu_ = ::sched_getcpu();
    }

    // Sorts the samples (in place, no allocation) and computes the report for the last run().
    LatencyReport finish(const char* label = "") {
        LatencyReport r;
        r.label = label;
        r.invariant_tsc = features_.invariant_tsc;
        r.rdtscp = features_.rdtscp;
        std::memcpy(r.clocksource, clocksource_, sizeof clocksource_);
        r.pinned = pin_.active();
        r.cpu = pin_.active() ? pin_.cpu() : end_cpu_;
        r.pin_held = pin_.active() && end_cpu_ == pin_.cpu();
        r.cpu_changes = cpu_changes_;
        r.calibration = calibration_;
        r.overhead_ticks = timer_ticks_;
        r.n = measured_;
        if (measured_ == 0) return r;

        std::uint64_t* const first = samples_.data();
        std::sort(first, first + measured_);
        r.min = first[0];
        r.max = first[measured_ - 1];
        r.p50 = detail::rank_value(first, measured_, 500);
        r.p90 = detail::rank_value(first, measured_, 900);
        r.p99 = detail::rank_value(first, measured_, 990);
        r.p999 = detail::rank_value(first, measured_, 999);
        double sum = 0.0;
        for (std::size_t i = 0; i < measured_; ++i) sum += static_cast<double>(first[i]);
        r.mean = sum / static_cast<double>(measured_);
        r.p999_resolved = measured_ >= 1000;
        return r;
    }

private:
    ProfilerConfig cfg_;
    CpuFeatures features_;
    ScopedPin pin_;
    char clocksource_[32] = {};
    std::vector<std::uint64_t> samples_;
    TscCalibration calibration_;
    std::uint64_t timer_ticks_ = 0;
    std::size_t measured_ = 0;
    std::uint64_t cpu_changes_ = 0;
    int end_cpu_ = -1;
    const char* err_ = nullptr;
};

// ---- Report ---------------------------------------------------------------------------------------

inline void print_report(const LatencyReport& r, std::FILE* out = stdout) {
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::fprintf(out, "== %s ==\n", r.label);
    std::fprintf(out, "  environment : invariant TSC %s, RDTSCP %s, kernel clocksource '%s'\n",
                 r.invariant_tsc ? "yes" : "NO", r.rdtscp ? "yes" : "NO", r.clocksource);
    if (r.pinned) {
        std::fprintf(out, "  pinning     : CPU %d, %s after the run; CPU changes between samples: %llu\n",
                     r.cpu, r.pin_held ? "still on it" : "NOT still on it", u(r.cpu_changes));
    } else {
        std::fprintf(out, "  pinning     : NOT PINNED (ended on CPU %d); CPU changes between samples: %llu\n",
                     r.cpu, u(r.cpu_changes));
    }
    std::fprintf(out, "  TSC         : %.6f ticks/ns (the TSC rate in GHz, not the core clock); median of %u x %.0f ms "
                      "windows vs CLOCK_MONOTONIC_RAW; spread %.0f ppm\n",
                 r.calibration.ticks_per_ns, r.calibration.windows,
                 static_cast<double>(r.calibration.window_ns) / 1e6, r.calibration.spread_ppm);
    std::fprintf(out, "  timer cost  : %llu ticks (%.1f ns) per empty begin/end pair; subtracted in the net columns, floored at 0\n",
                 u(r.overhead_ticks), r.ns(r.overhead_ticks));
    std::fprintf(out, "  samples     : %zu\n", r.n);
    if (r.n == 0) { std::fprintf(out, "  (no samples)\n"); return; }
    std::fprintf(out, "  %-14s %12s %12s %12s\n", "statistic", "raw ticks", "net ticks", "net ns");
    const auto row = [&](const char* name, std::uint64_t raw) {
        std::fprintf(out, "  %-14s %12llu %12llu %12.1f\n", name, u(raw), u(r.net(raw)), r.ns(r.net(raw)));
    };
    row("min", r.min);
    row("p50", r.p50);
    row("p90", r.p90);
    row("p99", r.p99);
    row("p99.9", r.p999);
    row("max", r.max);
    std::fprintf(out, "  %-14s %12.1f\n", "mean (raw)", r.mean);
    if (!r.p999_resolved) std::fprintf(out, "  note: fewer than 1000 samples, so p99.9 is not resolved (it equals the max)\n");
}

} // namespace bench
} // namespace animus
