#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace concurrentx {

/**
 * Move-only, type-erased work unit for the scheduler.
 *
 * Stores an arbitrary nullary callable behind a virtual interface so the
 * task queue can hold heterogeneous jobs without templates at the queue
 * boundary. Copy construction/assignment are deleted (rule of five) so
 * resource-owning callables (e.g. packaged_task, unique ownership) cannot
 * be accidentally duplicated.
 *
 * Thread-safety: a single Task instance must not be invoked concurrently
 * from multiple threads. Distinct Task objects may run in parallel.
 * Ownership transfer across threads is safe once the producing thread
 * has finished writing (happens-before via mutex / queue handoff).
 */
class Task {
public:
    Task() noexcept = default;

    /**
     * Type-erasing constructor.
     * Callable must be invocable with no arguments. Return values (if any)
     * are discarded; use std::packaged_task at the call site when a
     * std::future is required.
     */
    template <typename Callable,
              typename = std::enable_if_t<
                  !std::is_same_v<std::decay_t<Callable>, Task> &&
                  std::is_invocable_v<std::decay_t<Callable>&>>>
    explicit Task(Callable&& callable)
        : impl_(std::make_unique<Model<std::decay_t<Callable>>>(
              std::forward<Callable>(callable))) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    ~Task() = default;

    /** True when a callable is owned (not default-constructed or moved-from). */
    explicit operator bool() const noexcept { return static_cast<bool>(impl_); }

    /**
     * Invoke the stored callable exactly once from the caller's perspective
     * of readiness. Leaves the Task empty afterward so a second run() throws.
     */
    void run();

    /** Swap ownership with another Task. */
    void swap(Task& other) noexcept;

private:
    /**
     * Type-erasure concept: heap-allocated, owned exclusively via unique_ptr.
     * Virtual destructor guarantees correct cleanup of Model<T>.
     */
    struct Concept {
        virtual ~Concept() = default;
        virtual void invoke() = 0;
    };

    template <typename Callable>
    struct Model final : Concept {
        // By-value sink: lvalues copy in, rvalues move in (incl. move-only).
        // Task itself remains non-copyable so unique ownership is preserved.
        explicit Model(Callable callable) : callable_(std::move(callable)) {}

        void invoke() override { callable_(); }

        Callable callable_;
    };

    std::unique_ptr<Concept> impl_;
};

inline void swap(Task& a, Task& b) noexcept { a.swap(b); }

}  // namespace concurrentx
