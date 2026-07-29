#pragma once

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include "concurrentx/task.hpp"

namespace concurrentx {

/**
 * Fixed-size worker pool with RAII thread lifecycle management.
 *
 * Workers are owned in a std::vector<std::thread> inside the pimpl and are
 * joined in the destructor. The internal task queue is protected by a mutex
 * and condition_variable; tasks run outside the lock so re-entrant submit()
 * from a running task cannot deadlock.
 *
 * Reentrancy: each task runs under a ReentrancyGuard (TLS ExecutionContext).
 * Prefer wait()/get() over raw future.get() when blocking on nested work from
 * inside a worker — they help-run queued tasks and avoid self-deadlock on a
 * single-thread pool.
 */
class ThreadPool {
public:
    /**
     * @param thread_count Number of worker threads. Zero selects
     *        std::thread::hardware_concurrency() (at least 1).
     * @param max_queue_size Soft capacity for the internal queue; zero means
     *        unbounded. Overflow throws TaskQueueOverflowException.
     */
    explicit ThreadPool(std::size_t thread_count = 0,
                        std::size_t max_queue_size = 0);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * RAII shutdown: signals stop, drains tasks already in the queue, then
     * joins all workers. Safe to call stop() beforehand; join is idempotent
     * with respect to an earlier stop().
     */
    ~ThreadPool();

    /**
     * Request cooperative shutdown. Workers finish queued work then exit;
     * further submit() / enqueue calls throw SchedulerStoppedException.
     */
    void stop();

    [[nodiscard]] bool is_stopped() const noexcept;
    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] std::size_t queued_tasks() const noexcept;

    /**
     * Enqueue an arbitrary callable. Returns a future for the result (or
     * for an exception thrown by the callable). Move-only callables are
     * supported via shared_ptr<packaged_task> + type-erased Task.
     */
    template <typename Callable, typename... Args>
    auto submit(Callable&& callable, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Callable>,
                                            std::decay_t<Args>...>>;

    /**
     * Block until future is ready, help-running queued tasks on this pool
     * while waiting. Safe to call from a worker of this pool (including a
     * single-thread pool) when waiting on nested submit() work.
     */
    template <typename T>
    T wait(std::future<T>& future);

    /** Alias for wait() — prefer over future.get() from inside pool tasks. */
    template <typename T>
    T get(std::future<T>& future);

private:
    struct Impl;
    // Pimpl keeps mutex / condition_variable / queue out of the header.
    // unique_ptr ownership: Impl is incomplete here, destroyed in .cpp only
    // after every worker has been joined.
    std::unique_ptr<Impl> impl_;

    void enqueue(Task task);

    /**
     * Pop and run at most one queued task under a nested ReentrancyGuard.
     * Returns true if a task was executed.
     */
    bool try_run_queued_task();
};

}  // namespace concurrentx

#include "concurrentx/thread_pool.inl"
