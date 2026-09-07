# Portable, cross-platform companion to AnimusCore_v1.vcxproj (MSVC-only).
#
# Builds AnimusNative -- the same C-ABI shim as AnimusCore_v1.dll
# (AnimusCore_v1/animus_engine.cpp, re-exposing animus.hpp's header-only
# animus::Engine across extern "C") -- but through CMake so it also produces
# libanimus_native.so / libanimus_native.dylib on Linux/macOS, which the
# vcxproj cannot. This target is additive: it does not replace or touch the
# existing AnimusCore_v1.dll build, and animus/bindings.py's native library
# search checks both names.
cmake_minimum_required(VERSION 3.15)
project(AnimusNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(animus_native SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/AnimusCore_v1/animus_engine.cpp
)

target_include_directories(animus_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/AnimusCore_v1
)

target_compile_definitions(animus_native PRIVATE ANIMUS_EXPORTS)

# Needed for pthread_setaffinity_np (animus_pin_current_thread_to_core's
# Linux implementation, animus_engine.cpp) -- std::thread's own use in
# EngineImpl already implies pthread, but that's an implicit dependency of
# whichever glibc/libstdc++ this builds against, not a guarantee; linking
# it explicitly via the portable CMake mechanism (works whether pthread is
# a separate library or merged into libc) doesn't rely on that holding.
find_package(Threads REQUIRED)
target_link_libraries(animus_native PRIVATE Threads::Threads)

# Needed for shm_open/shm_unlink (SharedMemorySegment's POSIX
# implementation, animus.hpp) on glibc older than 2.34, which shipped them
# in a separate librt rather than libc itself; harmless to link even where
# it's already merged, since glibc still ships librt.so as a compatibility
# stub. Not needed (and not present) on macOS, where shm_open is part of
# libSystem directly.
if(UNIX AND NOT APPLE)
    target_link_libraries(animus_native PRIVATE rt)
endif()

# Needed for animus_verify_license's Windows implementation (animus_engine.cpp):
# bcrypt (RSA signature verification + SHA-256), iphlpapi (MAC address
# enumeration), ws2_32 (winsock2.h/ws2tcpip.h, required by iphlpapi.h's own
# declarations). MSVC picks these up automatically via #pragma comment(lib,
# ...) in the source; MinGW/g++ does not honor that pragma, so this is the
# only thing that actually links them for a CMake+MinGW build on Windows.
if(WIN32)
    target_link_libraries(animus_native PRIVATE bcrypt iphlpapi ws2_32)
endif()

if(NOT MSVC)
    target_compile_options(animus_native PRIVATE -fvisibility=hidden)
    # Statically link the GCC/MinGW runtime (libstdc++/libgcc/winpthread) so
    # the produced binary is redistributable on its own -- without this, a
    # MinGW-built AnimusNative.dll only loads on a machine that also has
    # MSYS2/MinGW's runtime DLLs on PATH, which defeats the point of
    # shipping a standalone C-ABI shared library.
    target_link_options(animus_native PRIVATE -static-libgcc -static-libstdc++)
    if(WIN32)
        target_link_options(animus_native PRIVATE -static -lwinpthread)
    endif()
endif()

if(WIN32)
    # -> AnimusNative.dll. PREFIX must be cleared explicitly: MinGW/GCC
    # defaults to a "lib" prefix on shared libraries even on Windows, where
    # MSVC does not -- without this, a MinGW build produces
    # libAnimusNative.dll instead of AnimusNative.dll.
    set_target_properties(animus_native PROPERTIES OUTPUT_NAME "AnimusNative" PREFIX "")
else()
    # -> libanimus_native.so / libanimus_native.dylib
    set_target_properties(animus_native PROPERTIES OUTPUT_NAME "animus_native")
endif()

# Land the built binary directly under the top-level build directory
# (rather than a per-config subdirectory) so animus/bindings.py's fixed
# search paths (build/, build/Release/) find it without guessing a
# generator-specific layout.
set_target_properties(animus_native PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} config_upper)
    set_target_properties(animus_native PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/${config}
        LIBRARY_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/${config}
    )
