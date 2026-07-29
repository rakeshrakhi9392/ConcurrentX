#include "concurrentx/execution_context.hpp"
#include "concurrentx/reentrant_mutex.hpp"
#include "concurrentx/thread_pool.hpp"

#include <cassert>
#include <cstdint>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void test_execution_context_outside_pool() {
    assert(!concurrentx::ExecutionContext::in_worker());
    assert(concurrentx::ExecutionContext::nesting_depth() == 0);
    assert(!concurrentx::ExecutionContext::is_reentrant());
    assert(concurrentx::ExecutionContext::current_pool() == nullptr);
}

void test_execution_context_inside_task() {
    concurrentx::ThreadPool pool{2};
    concurrentx::ThreadPool* seen_pool = nullptr;
    std::size_t depth = 0;
    bool in_worker = false;

    auto future = pool.submit([&] {
        in_worker = concurrentx::ExecutionContext::in_worker();
        depth = concurrentx::ExecutionContext::nesting_depth();
        seen_pool = concurrentx::ExecutionContext::current_pool();
        assert(!concurrentx::ExecutionContext::is_reentrant());
    });
    future.get();

    assert(in_worker);
    assert(depth == 1);
    assert(seen_pool == &pool);
}

void test_single_worker_reentrant_wait_no_deadlock() {
    // One worker: nested submit + raw future.get() would deadlock.
    // wait() help-runs the inner task on the same thread.
    concurrentx::ThreadPool pool{1};

    auto outer = pool.submit([&pool] {
        assert(concurrentx::ExecutionContext::nesting_depth() == 1);

        std::size_t nested_depth = 0;
        bool nested_reentrant = false;
        auto inner = pool.submit([&] {
            nested_depth = concurrentx::ExecutionContext::nesting_depth();
            nested_reentrant = concurrentx::ExecutionContext::is_reentrant();
            return 7;
        });

        const int value = pool.wait(inner);
        assert(nested_depth == 2);
        assert(nested_reentrant);
        return value;
    });

    assert(outer.get() == 7);
}

void test_reentrant_mutex_same_thread_nesting() {
    concurrentx::ReentrantMutex mutex;
    int shared = 0;
    concurrentx::ThreadPool pool{2};

    auto future = pool.submit([&] {
        std::lock_guard<concurrentx::ReentrantMutex> outer(mutex);
        // Same-thread re-lock must not deadlock (recursive_mutex).
        {
            std::lock_guard<concurrentx::ReentrantMutex> inner(mutex);
            ++shared;
        }
        ++shared;
    });
    future.get();
    assert(shared == 2);
}

void test_shared_state_no_corruption_under_contention() {
    // Many workers mutate a non-atomic int under ReentrantMutex. Without the
    // lock this would race; with it the final count must match the task count.
    concurrentx::ReentrantMutex mutex;
    int shared = 0;
    constexpr int kTasks = 2000;
    concurrentx::ThreadPool pool{8};

    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<std::size_t>(kTasks));

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.submit([&mutex, &shared] {
            std::lock_guard<concurrentx::ReentrantMutex> lock(mutex);
            // Non-atomic read-modify-write: safe only while holding the mutex.
            const int tmp = shared;
            // Encourage interleaving if the lock were missing.
            if ((tmp & 0x3f) == 0) {
                std::this_thread::yield();
            }
            shared = tmp + 1;
        }));
    }

    for (auto& future : futures) {
        future.get();
    }
    assert(shared == kTasks);
}

void test_reentrant_shared_state_across_nested_help_run() {
    // Outer task holds the mutex, then wait() help-runs an inner task that
    // also locks the same ReentrantMutex. Serialization + recursion must
    // both succeed without deadlock or lost updates.
    concurrentx::ReentrantMutex mutex;
    int shared = 0;
    concurrentx::ThreadPool pool{1};

    auto outer = pool.submit([&pool, &mutex, &shared] {
        std::lock_guard<concurrentx::ReentrantMutex> outer_lock(mutex);
        ++shared;

        auto inner = pool.submit([&mutex, &shared] {
            assert(concurrentx::ExecutionContext::is_reentrant());
            std::lock_guard<concurrentx::ReentrantMutex> inner_lock(mutex);
            ++shared;
            return shared;
        });

        const int after_inner = pool.wait(inner);
        assert(after_inner == 2);
        ++shared;
        return shared;
    });

    assert(outer.get() == 3);
    assert(shared == 3);
}

void test_concurrent_reentrant_functions_isolated_per_thread() {
    // Same logical function runs on many workers. TLS context is per-thread
    // (no cross-thread corruption of nesting_depth). Shared accumulator is
    // protected by ReentrantMutex so concurrent updates stay consistent.
    concurrentx::ReentrantMutex mutex;
    std::uint64_t checksum = 0;
    constexpr int kTasks = 256;
    concurrentx::ThreadPool pool{4};

    auto reentrant_work = [&](int id) {
        assert(concurrentx::ExecutionContext::in_worker());
        assert(concurrentx::ExecutionContext::current_pool() == &pool);
        assert(concurrentx::ExecutionContext::nesting_depth() >= 1);

        concurrentx::ThreadPool* pool_ptr =
            concurrentx::ExecutionContext::current_pool();
        auto nested = pool_ptr->submit([] {
            // May run as a help-run (depth > 1) or on another worker (depth == 1).
            assert(concurrentx::ExecutionContext::in_worker());
            assert(concurrentx::ExecutionContext::nesting_depth() >= 1);
            return 0;
        });
        (void)pool_ptr->wait(nested);

        std::lock_guard<concurrentx::ReentrantMutex> lock(mutex);
        checksum += static_cast<std::uint64_t>(id) + 1u;
    };

    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<std::size_t>(kTasks));
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.submit([&, i] { reentrant_work(i); }));
    }
    for (auto& future : futures) {
        future.get();
    }

    // Sum 1..kTasks
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kTasks) *
        static_cast<std::uint64_t>(kTasks + 1) / 2u;
    assert(checksum == expected);
}

}  // namespace

int main() {
    test_execution_context_outside_pool();
    test_execution_context_inside_task();
    test_single_worker_reentrant_wait_no_deadlock();
    test_reentrant_mutex_same_thread_nesting();
    test_shared_state_no_corruption_under_contention();
    test_reentrant_shared_state_across_nested_help_run();
    test_concurrent_reentrant_functions_isolated_per_thread();
    return 0;
}
