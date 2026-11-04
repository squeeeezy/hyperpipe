cmake_minimum_required(VERSION 3.20)
project(hyperpipe VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(hyperpipe_core STATIC
    src/hp_agg.c
    src/hp_cpuid.c
    src/hp_csv_scalar.c
    src/hp_filter.c
    src/hp_gzip.c
    src/hp_json_scalar.c
    src/hp_keymap.c
    src/hp_parallel.c
    src/hp_ring_buffer.c
    src/hp_summary.c
    src/hp_transcoder.c
)
target_include_directories(hyperpipe_core PUBLIC include)
target_compile_definitions(hyperpipe_core PUBLIC _POSIX_C_SOURCE=200809L _FILE_OFFSET_BITS=64)
if(APPLE)
    # macOS hides sysconf names such as _SC_NPROCESSORS_ONLN under strict POSIX
    target_compile_definitions(hyperpipe_core PUBLIC _DARWIN_C_SOURCE)
endif()
target_link_libraries(hyperpipe_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(hyperpipe_core PUBLIC psapi)
endif()

if(MSVC)
    target_compile_options(hyperpipe_core PUBLIC /W4 /WX)
else()
    target_compile_options(hyperpipe_core PUBLIC -Wall -Wextra -Wpedantic -Werror)
endif()

# The vector units are x86 only. Only they use vector instructions; runtime
# dispatch decides whether they run, so the binary stays portable across
# x86 CPUs.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    target_sources(hyperpipe_core PRIVATE
        src/hp_csv_avx2.c
        src/hp_csv_avx512.c
        src/hp_json_avx2.c
        src/hp_json_avx512.c
    )
    if(NOT MSVC)
        set_source_files_properties(src/hp_csv_avx2.c src/hp_json_avx2.c
            PROPERTIES COMPILE_OPTIONS "-mavx2;-mbmi2")
        set_source_files_properties(src/hp_csv_avx512.c src/hp_json_avx512.c
            PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx512vl;-mavx2;-mbmi2")
    endif()
endif()

add_executable(hyperpipe src/hp_main.c)
target_link_libraries(hyperpipe PRIVATE hyperpipe_core)

add_executable(bench_hyperpipe bench/bench_main.c)
target_link_libraries(bench_hyperpipe PRIVATE hyperpipe_core)

enable_testing()
file(GLOB test_sources CONFIGURE_DEPENDS tests/test_*.c)
foreach(source IN LISTS test_sources)
    get_filename_component(name ${source} NAME_WE)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE hyperpipe_core)
    target_compile_definitions(${name} PRIVATE HP_TEST_TMPDIR="${CMAKE_BINARY_DIR}/")
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
