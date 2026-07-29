#include "concurrentx/execution_context.hpp"

#include "concurrentx/debug.hpp"
#include "concurrentx/log.hpp"

#include <cstdlib>

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

ReentrancyGuard::ReentrancyGuard(ThreadPool* pool)
    : previous_pool_(tls_pool) {
    // Workers always pass a live pool pointer; nullptr would break
    // current_pool() observers and nested wait()/get() helping.
    CX_INVARIANT(pool != nullptr);
    tls_pool = pool;
    ++tls_depth;
    CX_INVARIANT(tls_depth > 0);
}

ReentrancyGuard::~ReentrancyGuard() {
    // Nesting must be balanced. Never throw from a destructor — abort on
    // invariant failure even when AssertMode::Throw is active.
#if CONCURRENTX_ENABLE_ASSERTS
    if (tls_depth == 0) {
        CX_LOG_ERROR << "ReentrancyGuard depth underflow";
        std::abort();
    }
#endif
    --tls_depth;
    tls_pool = previous_pool_;
#if CONCURRENTX_ENABLE_ASSERTS
    if ((tls_depth == 0) != (tls_pool == nullptr)) {
        CX_LOG_ERROR << "ReentrancyGuard TLS pool/depth mismatch";
        std::abort();
    }
#endif
}

}  // namespace concurrentx
