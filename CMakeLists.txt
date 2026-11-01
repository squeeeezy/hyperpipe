cmake_minimum_required(VERSION 3.20)
project(hyperpipe C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX /O2)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -O3)
endif()

include_directories(include)

# Static library for core engine
set(HYPERPIPE_SOURCES
    src/hp_ring_buffer.c
    src/hp_cpuid.c
    src/hp_csv_scalar.c
    src/hp_csv_avx2.c
    src/hp_csv_avx512.c
    src/hp_json_scalar.c
    src/hp_json_avx2.c
    src/hp_json_avx512.c
    src/hp_filter.c
    src/hp_transcoder.c
)

add_library(hyperpipe_core STATIC ${HYPERPIPE_SOURCES})

# Target-specific SIMD instruction flags for GNU/Clang
if(NOT MSVC)
    set_source_files_properties(src/hp_csv_avx2.c src/hp_json_avx2.c
        PROPERTIES COMPILE_FLAGS "-mavx2 -mbmi2")
    set_source_files_properties(src/hp_csv_avx512.c src/hp_json_avx512.c
        PROPERTIES COMPILE_FLAGS "-mavx512f -mavx512bw -mavx512vl -mavx2 -mbmi2")
endif()

# CLI Executable
add_executable(hyperpipe src/hp_main.c)
target_link_libraries(hyperpipe PRIVATE hyperpipe_core)

# Tests
enable_testing()

add_executable(test_rfc4180 tests/test_rfc4180.c)
target_link_libraries(test_rfc4180 PRIVATE hyperpipe_core)
add_test(NAME test_rfc4180 COMMAND test_rfc4180)

add_executable(test_ring_buffer tests/test_ring_buffer.c)
target_link_libraries(test_ring_buffer PRIVATE hyperpipe_core)
add_test(NAME test_ring_buffer COMMAND test_ring_buffer)

add_executable(test_differential tests/test_differential.c)
target_link_libraries(test_differential PRIVATE hyperpipe_core)
add_test(NAME test_differential COMMAND test_differential)

add_executable(test_filter tests/test_filter.c)
target_link_libraries(test_filter PRIVATE hyperpipe_core)
add_test(NAME test_filter COMMAND test_filter)

add_executable(test_roundtrip tests/test_roundtrip.c)
target_link_libraries(test_roundtrip PRIVATE hyperpipe_core)
add_test(NAME test_roundtrip COMMAND test_roundtrip)

# Benchmark harness
add_executable(bench_hyperpipe bench/bench_main.c)
target_link_libraries(bench_hyperpipe PRIVATE hyperpipe_core)
