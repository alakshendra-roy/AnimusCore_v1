// Linux POSIX shared-memory probe: what does each way of creating and mapping a
// shm segment actually cost on the first write, and does the segment have real
// backing? Prints one row per strategy, from the kernel's own counters
// (getrusage minor faults, fstat st_blocks) -- no timing, nothing assumed.
//
//   lazy library mapping     SharedMemoryRegion::create(..., prefault = false)
//   prefaulted library       SharedMemoryRegion::create(..., prefault = true)   <- the default
//   raw fallocate, lazy      shm_open + posix_fallocate + mmap(MAP_SHARED)
//   raw fallocate+POPULATE   ... + MAP_POPULATE
//   raw ftruncate, lazy      shm_open + ftruncate + mmap  (the bare minimum; sparse backing)
//
// The two things to read: "first write" faults (a hot loop pays one per 4 KiB page
// when this is high) and "backed MiB" (what the segment really occupies BEFORE anything
// writes to it: posix_fallocate reserves it at create time, whereas a sparse
// ftruncate'd segment reports ~0 and fails later, with SIGBUS, if /dev/shm fills up).
//
// Linux only (exits 0 with a note elsewhere).
// Build: g++ -std=c++17 -O1 -pthread -Iinclude tests/posix_shm_fault_probe.cpp -o posix_shm_fault_probe -lrt
// Usage: posix_shm_fault_probe [--mib N]      (default 64)
#include "../include/animus/shm_ipc.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/stat.h>

using animus::sys::ipc::SharedMemoryRegion;

namespace {

long minor_faults() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

long write_every_page(void* base, size_t size, size_t page) {
    auto* bytes = static_cast<volatile unsigned char*>(base);
    const long before = minor_faults();
    for (size_t off = 0; off < size; off += page) bytes[off] = 1;
    return minor_faults() - before;
}

long long backed_mib(const char* name) {
    const int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) return -1;
    struct stat st{};
    const long long mib = fstat(fd, &st) == 0 ? static_cast<long long>(st.st_blocks) * 512 / (1 << 20) : -1;
    close(fd);
    return mib;
}

struct Row {
    const char* label;
    long map_faults = 0;
    long write_faults = 0;
    long long backed = 0;
    bool ok = false;
};

Row run_library(const char* label, size_t size, size_t page, bool prefault) {
    Row r{ label };
    const char* name = "animus_fault_probe_lib";
    shm_unlink(name);
    const long f0 = minor_faults();
    SharedMemoryRegion region;
    r.ok = SharedMemoryRegion::create(name, size, region, prefault);
    r.map_faults = minor_faults() - f0;
    if (r.ok) {
        r.backed = backed_mib(name); // BEFORE the write pass: after it, every strategy has touched (and so backed) every page
        r.write_faults = write_every_page(region.data(), size, page);
    }
    region.close();
    shm_unlink(name);
    return r;
}

Row run_raw(const char* label, size_t size, size_t page, bool fallocate, bool populate) {
    Row r{ label };
    const char* name = "animus_fault_probe_raw";
    shm_unlink(name);
    const long f0 = minor_faults();
    const int fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) return r;
    const bool sized = fallocate ? posix_fallocate(fd, 0, static_cast<off_t>(size)) == 0
                                 : ftruncate(fd, static_cast<off_t>(size)) == 0;
    void* base = sized ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | (populate ? MAP_POPULATE : 0), fd, 0) : MAP_FAILED;
    close(fd);
    if (base != MAP_FAILED) {
        r.ok = true;
        r.map_faults = minor_faults() - f0;
        r.backed = backed_mib(name); // before any write, for the same reason
        r.write_faults = write_every_page(base, size, page);
        munmap(base, size);
    }
    shm_unlink(name);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    size_t mib = 64;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mib") == 0 && i + 1 < argc) mib = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        else { std::fprintf(stderr, "usage: %s [--mib N]\n", argv[0]); return 2; }
    }
    const size_t size = mib << 20;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    std::printf("POSIX shm probe: %zu MiB segment, %zu-byte pages (%zu pages), kernel counters only\n\n", mib, page, size / page);
    std::printf("%-34s %14s %20s %12s\n", "strategy", "faults @ map", "faults @ 1st write", "backed MiB");
    const Row rows[] = {
        run_library("library, prefault=false", size, page, false),
        run_library("library, prefault=true (default)", size, page, true),
        run_raw("raw: posix_fallocate, lazy", size, page, true, false),
        run_raw("raw: posix_fallocate + MAP_POPULATE", size, page, true, true),
        run_raw("raw: ftruncate only (sparse), lazy", size, page, false, false),
    };
    for (const Row& r : rows) {
        if (!r.ok) { std::printf("%-34s  (failed)\n", r.label); continue; }
        std::printf("%-34s %14ld %20ld %12lld\n", r.label, r.map_faults, r.write_faults, r.backed);
    }
    std::printf("\nRead: a hot loop pays 'faults @ 1st write' page faults on its first pass; the library default moves them into create().\n");
    std::printf("      'backed MiB' ~ 0 means the segment is sparse: a full /dev/shm then shows up as SIGBUS on first touch, not as a create() error.\n");
    return 0;
}
#else
int main() {
    std::printf("posix_shm_fault_probe: Linux only (uses getrusage minor-fault counters and /dev/shm semantics)\n");
    return 0;
}
#endif
