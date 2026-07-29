#pragma once

#include <mutex>

namespace concurrentx {

/**
 * Mutex that may be locked recursively by the same thread and that serializes
 * concurrent callers from different worker threads.
 *
 * Use this (or another synchronization primitive) around shared mutable state
 * touched by tasks. ExecutionContext alone tracks reentrancy; it does not
 * make heap writes safe across workers.
 *
 * Models Lockable so it works with std::lock_guard / std::unique_lock.
 */
class ReentrantMutex {
public:
    ReentrantMutex() = default;

    ReentrantMutex(const ReentrantMutex&) = delete;
    ReentrantMutex& operator=(const ReentrantMutex&) = delete;

    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }
    bool try_lock() { return mutex_.try_lock(); }

private:
    std::recursive_mutex mutex_;
};

}  // namespace concurrentx
