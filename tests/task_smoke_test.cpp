#include "concurrentx/exceptions.hpp"
#include "concurrentx/task.hpp"

#include <cassert>
#include <future>
#include <utility>

namespace {

void test_move_only_and_run() {
    int value = 0;
    concurrentx::Task task{[&value]() { value = 42; }};
    assert(static_cast<bool>(task));

    concurrentx::Task moved = std::move(task);
    assert(!task);
    assert(static_cast<bool>(moved));

    moved.run();
    assert(value == 42);
    assert(!moved);
}

void test_empty_run_throws() {
    concurrentx::Task empty;
    bool threw = false;
    try {
        empty.run();
    } catch (const concurrentx::EmptyTaskException&) {
        threw = true;
    }
    assert(threw);
}

void test_packaged_task_via_type_erasure() {
    std::packaged_task<int()> packaged{[]() { return 7; }};
    auto future = packaged.get_future();

    concurrentx::Task task{std::move(packaged)};
    task.run();
    assert(future.get() == 7);
}

void test_double_run_throws() {
    concurrentx::Task task{[]() {}};
    task.run();
    bool threw = false;
    try {
        task.run();
    } catch (const concurrentx::EmptyTaskException&) {
        threw = true;
    }
    assert(threw);
}

}  // namespace

int main() {
    test_move_only_and_run();
    test_empty_run_throws();
    test_packaged_task_via_type_erasure();
    test_double_run_throws();
    return 0;
}
