# Animus Engine -- Institutional Python SDK (sdk/python), nanobind native
# extension (csrc/animus_ring_buffer.cpp).
#
# Standalone CMake project, same pattern as bindings/CMakeLists.txt (which
# this repo's animus/ zero-dependency package uses): kept independent of
# the root CMakeLists.txt (animus_native, the C-ABI shim) and of
# bindings/CMakeLists.txt (the internal-engine telemetry/shm bindings) so
# all three stay separately buildable and shippable. This is the SDK a
# client installs to consume TelemetryFrame records straight into NumPy --
# it depends only on the header-only animus-eval-kit and include/ trees,
# never on AnimusCore_v1.hpp's much larger engine surface.
cmake_minimum_required(VERSION 3.15)
project(animus_sdk_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python 3.8 COMPONENTS Interpreter Development.Module REQUIRED)

# Locate nanobind's CMake package via the interpreter actually building
# this, same approach as bindings/CMakeLists.txt (see that file's own
# comment for why: this finds a venv's nanobind correctly instead of
# guessing a path).
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE NB_DIR
    RESULT_VARIABLE NB_DIR_RESULT
)
if(NOT NB_DIR_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Could not locate nanobind's CMake config via '${Python_EXECUTABLE} -m nanobind --cmake_dir'. "
        "Is nanobind installed in that interpreter's environment? (pip install nanobind)")
endif()
list(APPEND CMAKE_PREFIX_PATH "${NB_DIR}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_animus_sdk_native
    NB_STATIC
    csrc/animus_ring_buffer.cpp
)

target_include_directories(_animus_sdk_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../animus-eval-kit/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

if(WIN32)
    target_compile_definitions(_animus_sdk_native PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(Threads REQUIRED)
target_link_libraries(_animus_sdk_native PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(_animus_sdk_native PRIVATE /W4 /permissive- /wd4324)
else()
    target_compile_options(_animus_sdk_native PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Installed as animus_sdk/_animus_sdk_native(.pyd|.so) -- animus_sdk/
# imports it as a sibling module.
install(TARGETS _animus_sdk_native LIBRARY DESTINATION animus_sdk)

# ---------------------------------------------------------------------
# Local build without scikit-build-core, for fast iteration:
#
#   pip install nanobind
#   cmake -S sdk/python -B sdk/python/build -DPython_EXECUTABLE=<path to your python>
#   cmake --build sdk/python/build --config Release
#
# The resulting _animus_sdk_native(.pyd|.so) lands in sdk/python/build/ (or
# sdk/python/build/Release/ on multi-config generators) -- copy it into
# sdk/python/animus_sdk/ (next to consumer.py) to import it directly, or
# add that build directory to PYTHONPATH. For an installable wheel, use
# `pip install ./sdk/python` instead -- see pyproject.toml.
# ---------------------------------------------------------------------
