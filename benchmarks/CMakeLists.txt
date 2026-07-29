# Benchmark harness placeholder — full CSV profiling lands in a later step.

add_executable(concurrentx_bench
    bench_main.cpp
)

target_link_libraries(concurrentx_bench PRIVATE ConcurrentX::ConcurrentX)
