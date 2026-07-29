cmake_minimum_required(VERSION 3.15)

project(ConcurrentX
    VERSION 0.1.0
    DESCRIPTION "Lightweight C++17 multithreaded task scheduler and thread pool"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CONCURRENTX_BUILD_TESTS "Build unit tests" ON)
option(CONCURRENTX_BUILD_BENCHMARKS "Build benchmarking harness" ON)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(ConcurrentX
    src/exceptions.cpp
    src/log.cpp
    src/debug.cpp
    src/task.cpp
    src/execution_context.cpp
    src/thread_pool.cpp
)

add_library(ConcurrentX::ConcurrentX ALIAS ConcurrentX)

target_include_directories(ConcurrentX
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(ConcurrentX PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(ConcurrentX PRIVATE /W4 /permissive-)
else()
    target_compile_options(ConcurrentX PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Optional targets
# ---------------------------------------------------------------------------
if(CONCURRENTX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CONCURRENTX_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
