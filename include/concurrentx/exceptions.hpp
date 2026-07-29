#pragma once

#include <cstddef>
#include <exception>
#include <string>

namespace concurrentx {

/**
 * Custom exception hierarchy for ConcurrentX.
 *
 *   std::exception
 *     └── SchedulerException            (base; owns message by value)
 *           ├── SchedulerStoppedException
 *           ├── TaskQueueOverflowException
 *           ├── EmptyTaskException
 *           └── AssertionFailureException
 *
 * Task-body failures are not wrapped in this hierarchy: submit() uses
 * std::packaged_task so the original exception is stored in the associated
 * std::future and rethrown by future.get() / ThreadPool::wait() on the
 * calling thread.
 */

/**
 * Base type for all scheduler / thread-pool failures.
 * Derived types carry specific failure modes; messages are owned by value.
 */
class SchedulerException : public std::exception {
public:
    explicit SchedulerException(std::string message);
    SchedulerException(const SchedulerException&) = default;
    SchedulerException& operator=(const SchedulerException&) = default;
    SchedulerException(SchedulerException&&) noexcept = default;
    SchedulerException& operator=(SchedulerException&&) noexcept = default;
    ~SchedulerException() override;

    const char* what() const noexcept override;

private:
    std::string message_;
};

/** Thrown when submit / enqueue is attempted after shutdown has begun. */
class SchedulerStoppedException : public SchedulerException {
public:
    SchedulerStoppedException();
    explicit SchedulerStoppedException(std::string message);
};

/** Thrown when a bounded task queue refuses an additional task. */
class TaskQueueOverflowException : public SchedulerException {
public:
    TaskQueueOverflowException();
    explicit TaskQueueOverflowException(std::size_t capacity);
    TaskQueueOverflowException(std::string message, std::size_t capacity);

    std::size_t capacity() const noexcept;

private:
    std::size_t capacity_{0};
};

/** Thrown when a Task is invoked in an empty / moved-from state. */
class EmptyTaskException : public SchedulerException {
public:
    EmptyTaskException();
};

/**
 * Thrown by CX_ASSERT* when AssertMode::Throw is active (or a custom
 * handler elects to throw). Not used for normal task failures.
 */
class AssertionFailureException : public SchedulerException {
public:
    explicit AssertionFailureException(std::string message);
};

}  // namespace concurrentx
