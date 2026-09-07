// Animus ITCH 5.0 Adapter -- Cross-Process Shared-Memory Bridge
// (itch50_shm_bridge)
//
// bench_itch_ingest.cpp proves zero-allocation ITCH ingestion into an
// in-process animus::eval::SpscRingBuffer<ItchFrame>. This binary proves
// the companion claim from adapters/itch50/README.md's Python Consumer
// Compatibility section: that a *separate* Python process can attach to
// the exact same decoded ItchFrame records over a real OS shared-memory
// segment and read them as a zero-copy NumPy structured array, using the
// existing schema-agnostic animus-native-stream binding
// (bindings/animus_shm_py.cpp's SharedSchemaChannel) and
// animus.dynamic_schema.to_structured_array() -- no ItchFrame-specific
// Python or C++ binding code required, because ItchFrame is a registered
// ANIMUS_DEFINE_SCHEMA wire schema (adapters/itch50/include/itch50_ring_frame.hpp).
//
// Creates a named animus::sys::ipc::ShmRing<ItchFrame> (Windows:
// CreateFileMapping; POSIX: shm_open/mmap), decodes and pushes
// --messages synthetic ITCH records into it (same Generator + decode()
// path as bench_itch_ingest.cpp, just without the RDTSC timing -- this
// binary demonstrates cross-process interop, not latency), then holds the
// segment open for --hold-seconds so a concurrently-running Python process
// has time to attach and read it.
//
// IMPORTANT (Windows): a Windows named file mapping is destroyed the
// instant its last open HANDLE closes -- unlike POSIX shm_open, which
// persists until an explicit shm_unlink() regardless of process exit (see
// include/animus/shm_ipc.hpp's own SharedMemoryRegion::close()/unlink()
// comments). That means on Windows this process MUST still be running
// (inside its --hold-seconds wait) when the Python side attaches --
// launch this first, then run adapters/itch50/verify_zero_copy_numpy.py
// in a second terminal while this one is still waiting, exactly as
// eval_kit/README.md's own Troubleshooting section documents for the
// same reason.

#include "animus/shm_ipc.hpp"
#include "itch50_codec.hpp"
#include "itch50_ring_frame.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace itch50_shm_bridge {

using adapters::itch50::ItchFrame;
using adapters::itch50::MessageType;
using Ring = animus::sys::ipc::ShmRing<ItchFrame>;

// Same synthetic-message generator as bench_itch_ingest.cpp, duplicated
// rather than shared: that copy also drives RDTSC-timed hot-path
// generation logic this binary has no use for, and the two files are
// meant to be readable independently (bench_itch_ingest.cpp is the
// performance proof point; this one is the cross-process interop proof
// point) -- see adapters/itch50/README.md's Layout section.
struct Generator {
    uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
    uint64_t next_order_ref = 1;
    uint64_t next_match_number = 1;
    uint64_t itch_clock_ns = 34'200'000'000'000ULL;

    static constexpr const char* kSymbols[] = {"AAPL    ", "MSFT    ", "GOOG    ", "AMZN    "};
    static constexpr int kNumSymbols = 4;

    uint64_t next_rand() noexcept {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        return rng_state;
    }

    std::size_t generate(uint8_t* buf, MessageType& type_out) noexcept {
        using namespace adapters::itch50;
        const uint32_t roll = static_cast<uint32_t>(next_rand() % 100);
        MessageType type = (roll < 60) ? MessageType::AddOrderNoMPID
                          : (roll < 85) ? MessageType::OrderExecuted
                                        : MessageType::OrderDelete;
        type_out = type;

        itch_clock_ns += 100 + (next_rand() % 5000);
        const uint16_t stock_locate = static_cast<uint16_t>(1 + (next_rand() % 8000));
        const uint16_t tracking_number = static_cast<uint16_t>(next_rand() % 65536);
        const uint32_t shares = static_cast<uint32_t>(1 + (next_rand() % 1000));
        const uint32_t price = static_cast<uint32_t>(500000 + (next_rand() % 2000000));
        const char* symbol = kSymbols[next_rand() % kNumSymbols];

        buf[0] = static_cast<uint8_t>(type);
        put_be16(buf + 1, stock_locate);
        put_be16(buf + 3, tracking_number);
        put_be48_ns(buf + 5, itch_clock_ns);

        std::size_t len = 0;
        switch (type) {
        case MessageType::AddOrderNoMPID: {
            const uint64_t order_ref = next_order_ref++;
            put_be64(buf + 11, order_ref);
            buf[19] = (next_rand() % 2) ? 'B' : 'S';
            put_be32(buf + 20, shares);
            std::memcpy(buf + 24, symbol, 8);
            put_be32(buf + 32, price);
            len = sizeof(AddOrderMsg);
            break;
        }
        case MessageType::OrderExecuted: {
            put_be64(buf + 11, next_order_ref == 1 ? 1 : (next_rand() % next_order_ref) + 1);
            put_be32(buf + 19, shares);
            put_be64(buf + 23, next_match_number++);
            len = sizeof(OrderExecutedMsg);
            break;
        }
        default: { // OrderDelete
            put_be64(buf + 11, next_order_ref == 1 ? 1 : (next_rand() % next_order_ref) + 1);
            len = sizeof(OrderDeleteMsg);
            break;
        }
        }
        return len;
    }
};

