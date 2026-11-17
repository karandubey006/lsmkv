cmake_minimum_required(VERSION 3.25)
project(lsmkv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LSMKV_BUILD_TESTS "Build the lsmkv test suite" ON)
option(LSMKV_WERROR "Treat compiler warnings as errors" OFF)
option(LSMKV_BUILD_BENCH "Build the microbenchmarks (Google Benchmark)" OFF)
set(LSMKV_SANITIZE "" CACHE STRING
    "Sanitizers to build with, e.g. 'address,undefined' or 'thread' (empty = none)")

# Sanitizers apply to everything, including GoogleTest, so every object in a
# binary is instrumented the same way. Must come before FetchContent below.
if(LSMKV_SANITIZE)
  add_compile_options(-fsanitize=${LSMKV_SANITIZE} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all -O1)
  add_link_options(-fsanitize=${LSMKV_SANITIZE})
endif()

# Warning flags apply only to lsmkv's own targets.
add_library(lsmkv_warnings INTERFACE)
target_compile_options(lsmkv_warnings INTERFACE -Wall -Wextra -Wpedantic)
if(LSMKV_WERROR)
  target_compile_options(lsmkv_warnings INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# The engine. Depends on the C++ standard library only.
# ---------------------------------------------------------------------------
add_library(lsmkv STATIC
  src/db/db.cc
  src/db/dbformat.cc
  src/db/map_db.cc
  src/db/write_batch.cc
  src/env/env.cc
  src/env/posix_env.cc
  src/memtable/arena.cc
  src/memtable/memtable.cc
  src/util/coding.cc
  src/util/crc32c.cc
  src/util/status.cc
  src/wal/wal.cc
)
target_include_directories(lsmkv
  PUBLIC  ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(lsmkv PRIVATE lsmkv_warnings)

# Test-only Env that simulates power loss. Kept out of the engine library.
add_library(lsmkv_fault_env STATIC src/env/fault_injection_env.cc)
target_include_directories(lsmkv_fault_env PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lsmkv_fault_env PUBLIC lsmkv PRIVATE lsmkv_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LSMKV_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM  # its headers are -isystem, so their warnings can't fail our -Werror
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(lsmkv_tests
    tests/arena_test.cc
    tests/coding_test.cc
    tests/crc32c_test.cc
    tests/db_test.cc
    tests/dbformat_test.cc
    tests/differential_test.cc
    tests/env_test.cc
    tests/fault_injection_env_test.cc
    tests/memtable_test.cc
    tests/skiplist_test.cc
    tests/status_test.cc
    tests/wal_test.cc
    tests/write_batch_test.cc
  )
  # Tests may include internal headers (env/env.h, util/coding.h, ...).
  target_include_directories(lsmkv_tests PRIVATE ${PROJECT_SOURCE_DIR}/src
                                                 ${PROJECT_SOURCE_DIR}/tests)
  # lsmkv_fault_env brings in lsmkv; naming both makes the macOS linker warn.
  target_link_libraries(lsmkv_tests PRIVATE lsmkv_fault_env lsmkv_warnings GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(lsmkv_tests DISCOVERY_MODE PRE_TEST)
endif()

# ---------------------------------------------------------------------------
# Microbenchmarks (on in the release preset; numbers only mean anything there)
# ---------------------------------------------------------------------------
if(LSMKV_BUILD_BENCH)
  include(FetchContent)
  FetchContent_Declare(googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(memtable_bench bench/memtable_bench.cc)
  target_include_directories(memtable_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(memtable_bench PRIVATE lsmkv lsmkv_warnings benchmark::benchmark)
endif()
