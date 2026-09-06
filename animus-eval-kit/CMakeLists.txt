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
# The resulting binary is build/bench_ring_buffer (build/bench_ring_buffer.exe
# on Windows, where it builds for development convenience but is not the
# supported measurement target -- see README.md).

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
