#include "concurrentx/debug.hpp"

#include "concurrentx/exceptions.hpp"
#include "concurrentx/log.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>

namespace concurrentx {
namespace {

std::mutex& assert_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::atomic<AssertMode>& assert_mode_storage() {
    static std::atomic<AssertMode> mode{AssertMode::Abort};
    return mode;
}

AssertHandler& assert_handler_storage() {
    // Guarded by assert_mutex when written; read under the same lock in
    // assertion_failed so the pointer cannot tear mid-call.
    static AssertHandler handler = nullptr;
    return handler;
}

[[noreturn]] void default_assert_handler(const char* file,
                                         int line,
                                         const char* expression,
                                         const char* message) {
    std::string text = "assertion failed: ";
    text += expression ? expression : "(null)";
    if (message && message[0] != '\0') {
        text += " — ";
        text += message;
    }
    text += " @ ";
    text += file ? file : "(unknown)";
    text += ':';
    text += std::to_string(line);

    CX_LOG_ERROR << text;

    if (assert_mode() == AssertMode::Throw) {
        throw AssertionFailureException{std::move(text)};
    }
    std::abort();
}

}  // namespace

void set_assert_handler(AssertHandler handler) noexcept {
    std::lock_guard<std::mutex> lock(assert_mutex());
    assert_handler_storage() = handler;
}

void set_assert_mode(AssertMode mode) noexcept {
    assert_mode_storage().store(mode, std::memory_order_release);
}

AssertMode assert_mode() noexcept {
    return assert_mode_storage().load(std::memory_order_acquire);
}

namespace detail {

[[noreturn]] void assertion_failed(const char* file,
                                   int line,
                                   const char* expression,
                                   const char* message) {
    AssertHandler handler = nullptr;
    {
        std::lock_guard<std::mutex> lock(assert_mutex());
        handler = assert_handler_storage();
    }

    if (handler) {
        handler(file, line, expression, message);
        // Custom handlers may return; fall through to default so [[noreturn]]
        // and "assert always ends the process/path" remain true.
    }
    default_assert_handler(file, line, expression, message);
}

}  // namespace detail
}  // namespace concurrentx
