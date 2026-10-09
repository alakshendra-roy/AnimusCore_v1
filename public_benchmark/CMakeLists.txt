# SPDX-License-Identifier: Apache-2.0
# The nanobind zero-copy bridge (src/python_bridge.cpp). `make pybridge` drives this; it can also be run by hand:
#
#   python3 -m pip install nanobind numpy
#   cmake -S . -B build/bridge -DCMAKE_BUILD_TYPE=Release        (-DPython_EXECUTABLE=<python with nanobind>)
#   cmake --build build/bridge --config Release
#   python python/test_python_bridge.py                          (finds the module under build/)
#
# The strict warning set applies to python_bridge.cpp only; nanobind's own sources are built by its own target
# with nanobind's flags. Verified platforms: x86-64 Linux (clang, gcc) and Windows x64 (MSVC); see README.md.
cmake_minimum_required(VERSION 3.18)
project(animus_ring_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)

execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE NB_DIR
    RESULT_VARIABLE NB_DIR_RESULT
)
if(NOT NB_DIR_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Could not locate nanobind via '${Python_EXECUTABLE} -m nanobind --cmake_dir'. "
        "Install it into that interpreter's environment (pip install nanobind) or pass -DPython_EXECUTABLE=.")
endif()
list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(animus_bridge NB_STATIC src/python_bridge.cpp)

target_include_directories(animus_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    # /wd4324: "structure was padded due to alignment specifier" is alignas(64) doing its job.
    target_compile_options(animus_bridge PRIVATE /W4 /WX /permissive- /wd4324)
    target_compile_definitions(animus_bridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(animus_bridge PRIVATE -O3 -march=native -Wall -Wextra -Werror -pedantic)
endif()