endforeach()

# telemetry_benchmark -- standalone C++23 proof-point harness (benchmarks/).
# A per-target language standard (cxx_std_23) rather than the project-wide
# CMAKE_CXX_STANDARD above, so this doesn't force C++23 onto animus_native,
# which stays on the C++17 the exported C-ABI shim is built against.
add_executable(telemetry_benchmark
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/telemetry_benchmark.cpp
)
target_compile_features(telemetry_benchmark PRIVATE cxx_std_23)
find_package(Threads REQUIRED)
target_link_libraries(telemetry_benchmark PRIVATE Threads::Threads)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # libstdc++'s <print> ships its Windows/POSIX terminal-writing backend
    # (std::__open_terminal / std::__write_to_terminal) in the separate
    # "experimental" static library until the TS support stabilizes --
    # without this, linking fails with undefined references to those two
    # symbols on any GCC (including MinGW) new enough to have <print> at all.
    target_link_libraries(telemetry_benchmark PRIVATE stdc++exp)
endif()
set_target_properties(telemetry_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# harness_benchmark -- cross-process shared-memory producer harness
# (Milestone 4). C++17 (matching include/animus/shm_ipc.hpp's own
# standard, not telemetry_benchmark's C++23 opt-in above) so this builds
# with the same toolchains as animus_native itself. Unlike
# telemetry_benchmark, this is one half of a two-*process* demo -- see
# benchmarks/consumer.py for the other half, and benchmarks/build.sh for
# the POSIX ASan/UBSan build path this CMake target does not replace.
add_executable(harness_benchmark
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/harness_benchmark.cpp
)
target_include_directories(harness_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(harness_benchmark PRIVATE cxx_std_17)
target_link_libraries(harness_benchmark PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(harness_benchmark PRIVATE rt)
endif()
set_target_properties(harness_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# test_dynamic_schema -- Milestone 1 regression test (Dynamic & User-Defined
# Wire Schemas). Same cross-process, C++17, include/-only dependency shape
# as harness_benchmark above; this binary is both the producer and the
# consumer side, selected by argv -- see tests/test_dynamic_schema.cpp's own
# header comment for how to run it as two separate OS processes.
add_executable(test_dynamic_schema
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dynamic_schema.cpp
)
target_include_directories(test_dynamic_schema PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(test_dynamic_schema PRIVATE cxx_std_17)
target_link_libraries(test_dynamic_schema PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_dynamic_schema PRIVATE rt)
endif()
set_target_properties(test_dynamic_schema PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# test_spmc -- Milestone 2 regression test (Single-Producer Multi-Consumer
# Broadcast Ring). Same shape as test_dynamic_schema above; this binary is
# all three roles (one producer, two independently-paced consumers),
# selected by argv -- see tests/test_spmc.cpp's own header comment for how
# to run it as three separate OS processes.
add_executable(test_spmc
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spmc.cpp
)
target_include_directories(test_spmc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(test_spmc PRIVATE cxx_std_17)
target_link_libraries(test_spmc PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_spmc PRIVATE rt)
endif()
set_target_properties(test_spmc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# test_telemetry -- Milestone 3 regression test (Non-Blocking Telemetry &
# Observability Layer). Single-process (see tests/test_telemetry.cpp's own
# header comment for why AnimusGetMetrics() doesn't need a second OS
# process to exercise), same include/-only dependency shape as the other
# tests above.
add_executable(test_telemetry
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_telemetry.cpp
)
target_include_directories(test_telemetry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(test_telemetry PRIVATE cxx_std_17)
target_link_libraries(test_telemetry PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_telemetry PRIVATE rt)
endif()
set_target_properties(test_telemetry PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# animus_bench -- institutional zero-allocation ingestion benchmark harness
# (C++20). Measures ingress-to-egress transit latency across the zero-copy
# SPSC ring buffer (animus-eval-kit/include/spsc_ring_buffer.hpp) under
# synthetic sustained/burst market-tick load, for live technical evaluations.
# Lands at build/bin/animus_bench (not build/, unlike the targets above) per
# that evaluation tooling's fixed expected path.
add_executable(animus_bench
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/animus_harness.cpp
)
target_include_directories(animus_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/animus-eval-kit/include
)
target_compile_features(animus_bench PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(animus_bench PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(animus_bench PRIVATE rt)
endif()
if(MSVC)
    # /permissive- is MSVC's closest analog to -Wpedantic (strict standards
    # conformance mode); /W4 is its closest analog to -Wall -Wextra.
    #
    # /wd4324 ("structure was padded due to alignment specifier") is
    # suppressed deliberately: it fires on TelemetryFrame's alignas(64) and
    # on SpscRingBuffer's head_/tail_ cursors, which are padded to a full
    # cache line on purpose (see the header comments on both) to avoid false
    # sharing. This is the intended layout, not an accidental one -Wall
    # should catch.
    target_compile_options(animus_bench PRIVATE /W4 /permissive- /O2 /wd4324)
else()
    target_compile_options(animus_bench PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
set_target_properties(animus_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# bench_itch_ingest -- NASDAQ TotalView-ITCH 5.0 adapter ingestion benchmark
# (adapters/itch50/). Same in-process SPSC ring (animus-eval-kit/include/
# spsc_ring_buffer.hpp) and RDTSC-calibration methodology as animus_bench
# above, applied to zero-allocation ITCH wire decoding instead of synthetic
# telemetry frames -- see adapters/itch50/README.md. C++17 (no C++20
# feature is used here; kept portable rather than matching animus_bench's
# arbitrary cxx_std_20, since this target's own build matrix requirement is
# "compiles cleanly under GCC/Clang and MSVC").
# Lands at build/bin/bench_itch_ingest, alongside animus_bench, since both
# are live-evaluation proof-point binaries with the same fixed expected
# path convention.
add_executable(bench_itch_ingest
    ${CMAKE_CURRENT_SOURCE_DIR}/adapters/itch50/bench_itch_ingest.cpp
)
target_include_directories(bench_itch_ingest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/animus-eval-kit/include
    ${CMAKE_CURRENT_SOURCE_DIR}/adapters/itch50/include
)
target_compile_features(bench_itch_ingest PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(bench_itch_ingest PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(bench_itch_ingest PRIVATE rt)
endif()
if(MSVC)
    # /wd4324: alignas(64) ItchFrame/SpscRingBuffer cache-line padding is
    # intentional -- see animus_bench's identical suppression above.
    target_compile_options(bench_itch_ingest PRIVATE /W4 /permissive- /O2 /wd4324)
else()
    target_compile_options(bench_itch_ingest PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
set_target_properties(bench_itch_ingest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# itch50_shm_bridge -- cross-process shared-memory producer proving the
# Python zero-copy consumption path (adapters/itch50/README.md's Python
# Consumer Compatibility section /
# adapters/itch50/verify_zero_copy_numpy.py): pushes decoded ItchFrame
# records into a named animus::sys::ipc::ShmRing<ItchFrame>
# (include/animus/shm_ipc.hpp) for a separate Python process to attach to
# via the existing schema-agnostic animus-native-stream binding. Same
# cross-process shape and C++17 standard as harness_benchmark/
# test_dynamic_schema above, not animus_bench's in-process ring.
add_executable(itch50_shm_bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/adapters/itch50/itch50_shm_bridge.cpp
)
target_include_directories(itch50_shm_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/adapters/itch50/include
)
target_compile_features(itch50_shm_bridge PRIVATE cxx_std_17)
target_link_libraries(itch50_shm_bridge PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(itch50_shm_bridge PRIVATE rt)
endif()
set_target_properties(itch50_shm_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
