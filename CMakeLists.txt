cmake_minimum_required(VERSION 3.20)

project(acomposter
    VERSION 0.1.0
    DESCRIPTION "acomposter - AV patching and performance environment"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR
        "acomposter targets Windows. Configure with a MSVC generator or one of the "
        "mingw-w64 toolchain files in cmake/ (see docs/BUILDING.md).")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

# The 32-bit VST bridge is a separate executable built from a separate configure
# pass, because it must be compiled for x86 while the host app is x64.
option(ACOMPOSTER_BRIDGE_ONLY "Build only the 32-bit VST bridge executable" OFF)
option(ACOMPOSTER_BUILD_TESTS "Build the host-side test runner" OFF)
option(ACOMPOSTER_WERROR      "Treat warnings as errors" OFF)

set(ACOMPOSTER_VERSION_STRING "${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Compiler configuration
# ---------------------------------------------------------------------------

add_library(acomposter_flags INTERFACE)

target_compile_definitions(acomposter_flags INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    ACOMPOSTER_VERSION="${ACOMPOSTER_VERSION_STRING}"
    $<$<CONFIG:Debug>:ACOMPOSTER_DEBUG=1>
)

# Windows 10 (0x0A00) is the minimum supported target.
target_compile_definitions(acomposter_flags INTERFACE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

if(MSVC)
    target_compile_options(acomposter_flags INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc
        /wd4100          # unreferenced formal parameter
        /wd4201          # nameless struct/union (used by the D3D/Win32 headers)
        $<$<CONFIG:Release>:/O2 /Oi /GL /fp:fast>
    )
    target_link_options(acomposter_flags INTERFACE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )
    if(ACOMPOSTER_WERROR)
        target_compile_options(acomposter_flags INTERFACE /WX)
    endif()
else()
    # mingw-w64 / clang targeting Windows
    target_compile_options(acomposter_flags INTERFACE
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -ffast-math
        $<$<CONFIG:Release>:-O2>
    )
    if(ACOMPOSTER_WERROR)
        target_compile_options(acomposter_flags INTERFACE -Werror)
    endif()
    # Static-link the runtime so the shipped binaries have no MSYS/GCC DLL deps.
    target_link_options(acomposter_flags INTERFACE
        -static-libgcc -static-libstdc++ -static
    )
endif()

# Denormal-flushing and SSE2 are assumed throughout the DSP code.
if(NOT MSVC)
    target_compile_options(acomposter_flags INTERFACE -msse2 -mfpmath=sse)
endif()

# The GUI executables enter through wWinMain. MSVC infers that from UNICODE, but
# mingw's startup code goes looking for a narrow WinMain unless it is told
# otherwise, at both compile and link time. This is deliberately kept off the
# shared flags: the test runner is a console program with a plain main().
add_library(acomposter_wide_entry INTERFACE)
if(NOT MSVC)
    target_compile_options(acomposter_wide_entry INTERFACE -municode)
    target_link_options(acomposter_wide_entry INTERFACE -municode)
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

add_subdirectory(src)

if(ACOMPOSTER_BUILD_TESTS AND NOT ACOMPOSTER_BRIDGE_ONLY)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "  acomposter ${ACOMPOSTER_VERSION_STRING}")
message(STATUS "  ------------------------------------------------")
message(STATUS "  compiler       : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  pointer size   : ${CMAKE_SIZEOF_VOID_P} bytes")
message(STATUS "  bridge only    : ${ACOMPOSTER_BRIDGE_ONLY}")
message(STATUS "")
