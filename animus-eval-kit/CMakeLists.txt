# Animus Evaluation Kit -- standalone build.
#
# Zero external runtime dependencies: the only link requirement is the
# platform's native pthreads implementation (for thread creation and, on
# Linux, pthread_setaffinity_np). No package manager, no vendored third-
# party libraries, no network fetch at configure time.
#
# Two-step build (see README.md for the full bare-metal tuning checklist):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# Produces two binaries (build/bench_ring_buffer[.exe] and
# build/bench_itch_ingest[.exe]; run_demo.sh / run_itch_demo.sh wrap this
# same two-step build for each). Both build for development convenience on
# Windows, but Linux + GCC/Clang is the only supported measurement target
# for reported numbers -- see README.md.

cmake_minimum_required(VERSION 3.16)
project(AnimusEvalKit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Link-time / interprocedural optimization: lets the compiler inline and
# specialize across bench_ring_buffer.cpp's single translation unit and
# spsc_ring_buffer.hpp's template instantiations as if they were one
# compilation, which matters for a header full of noexcept, single-line
# push()/pop() bodies meant to be inlined at every call site.
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error_message)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
else()
    message(WARNING "IPO/LTO not supported by this toolchain: ${ipo_error_message}")
endif()

add_executable(bench_ring_buffer
    benchmarks/bench_ring_buffer.cpp
)

target_include_directories(bench_ring_buffer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    # -march=native / -mtune=native have no MSVC equivalent (MSVC's closest
    # analog, /arch:AVX2, targets an instruction-set floor, not "this exact
    # host CPU"); /O2 /GL /DNDEBUG plus CMAKE_INTERPROCEDURAL_OPTIMIZATION's
    # /LTCG above is the closest MSVC equivalent to the GCC/Clang release
    # flags below. This branch exists so the harness still builds on a
    # Windows development machine -- README.md documents Linux + GCC/Clang
    # as the only supported measurement target for reported numbers.
    target_compile_options(bench_ring_buffer PRIVATE /O2 /GL /DNDEBUG /W4)
else()
    target_compile_options(bench_ring_buffer PRIVATE
        -O3
        -march=native
        -mtune=native
        -DNDEBUG
        -Wall
        -Wextra
    )
endif()

find_package(Threads REQUIRED)
target_link_libraries(bench_ring_buffer PRIVATE Threads::Threads)

# bench_itch_ingest -- NASDAQ TotalView-ITCH 5.0 adapter ingestion benchmark.
# Same standalone-kit SpscRingBuffer<T> as bench_ring_buffer above; headers
# under include/ are the exact bundle from adapters/itch50/include/ and
# include/animus/ in the full repo (thread_affinity.hpp, schema.hpp,
# execution_event.hpp) -- see README.md's Contents section. Flags mirror the
# root CMakeLists.txt's verified build for this same source file
# (-O3 -Wall -Wextra -Wpedantic on GCC/Clang, /W4 /permissive- /wd4324 on
# MSVC) rather than bench_ring_buffer's -march=native tuning above, since
# this target's own build matrix requirement is portability, not peak
# single-host tuning -- see adapters/itch50/README.md §5.1.
add_executable(bench_itch_ingest
    benchmarks/bench_itch_ingest.cpp
)

target_include_directories(bench_itch_ingest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(bench_itch_ingest PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(bench_itch_ingest PRIVATE rt)
endif()

if(MSVC)
    # /wd4324: alignas(64) ItchFrame/SpscRingBuffer cache-line padding is
    # intentional, not a layout bug -- same suppression the root
    # CMakeLists.txt applies to this identical translation unit.
    target_compile_options(bench_itch_ingest PRIVATE /W4 /permissive- /O2 /wd4324)
else()
    target_compile_options(bench_itch_ingest PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
