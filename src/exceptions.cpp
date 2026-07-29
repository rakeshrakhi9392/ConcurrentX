#include "concurrentx/exceptions.hpp"

#include <string>
#include <utility>

namespace concurrentx {

SchedulerException::SchedulerException(std::string message)
    : message_(std::move(message)) {}

SchedulerException::~SchedulerException() = default;

const char* SchedulerException::what() const noexcept {
    return message_.c_str();
}

SchedulerStoppedException::SchedulerStoppedException()
    : SchedulerException("scheduler has been stopped") {}

SchedulerStoppedException::SchedulerStoppedException(std::string message)
    : SchedulerException(std::move(message)) {}

TaskQueueOverflowException::TaskQueueOverflowException()
    : SchedulerException("task queue capacity exceeded"), capacity_(0) {}

TaskQueueOverflowException::TaskQueueOverflowException(std::size_t capacity)
    : SchedulerException("task queue capacity exceeded (" +
                         std::to_string(capacity) + ")"),
      capacity_(capacity) {}

TaskQueueOverflowException::TaskQueueOverflowException(std::string message,
                                                       std::size_t capacity)
    : SchedulerException(std::move(message)), capacity_(capacity) {}

std::size_t TaskQueueOverflowException::capacity() const noexcept {
    return capacity_;
}

EmptyTaskException::EmptyTaskException()
    : SchedulerException("attempted to run an empty or moved-from Task") {}

}  // namespace concurrentx
