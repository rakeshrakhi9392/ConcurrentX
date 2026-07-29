#pragma once

#include <cstddef>

namespace concurrentx {

class ThreadPool;

/**
 * Thread-local view of the pool execution stack.
 *
 * Workers install a ReentrancyGuard around every task invocation. Nested
 * helping (e.g. ThreadPool::wait running a queued task while blocked) pushes
 * additional frames so callers can detect reentrancy (nesting_depth() > 1).
 *
 * Concurrent tasks on different workers each see their own TLS context; this
 * API does not by itself protect shared heap state — pair it with
 * ReentrantMutex (or other synchronization) for that.
 */
class ExecutionContext {
public:
    /** True while the calling thread is inside a pool task (any nesting depth). */
    [[nodiscard]] static bool in_worker() noexcept;

    /**
     * Number of active ReentrancyGuard frames on this thread.
     * 0 = not in pool work; 1 = top-level task; >1 = reentrant / nested run.
     */
    [[nodiscard]] static std::size_t nesting_depth() noexcept;

    /** True when nesting_depth() > 1 (nested task run on the same thread). */
    [[nodiscard]] static bool is_reentrant() noexcept;

    /**
     * Pool that owns the innermost active guard, or nullptr when not in a
     * worker task.
     */
    [[nodiscard]] static ThreadPool* current_pool() noexcept;
};

/**
 * RAII frame that publishes ExecutionContext TLS for the current thread.
 * ThreadPool installs one around each task body (including nested help-runs).
 */
class ReentrancyGuard {
public:
    explicit ReentrancyGuard(ThreadPool* pool);
    ~ReentrancyGuard();

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;
    ReentrancyGuard(ReentrancyGuard&&) = delete;
    ReentrancyGuard& operator=(ReentrancyGuard&&) = delete;

private:
    ThreadPool* previous_pool_;
};

}  // namespace concurrentx
