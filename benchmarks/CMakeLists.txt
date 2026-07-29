# Benchmarking harness: throughput / latency CSV profiling across thread counts.

add_executable(concurrentx_bench
    bench_main.cpp
)

target_link_libraries(concurrentx_bench PRIVATE ConcurrentX::ConcurrentX)

target_compile_features(concurrentx_bench PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(concurrentx_bench PRIVATE /W4 /permissive-)
else()
    target_compile_options(concurrentx_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Optional convenience target:
#   cmake --build build --target run_bench
add_custom_target(run_bench
    COMMAND concurrentx_bench
            --threads 1,2,4,8,16
            --tasks 10000
            --workload all
            --output "${CMAKE_BINARY_DIR}/bench_results.csv"
    DEPENDS concurrentx_bench
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running ConcurrentX benchmark harness (CSV -> build/bench_results.csv)"
)
