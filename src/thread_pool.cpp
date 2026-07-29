#include "concurrentx/thread_pool.hpp"

#include "concurrentx/exceptions.hpp"
#include "concurrentx/execution_context.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace concurrentx {

namespace {

std::size_t resolve_thread_count(std::size_t thread_count) {
    if (thread_count != 0) {
        return thread_count;
    }
    const unsigned hc = std::thread::hardware_concurrency();
    return hc == 0 ? std::size_t{1} : static_cast<std::size_t>(hc);
}

}  // namespace

/**
 * Private implementation (pimpl). Shared mutable state is guarded by mutex_;
 * workers sleep on cv_ until a task arrives or stop_ is set.
 *
 * Lock order (single lock today): mutex_ only. Document additional locks
 * here if the design grows to avoid ABBA deadlocks.
 *
 * Ownership: ThreadPool holds Impl via unique_ptr. Worker threads capture
 * Impl* (not ThreadPool*) so they never outlive the Impl they observe —
 * workers are always joined before Impl is destroyed.
 */
struct ThreadPool::Impl {
    explicit Impl(ThreadPool* owner,
                  std::size_t thread_count,
                  std::size_t max_queue_size)
        : owner_(owner),
          max_queue_size_(max_queue_size),
          thread_count_(resolve_thread_count(thread_count)) {}

    /**
     * Worker entry point. Pop under the mutex, then run outside the lock so
     * a task that re-submits to this pool cannot deadlock on mutex_.
     * ReentrancyGuard publishes TLS ExecutionContext for the task body.
     */
    void worker_main() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Spurious wakeups are filtered by the predicate.
                // Exit only when stop is set and the queue has been drained
                // so already-submitted futures still complete.
                cv_.wait(lock, [this] {
                    return stop_.load(std::memory_order_relaxed) ||
                           !queue_.empty();
                });
                if (queue_.empty()) {
                    return;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }

            run_task(std::move(task));
        }
    }

    void run_task(Task task) {
        ReentrancyGuard guard(owner_);
        // Exceptions from packaged_task land in the associated future.
        // Absorb anything else so one bad task cannot terminate a worker.
        try {
            task.run();
        } catch (...) {
        }
    }

    /** Push under lock. Caller must hold mutex_. Returns false on overflow. */
    bool try_push_unlocked(Task task) {
        if (max_queue_size_ != 0 && queue_.size() >= max_queue_size_) {
            return false;
        }
        queue_.push_back(std::move(task));
        return true;
    }

    ThreadPool* owner_{nullptr};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
    std::size_t max_queue_size_{0};
    std::size_t thread_count_{0};
};

ThreadPool::ThreadPool(std::size_t thread_count, std::size_t max_queue_size)
    : impl_(std::make_unique<Impl>(this, thread_count, max_queue_size)) {
    // Spawn workers after Impl is fully constructed. If emplace_back throws
    // mid-loop, stop + join the threads already started before rethrowing so
    // no joinable thread is abandoned (RAII / no thread leak).
    try {
        impl_->workers_.reserve(impl_->thread_count_);
        for (std::size_t i = 0; i < impl_->thread_count_; ++i) {
            Impl* self = impl_.get();
            impl_->workers_.emplace_back([self] { self->worker_main(); });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex_);
            impl_->stop_.store(true, std::memory_order_release);
        }
        impl_->cv_.notify_all();
        for (std::thread& worker : impl_->workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadPool::~ThreadPool() {
    if (!impl_) {
        return;
    }
    // Cooperative stop, then join every worker. Joining here guarantees that
    // no worker still touches mutex_/queue_/cv_ when unique_ptr destroys Impl.
    stop();
    for (std::thread& worker : impl_->workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::stop() {
    if (!impl_) {
        return;
    }
    {
        // Publish stop under the same mutex workers wait on so the flag and
        // the condition_variable wake-up are not reordered relative to wait.
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->stop_.store(true, std::memory_order_release);
    }
    impl_->cv_.notify_all();
}

bool ThreadPool::is_stopped() const noexcept {
    return !impl_ || impl_->stop_.load(std::memory_order_acquire);
}

std::size_t ThreadPool::thread_count() const noexcept {
    return impl_ ? impl_->thread_count_ : 0;
}

std::size_t ThreadPool::queued_tasks() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->queue_.size();
}

void ThreadPool::enqueue(Task task) {
    // Fast path: reject without contending if shutdown already published.
    if (!impl_ || impl_->stop_.load(std::memory_order_acquire)) {
        throw SchedulerStoppedException{};
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        // Re-check under lock: stop() may have won the race after the load.
        if (impl_->stop_.load(std::memory_order_relaxed)) {
            throw SchedulerStoppedException{};
        }
        if (!impl_->try_push_unlocked(std::move(task))) {
            throw TaskQueueOverflowException{impl_->max_queue_size_};
        }
    }
    // Notify outside the critical section: the woken worker only needs the
    // mutex after we have released it, reducing lock hold time.
    impl_->cv_.notify_one();
}

bool ThreadPool::try_run_queued_task() {
    if (!impl_) {
        return false;
    }

    Task task;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (impl_->queue_.empty()) {
            return false;
        }
        task = std::move(impl_->queue_.front());
        impl_->queue_.pop_front();
    }

    // Nested ReentrancyGuard: nesting_depth() becomes > 1 so clients can
    // observe is_reentrant() while this help-run executes.
    impl_->run_task(std::move(task));
    return true;
}

}  // namespace concurrentx
