#pragma once

#include "concurrentx/exceptions.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace concurrentx {

template <typename Callable, typename... Args>
auto ThreadPool::submit(Callable&& callable, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<Callable>,
                                        std::decay_t<Args>...>> {
    using Result =
        std::invoke_result_t<std::decay_t<Callable>, std::decay_t<Args>...>;

    // Capture callable + args by move into a nullary packaged_task so the
    // queue only stores move-only Task objects. std::apply keeps argument
    // values (including move-only ones) intact until the worker runs.
    auto packaged = std::make_shared<std::packaged_task<Result()>>(
        [func = std::decay_t<Callable>(std::forward<Callable>(callable)),
         argv = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(std::move(func), std::move(argv));
        });

    std::future<Result> result = packaged->get_future();
    enqueue(Task{[packaged]() { (*packaged)(); }});
    return result;
}

template <typename T>
T ThreadPool::wait(std::future<T>& future) {
    // Help-run pending work so a worker blocked on nested submit() cannot
    // deadlock against an empty free-thread set (classic single-pool reentry).
    while (future.wait_for(std::chrono::milliseconds(0)) !=
           std::future_status::ready) {
        if (!try_run_queued_task()) {
            // Queue empty or pool draining: block briefly for progress.
            if (future.wait_for(std::chrono::milliseconds(1)) ==
                std::future_status::ready) {
                break;
            }
        }
    }
    return future.get();
}

template <typename T>
T ThreadPool::get(std::future<T>& future) {
    return wait(future);
}

}  // namespace concurrentx
