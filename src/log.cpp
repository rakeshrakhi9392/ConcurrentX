#include "concurrentx/log.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace concurrentx {
namespace {

std::mutex& logger_mutex() {
    static std::mutex mutex;
    return mutex;
}

LogLevel& level_storage() {
    // Written/read under logger_mutex.
    static LogLevel level = LogLevel::Warn;
    return level;
}

Logger::Sink& sink_storage() {
    static Logger::Sink sink;
    return sink;
}

void default_sink(LogLevel level, const std::string& message) {
    // Timestamps are best-effort; clock failures must never throw out of log.
    std::ostringstream line;
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    line << std::put_time(&tm_buf, "%H:%M:%S") << " ["
         << Logger::level_name(level) << "] " << message << '\n';
    std::cerr << line.str();
}

}  // namespace

void Logger::set_level(LogLevel level) noexcept {
    std::lock_guard<std::mutex> lock(logger_mutex());
    level_storage() = level;
}

LogLevel Logger::level() noexcept {
    std::lock_guard<std::mutex> lock(logger_mutex());
    return level_storage();
}

bool Logger::enabled(LogLevel level) noexcept {
    if (level == LogLevel::Off) {
        return false;
    }
    std::lock_guard<std::mutex> lock(logger_mutex());
    return static_cast<int>(level) <= static_cast<int>(level_storage()) &&
           level_storage() != LogLevel::Off;
}

void Logger::set_sink(Sink sink) {
    std::lock_guard<std::mutex> lock(logger_mutex());
    sink_storage() = std::move(sink);
}

void Logger::log(LogLevel level, const std::string& message) {
    Sink sink_copy;
    {
        std::lock_guard<std::mutex> lock(logger_mutex());
        if (level_storage() == LogLevel::Off ||
            static_cast<int>(level) > static_cast<int>(level_storage())) {
            return;
        }
        sink_copy = sink_storage();
    }

    try {
        if (sink_copy) {
            sink_copy(level, message);
        } else {
            // Default sink also needs mutual exclusion against concurrent
            // cerr writes from multiple workers.
            std::lock_guard<std::mutex> lock(logger_mutex());
            default_sink(level, message);
        }
    } catch (...) {
        // Logging must never escape into worker / scheduler control flow.
    }
}

const char* Logger::level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Off:
            return "OFF";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Trace:
            return "TRACE";
    }
    return "UNKNOWN";
}

}  // namespace concurrentx
