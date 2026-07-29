#include "concurrentx/task.hpp"

#include "concurrentx/exceptions.hpp"

#include <utility>

namespace concurrentx {

void Task::run() {
    // Transfer ownership out of impl_ before invoke so a re-entrant or
    // exceptional path cannot observe a half-run Task, and a second run()
    // always hits EmptyTaskException.
    std::unique_ptr<Concept> local = std::move(impl_);
    if (!local) {
        throw EmptyTaskException{};
    }
    local->invoke();
}

void Task::swap(Task& other) noexcept {
    using std::swap;
    swap(impl_, other.impl_);
}

}  // namespace concurrentx
