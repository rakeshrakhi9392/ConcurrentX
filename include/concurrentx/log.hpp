#pragma once

/**
 * Lightweight, thread-safe logging for ConcurrentX.
 *
 * Default sink writes to stderr under a process-wide mutex. Levels and sinks
 * are adjustable at runtime so benchmarks can silence output and tests can
 * capture messages.
 *
 * Prefer the CX_LOG_* macros; they evaluate arguments only when the level is
 * enabled.
 */

#include <functional>
#include <sstream>
#include <string>
#include <utility>

namespace concurrentx {

enum class LogLevel {
    Off = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
    Trace = 5,
};

/**
 * Process-wide logger. All methods are thread-safe.
 *
 * Ownership: the sink callback is stored by value (std::function). Callers
 * that capture shared state should use shared_ptr / weak_ptr inside the
 * callback so the sink cannot outlive that state unsafely.
 */
class Logger {
public:
    using Sink = std::function<void(LogLevel level, const std::string& message)>;

    static void set_level(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel level() noexcept;
    [[nodiscard]] static bool enabled(LogLevel level) noexcept;

    /** Replace the sink. Pass an empty Sink to restore the default stderr sink. */
    static void set_sink(Sink sink);

    /** Emit a fully-formatted message if level is enabled. */
    static void log(LogLevel level, const std::string& message);

    static const char* level_name(LogLevel level) noexcept;
};

namespace detail {

/**
 * Temporary stream object used by CX_LOG_* macros.
 * Flushes to Logger in the destructor (end of the full expression).
 */
class LogStream {
public:
    explicit LogStream(LogLevel level) : level_(level), active_(Logger::enabled(level)) {}

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    ~LogStream() {
        if (active_) {
            Logger::log(level_, stream_.str());
        }
    }

    template <typename T>
    LogStream& operator<<(const T& value) {
        if (active_) {
            stream_ << value;
        }
        return *this;
    }

private:
    LogLevel level_;
    bool active_;
    std::ostringstream stream_;
};

}  // namespace detail

}  // namespace concurrentx

#define CX_LOG_ERROR ::concurrentx::detail::LogStream(::concurrentx::LogLevel::Error)
#define CX_LOG_WARN ::concurrentx::detail::LogStream(::concurrentx::LogLevel::Warn)
#define CX_LOG_INFO ::concurrentx::detail::LogStream(::concurrentx::LogLevel::Info)
#define CX_LOG_DEBUG ::concurrentx::detail::LogStream(::concurrentx::LogLevel::Debug)
#define CX_LOG_TRACE ::concurrentx::detail::LogStream(::concurrentx::LogLevel::Trace)
