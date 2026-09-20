# Standalone CMake project for AnimusCore's hot-path benchmark harness and
# telemetry-packet example. Deliberately separate from the repository root's
# CMakeLists.txt (which builds AnimusNative -- the C-ABI DLL/shared library,
# licensing layer, and market-data adapters): everything here depends on
# nothing but the single-header AnimusCore_v1/animus.hpp core, the C++17
# standard library, and pthreads, so it configures and builds in well under
# a minute with no other target in the tree.
#
#   cmake -S bench -B build_bench -DCMAKE_BUILD_TYPE=Release
#   cmake --build build_bench -j
#   ./build_bench/hotpath_bench
#   ./build_bench/custom_packet_example

cmake_minimum_required(VERSION 3.15)
project(animus_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

set(ANIMUS_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../AnimusCore_v1")

function(animus_bench_target name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${ANIMUS_CORE_DIR})
    target_link_libraries(${name} PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(${name} PRIVATE /O2 /W4)
    else()
        target_compile_options(${name} PRIVATE -O3 -march=native -Wall -Wextra)
    endif()
endfunction()

animus_bench_target(hotpath_bench hotpath_bench.cpp)
animus_bench_target(custom_packet_example custom_packet_example.cpp)
