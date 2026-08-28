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