struct Options {
    std::string name = "animus_itch50_shm_demo";
    uint64_t message_count = 100'000ull;
    size_t ring_capacity = 1 << 17; // 131,072 frames * 64 bytes = 8 MiB segment -- comfortably holds the default --messages with no overwrite
    int hold_seconds = 60;
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(2); }
            return argv[++i];
        };
        if (arg == "--name") o.name = next("--name");
        else if (arg == "--messages") o.message_count = std::strtoull(next("--messages").c_str(), nullptr, 10);
        else if (arg == "--capacity") o.ring_capacity = static_cast<size_t>(std::strtoull(next("--capacity").c_str(), nullptr, 10));
        else if (arg == "--hold-seconds") o.hold_seconds = std::atoi(next("--hold-seconds").c_str());
        else if (arg == "--help") {
            std::printf(
                "Usage: itch50_shm_bridge [--name NAME] [--messages N] [--capacity SLOTS] [--hold-seconds S]\n"
                "  --name           shared-memory segment name (default: animus_itch50_shm_demo)\n"
                "  --messages       synthetic decoded ItchFrame records to push (default: 100000)\n"
                "  --capacity       ring capacity in frames, rounded up to a power of two (default: 65536)\n"
                "  --hold-seconds   seconds to hold the segment open after pushing, for a Python\n"
                "                   consumer to attach (default: 60) -- required on Windows, see\n"
                "                   this file's own header comment\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "error: unrecognized argument '%s'\n", argv[i]);
            std::exit(2);
        }
    }
    return o;
}

int run(int argc, char** argv) {
    const Options o = parse_args(argc, argv);

    auto ring = Ring::create(o.name.c_str(), o.ring_capacity);
    if (!ring) {
        std::fprintf(stderr,
            "error: Ring::create('%s') failed -- a segment with this name may already exist "
            "(rm -f /dev/shm/%s on POSIX; on Windows it disappears once every handle to it "
            "closes) or the OS refused the mapping\n", o.name.c_str(), o.name.c_str());
        return 1;
    }
    ring->mark_producer_attached();
    std::printf("[itch50_shm_bridge] created ring '%s' (capacity=%zu frames, stride=%zu bytes, schema='%s')\n",
                o.name.c_str(), ring->capacity(), ring->stride(), ring->schema_name());

    Generator gen;
    uint64_t pushed = 0, decode_failures = 0;
    uint8_t wire_buf[adapters::itch50::kMaxWireMessageSize];
    for (uint64_t i = 0; i < o.message_count; ++i) {
        MessageType type;
        const std::size_t wire_len = gen.generate(wire_buf, type);
        ItchFrame frame{};
        if (!adapters::itch50::decode(wire_buf, wire_len, frame)) {
            ++decode_failures;
            continue;
        }
        frame.sequence_id = pushed;
        frame.recv_timestamp_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        // push_overwrite(), not push_spin(): this demo has no consumer
        // draining the ring (SharedSchemaChannel on the Python side is a
        // read-only inspection view -- it never advances tail), so a
        // bounded-retry push_spin() would spin its full kDefaultMaxSpins
        // budget once the ring fills and never succeed, effectively
        // hanging this process. Overwrite mode is the correct, documented
        // choice for exactly this "no consumer attached (yet)" shape --
        // same reasoning as eval_kit/README.md's own default producer mode.
        ring->push_overwrite(frame);
        ++pushed;
        ring->producer_heartbeat();
    }

    std::printf("[itch50_shm_bridge] pushed %llu ItchFrame records (%llu decode failures, %llu overwritten before read)\n",
                static_cast<unsigned long long>(pushed), static_cast<unsigned long long>(decode_failures),
                static_cast<unsigned long long>(ring->dropped_count()));
    std::printf("[itch50_shm_bridge] holding segment '%s' open for %d second(s) -- run "
                "adapters/itch50/verify_zero_copy_numpy.py --name %s now\n",
                o.name.c_str(), o.hold_seconds, o.name.c_str());

    for (int remaining = o.hold_seconds; remaining > 0; --remaining) {
        ring->producer_heartbeat();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    Ring::unlink(o.name.c_str());
    std::printf("[itch50_shm_bridge] segment unlinked, exiting\n");
    return 0;
}

} // namespace itch50_shm_bridge

int main(int argc, char** argv) {
    return itch50_shm_bridge::run(argc, argv);
}
