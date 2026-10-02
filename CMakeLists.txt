cmake_minimum_required(VERSION 3.20)
project(chess_engine VERSION 2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# An engine built without optimization is a different, much weaker program:
# default to Release unless a build type is chosen.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Link-time optimization lets calls inline across the engine's files, and
# -march=native tunes for this machine's CPU (the binaries then only run on
# CPUs with the same instruction sets). Both are off by default: measured
# on 2026-09-26 with GCC 16, neither changed bench speed (all four
# combinations within 1.5%). Worth measuring again once the evaluation
# uses SIMD.
option(LTO "Link-time optimization" OFF)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if (LTO AND ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# Release builds for other machines: ARCH sets -march to a portable CPU
# level, x86-64 (any 64-bit x86 CPU) or x86-64-v3 (AVX2, BMI2: Haswell,
# Zen and later), so testers' CPUs don't hit instructions they lack. Empty
# (the default) leaves it to the compiler. NATIVE overrides it.
set(ARCH "" CACHE STRING "CPU level for -march: x86-64, x86-64-v3, or empty")
if (ARCH AND NOT NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-march=${ARCH})
endif()

option(NATIVE "Optimize for the build machine's CPU" OFF)
if (NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-march=native)
endif()

# -DTUNE=ON advertises the search parameters in params.h as UCI options,
# for automated tuning (SPSA). Normal builds leave them out of the GUI.
option(TUNE "Expose search parameters as UCI options" OFF)
# Continuation logging (cont_log.h, the genconlog command): compiled in only
# with CONT_LOG, since even switched off its hooks cost the search about 2%
# nps.
option(CONT_LOG "Compile in continuation logging (genconlog)" OFF)

function(engine_warnings target)
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    endif()
    # Link the C++ runtime statically on MinGW. Otherwise the executable
    # loads whichever libstdc++-6.dll comes first on PATH (Git for Windows
    # ships an older, incompatible one), and it can't be copied elsewhere
    # to run under a match runner.
    get_target_property(type ${target} TYPE)
    if (MINGW AND type STREQUAL "EXECUTABLE")
        target_link_options(${target} PRIVATE -static)
        # Reserve 16 MB of stack (address space only) for every thread,
        # the search thread included, instead of MinGW's 2 MB: deep
        # searches recurse far, singular-extension probes on top.
        target_link_options(${target} PRIVATE -Wl,--stack,16777216)
    elseif (MSVC AND type STREQUAL "EXECUTABLE")
        # The same for MSVC (default 1 MB). POSIX builds size the search
        # thread's stack when they create it (Worker in uci.cpp).
        target_link_options(${target} PRIVATE /STACK:16777216)
    endif()
endfunction()

# Fathom (MIT licence, third_party/fathom/LICENSE): Syzygy tablebase
# probing, unmodified from github.com/jdart1/Fathom at commit c9c6fef
# (2025-12-24), built with its own settings. Its headers are system
# headers to the engine, so our warnings skip them. It locks with pthreads
# outside Windows.
find_package(Threads REQUIRED)
add_library(fathom STATIC third_party/fathom/tbprobe.c)
target_include_directories(fathom SYSTEM PUBLIC third_party/fathom)
target_compile_definitions(fathom PUBLIC TB_NO_HELPER_API)
target_link_libraries(fathom PUBLIC Threads::Threads)
set_target_properties(fathom PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# Everything but the UCI front end, shared by the engine and the tests.
add_library(engine STATIC
    src/position.cpp
    src/attacks.cpp
    src/movegen.cpp
    src/eval.cpp
    src/initiative.cpp
    src/see.cpp
    src/pruning.cpp
    src/search.cpp
    src/tablebase.cpp
    src/threads.cpp
    src/guide_features.cpp
    src/labels.cpp
    src/bingo.cpp
    src/cont_log.cpp
    src/perft.cpp
    src/pgn.cpp
)
target_include_directories(engine PUBLIC src)
target_link_libraries(engine PUBLIC fathom)
engine_warnings(engine)
# The source tree's git commit, for data files to record what made them
# (labels' .params): regenerated at every build, rewritten only on change.
add_custom_target(git_hash ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/git_hash.h
            -P ${CMAKE_SOURCE_DIR}/cmake/git_hash.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/git_hash.h)
add_dependencies(engine git_hash)
target_include_directories(engine PRIVATE ${CMAKE_BINARY_DIR}/generated)
if (CONT_LOG)
    target_compile_definitions(engine PUBLIC CONT_LOG)
endif()

add_executable(chess
    src/main.cpp
    src/uci.cpp
    src/bench.cpp
    src/telemetry.cpp
)
target_link_libraries(chess PRIVATE engine)
if (TUNE)
    target_compile_definitions(chess PRIVATE TUNE)
endif()
engine_warnings(chess)

enable_testing()

add_executable(perft_tests tests/perft_tests.cpp)
target_link_libraries(perft_tests PRIVATE engine)
engine_warnings(perft_tests)
add_test(NAME perft COMMAND perft_tests)

add_executable(search_tests tests/search_tests.cpp)
target_link_libraries(search_tests PRIVATE engine)
engine_warnings(search_tests)
add_test(NAME search COMMAND search_tests)
# A directory with the 3-4-5-piece Syzygy tables turns on the tablebase
# tests that need them (about 1 GB, so not required).
set(SYZYGY_PATH "" CACHE PATH "Syzygy tables for the tests (optional)")
if (SYZYGY_PATH)
    set_tests_properties(search PROPERTIES ENVIRONMENT "FUNCOMP_SYZYGY=${SYZYGY_PATH}")
endif()
