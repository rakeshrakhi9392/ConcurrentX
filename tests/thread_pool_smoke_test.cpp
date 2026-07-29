#include "concurrentx/exceptions.hpp"
#include "concurrentx/thread_pool.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void test_submit_returns_result() {
    concurrentx::ThreadPool pool{2};
    auto future = pool.submit([] { return 42; });
    assert(future.get() == 42);
}

void test_concurrent_submissions() {
    concurrentx::ThreadPool pool{4};
    constexpr int kTasks = 64;
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (auto& future : futures) {
        future.get();
    }
    assert(counter.load() == kTasks);
}

void test_raii_joins_workers() {
    // Leaving scope must stop and join without hanging or leaking threads.
    {
        concurrentx::ThreadPool pool{2};
        auto future = pool.submit([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return 1;
        });
        assert(future.get() == 1);
        assert(pool.thread_count() == 2);
    }
}

void test_stop_rejects_new_work() {
    concurrentx::ThreadPool pool{1};
    pool.stop();
    assert(pool.is_stopped());

    bool threw = false;
    try {
        (void)pool.submit([] { return 0; });
    } catch (const concurrentx::SchedulerStoppedException&) {
        threw = true;
    }
    assert(threw);
}

void test_bounded_queue_overflow() {
    concurrentx::ThreadPool pool{1, /*max_queue_size=*/1};

    // Handshake so the worker is known to be busy before we fill the queue.
    std::promise<void> started;
    std::promise<void> release;
    auto started_fut = started.get_future();
    auto release_fut = release.get_future();

    auto blocked = pool.submit([&started, &release_fut] {
        started.set_value();
        release_fut.wait();
        return 0;
    });
    started_fut.wait();

    // Worker is occupied; this occupies the single queue slot.
    auto queued = pool.submit([] { return 1; });

    bool threw = false;
    try {
        (void)pool.submit([] { return 2; });
    } catch (const concurrentx::TaskQueueOverflowException& ex) {
        threw = true;
        assert(ex.capacity() == 1);
    }
    assert(threw);

    release.set_value();
    assert(blocked.get() == 0);
    assert(queued.get() == 1);
}

void test_exception_propagates_via_future() {
    concurrentx::ThreadPool pool{1};
    auto future = pool.submit([]() -> int {
        throw std::runtime_error("boom");
    });

    bool threw = false;
    try {
        (void)future.get();
    } catch (const std::runtime_error& ex) {
        threw = true;
        assert(std::string(ex.what()) == "boom");
    }
    assert(threw);
}

void test_reentrant_submit_from_task() {
    // Running a task that submits more work must not deadlock (run outside lock).
    concurrentx::ThreadPool pool{2};
    auto outer = pool.submit([&pool] {
        auto inner = pool.submit([] { return 9; });
        return pool.wait(inner);
    });
    assert(outer.get() == 9);
}

}  // namespace

int main() {
    test_submit_returns_result();
    test_concurrent_submissions();
    test_raii_joins_workers();
    test_stop_rejects_new_work();
    test_bounded_queue_overflow();
    test_exception_propagates_via_future();
    test_reentrant_submit_from_task();
    return 0;
}
