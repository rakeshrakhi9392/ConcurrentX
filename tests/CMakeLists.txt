# Smoke tests for Task, ThreadPool, reentrancy, and error-handling / logging.
# GoogleTest wiring arrives with the full unit-test suite.

add_executable(concurrentx_task_smoke
    task_smoke_test.cpp
)
target_link_libraries(concurrentx_task_smoke PRIVATE ConcurrentX::ConcurrentX)
add_test(NAME concurrentx_task_smoke COMMAND concurrentx_task_smoke)

add_executable(concurrentx_thread_pool_smoke
    thread_pool_smoke_test.cpp
)
target_link_libraries(concurrentx_thread_pool_smoke PRIVATE ConcurrentX::ConcurrentX)
add_test(NAME concurrentx_thread_pool_smoke COMMAND concurrentx_thread_pool_smoke)

add_executable(concurrentx_reentrancy_test
    reentrancy_test.cpp
)
target_link_libraries(concurrentx_reentrancy_test PRIVATE ConcurrentX::ConcurrentX)
add_test(NAME concurrentx_reentrancy_test COMMAND concurrentx_reentrancy_test)

add_executable(concurrentx_error_handling_test
    error_handling_test.cpp
)
target_link_libraries(concurrentx_error_handling_test PRIVATE ConcurrentX::ConcurrentX)
# Keep asserts enabled for this target even in Release so Throw-mode coverage runs.
target_compile_definitions(concurrentx_error_handling_test
    PRIVATE CONCURRENTX_ENABLE_ASSERTS=1
)
add_test(NAME concurrentx_error_handling_test COMMAND concurrentx_error_handling_test)
