# GoogleTest integration suite + existing lightweight smoke runners.

include(FetchContent)

# Pin a known release so CI/local builds stay reproducible.
set(GOOGLETEST_VERSION "1.14.0")
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v${GOOGLETEST_VERSION}.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Match the CRT of ConcurrentX (MD/MDd) on MSVC; otherwise link fails.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ---------------------------------------------------------------------------
# Existing lightweight assert-based smoke / focused suites
# ---------------------------------------------------------------------------

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

# ---------------------------------------------------------------------------
# GoogleTest integration / verification suite (Step 6)
# ---------------------------------------------------------------------------

add_executable(concurrentx_integration_tests
    integration_test.cpp
)

target_link_libraries(concurrentx_integration_tests
    PRIVATE
        ConcurrentX::ConcurrentX
        GTest::gtest
)

target_compile_features(concurrentx_integration_tests PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(concurrentx_integration_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(concurrentx_integration_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

gtest_discover_tests(concurrentx_integration_tests
    DISCOVERY_TIMEOUT 60
    PROPERTIES
        TIMEOUT 120
        LABELS "integration"
)
