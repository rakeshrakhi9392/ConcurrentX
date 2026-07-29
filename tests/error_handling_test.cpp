#include "concurrentx/debug.hpp"
#include "concurrentx/exceptions.hpp"
#include "concurrentx/log.hpp"
#include "concurrentx/task.hpp"
#include "concurrentx/thread_pool.hpp"

#include <cassert>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct LogCapture {
    std::mutex mutex;
    std::vector<std::pair<concurrentx::LogLevel, std::string>> lines;

    void install() {
        concurrentx::Logger::set_sink(
            [this](concurrentx::LogLevel level, const std::string& message) {
                std::lock_guard<std::mutex> lock(mutex);
                lines.emplace_back(level, message);
            });
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        lines.clear();
    }

    bool contains(concurrentx::LogLevel level, const std::string& fragment) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& line : lines) {
            if (line.first == level &&
                line.second.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

void test_exception_hierarchy_what() {
    concurrentx::SchedulerStoppedException stopped;
    assert(std::string(stopped.what()).find("stopped") != std::string::npos);

    concurrentx::TaskQueueOverflowException overflow{8};
    assert(overflow.capacity() == 8);
    assert(std::string(overflow.what()).find("8") != std::string::npos);

    concurrentx::EmptyTaskException empty;
    assert(std::string(empty.what()).find("empty") != std::string::npos);

    concurrentx::AssertionFailureException assert_ex{"invariant broken"};
    assert(std::string(assert_ex.what()) == "invariant broken");

    // All scheduler errors are catchable via the base type.
    try {
        throw concurrentx::SchedulerStoppedException{};
    } catch (const concurrentx::SchedulerException& ex) {
        assert(std::string(ex.what()).find("stopped") != std::string::npos);
    }
}

void test_task_exception_propagates_via_future() {
    concurrentx::ThreadPool pool{1};
    auto future = pool.submit([]() -> int {
        throw std::logic_error("task failed");
    });

    bool saw = false;
    try {
        (void)future.get();
    } catch (const std::logic_error& ex) {
        saw = true;
        assert(std::string(ex.what()) == "task failed");
    }
    assert(saw);
}

void test_nested_wait_propagates_inner_exception() {
    concurrentx::ThreadPool pool{2};
    auto outer = pool.submit([&pool] {
        auto inner = pool.submit([]() -> int {
            throw std::runtime_error("inner boom");
        });
        return pool.wait(inner);
    });

    bool saw = false;
    try {
        (void)outer.get();
    } catch (const std::runtime_error& ex) {
        saw = true;
        assert(std::string(ex.what()) == "inner boom");
    }
    assert(saw);
}

void test_empty_task_exception_propagates_via_future() {
    concurrentx::ThreadPool pool{1};
    // EmptyTaskException thrown inside the callable is captured by
    // packaged_task and rethrown from future.get().
    auto future = pool.submit([] {
        concurrentx::Task empty;
        empty.run();
    });

    bool saw = false;
    try {
        future.get();
    } catch (const concurrentx::EmptyTaskException&) {
        saw = true;
    }
    assert(saw);
}

void test_logger_level_filtering() {
    LogCapture capture;
    capture.install();
    concurrentx::Logger::set_level(concurrentx::LogLevel::Warn);

    CX_LOG_ERROR << "err-visible";
    CX_LOG_WARN << "warn-visible";
    CX_LOG_INFO << "info-hidden";
    CX_LOG_DEBUG << "debug-hidden";

    assert(capture.contains(concurrentx::LogLevel::Error, "err-visible"));
    assert(capture.contains(concurrentx::LogLevel::Warn, "warn-visible"));
    assert(!capture.contains(concurrentx::LogLevel::Info, "info-hidden"));
    assert(!capture.contains(concurrentx::LogLevel::Debug, "debug-hidden"));

    concurrentx::Logger::set_sink({});
    concurrentx::Logger::set_level(concurrentx::LogLevel::Warn);
}

void test_logger_off_silences_all() {
    LogCapture capture;
    capture.install();
    concurrentx::Logger::set_level(concurrentx::LogLevel::Off);

    CX_LOG_ERROR << "should-not-appear";
    assert(capture.lines.empty());

    concurrentx::Logger::set_sink({});
    concurrentx::Logger::set_level(concurrentx::LogLevel::Warn);
}

void test_stopped_pool_logs_and_throws() {
    LogCapture capture;
    capture.install();
    concurrentx::Logger::set_level(concurrentx::LogLevel::Warn);

    concurrentx::ThreadPool pool{1};
    pool.stop();

    bool threw = false;
    try {
        (void)pool.submit([] { return 1; });
    } catch (const concurrentx::SchedulerStoppedException&) {
        threw = true;
    }
    assert(threw);
    assert(capture.contains(concurrentx::LogLevel::Warn, "scheduler stopped"));

    concurrentx::Logger::set_sink({});
    concurrentx::Logger::set_level(concurrentx::LogLevel::Warn);
}

void test_assert_throw_mode() {
#if CONCURRENTX_ENABLE_ASSERTS
    const auto previous = concurrentx::assert_mode();
    concurrentx::set_assert_mode(concurrentx::AssertMode::Throw);

    bool threw = false;
    try {
        CX_ASSERT_MSG(false, "expected failure");
    } catch (const concurrentx::AssertionFailureException& ex) {
        threw = true;
        assert(std::string(ex.what()).find("expected failure") !=
               std::string::npos);
    }
    assert(threw);

    concurrentx::set_assert_mode(previous);
#else
    // Asserts compiled out: macro must be a no-op.
    CX_ASSERT(false);
    CX_ASSERT_MSG(false, "ignored");
#endif
}

void test_scheduler_exception_is_std_exception() {
    const concurrentx::SchedulerStoppedException ex;
    const std::exception& base = ex;
    assert(base.what() != nullptr);
}

}  // namespace

int main() {
    test_exception_hierarchy_what();
    test_scheduler_exception_is_std_exception();
    test_task_exception_propagates_via_future();
    test_nested_wait_propagates_inner_exception();
    test_empty_task_exception_propagates_via_future();
    test_logger_level_filtering();
    test_logger_off_silences_all();
    test_stopped_pool_logs_and_throws();
    test_assert_throw_mode();
    return 0;
}
