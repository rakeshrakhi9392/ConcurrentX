#include "concurrentx/execution_context.hpp"

namespace concurrentx {

namespace {

// Per-thread execution stack. Only mutated by ReentrancyGuard on the same
// thread, so no additional synchronization is required for these locals.
thread_local ThreadPool* tls_pool = nullptr;
thread_local std::size_t tls_depth = 0;

}  // namespace

bool ExecutionContext::in_worker() noexcept {
    return tls_depth > 0;
}

std::size_t ExecutionContext::nesting_depth() noexcept {
    return tls_depth;
}

bool ExecutionContext::is_reentrant() noexcept {
    return tls_depth > 1;
}

ThreadPool* ExecutionContext::current_pool() noexcept {
    return tls_pool;
}

ReentrancyGuard::ReentrancyGuard(ThreadPool* pool) noexcept
    : previous_pool_(tls_pool) {
    tls_pool = pool;
    ++tls_depth;
}

ReentrancyGuard::~ReentrancyGuard() {
    --tls_depth;
    tls_pool = previous_pool_;
}

}  // namespace concurrentx
