// oreshnek/include/oreshnek/utils/Logger.h
#ifndef ORESHNEK_UTILS_LOGGER_H
#define ORESHNEK_UTILS_LOGGER_H

#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace Oreshnek {
namespace Utils {

enum class LogLevel : int { TRACE = 0, DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, OFF = 5 };

// Minimal thread-safe logger. All output goes through a single mutex-guarded
// sink so concurrent log calls from the event loop and worker threads do not
// race on the underlying stream. This is a stop-gap until a full structured
// logging backend (e.g. spdlog) is integrated; the call sites use it already.
class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level) { level_.store(level, std::memory_order_relaxed); }
    LogLevel level() const { return level_.load(std::memory_order_relaxed); }
    bool enabled(LogLevel level) const { return level >= level_.load(std::memory_order_relaxed); }

    // Writes one already-formatted line atomically (adds level prefix + newline).
    void write(LogLevel level, std::string_view message);

private:
    Logger() = default;
    std::mutex mutex_;
    std::atomic<LogLevel> level_{LogLevel::INFO};
};

// Stream-style helper: buffers the message and flushes it under the logger's
// lock on destruction, so a whole line is emitted atomically.
class LogMessage {
public:
    explicit LogMessage(LogLevel level) : level_(level), active_(Logger::instance().enabled(level)) {}
    ~LogMessage() {
        if (active_) Logger::instance().write(level_, stream_.str());
    }

    LogMessage(const LogMessage&) = delete;
    LogMessage& operator=(const LogMessage&) = delete;

    template <typename T>
    LogMessage& operator<<(const T& value) {
        if (active_) stream_ << value;
        return *this;
    }

private:
    LogLevel level_;
    bool active_;
    std::ostringstream stream_;
};

}  // namespace Utils
}  // namespace Oreshnek

// Usage: ORE_LOG(INFO) << "listening on " << port;
#define ORE_LOG(level) ::Oreshnek::Utils::LogMessage(::Oreshnek::Utils::LogLevel::level)

#endif  // ORESHNEK_UTILS_LOGGER_H
