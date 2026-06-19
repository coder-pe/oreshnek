// oreshnek/include/oreshnek/utils/Logger.h
#ifndef ORESHNEK_UTILS_LOGGER_H
#define ORESHNEK_UTILS_LOGGER_H

#include <atomic>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace Oreshnek {
namespace Utils {

enum class LogLevel : int { TRACE = 0, DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, OFF = 5 };

// Parse a level name ("info", "WARN", ...) case-insensitively. Unknown names
// fall back to the provided default.
LogLevel level_from_string(std::string_view name, LogLevel fallback = LogLevel::INFO);

// Thread-safe logger. All output goes through a single mutex-guarded sink so
// concurrent log calls from the event loop and worker threads do not race on the
// underlying stream. Output is structured: timestamp, level, thread id, message.
// Optionally writes to a file with size-based rotation instead of std::clog.
class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level) { level_.store(level, std::memory_order_relaxed); }
    LogLevel level() const { return level_.load(std::memory_order_relaxed); }
    bool enabled(LogLevel level) const { return level >= level_.load(std::memory_order_relaxed); }

    // Direct output to a file with rotation: when the active file would exceed
    // max_bytes, it is rotated to "<path>.1" (older files shift to .2, .3, ...,
    // up to max_files; the oldest is discarded) and a fresh file is opened.
    // Passing an empty path (or never calling this) keeps the std::clog sink.
    // Returns false if the file could not be opened.
    bool set_file(const std::string& path, std::size_t max_bytes = 10 * 1024 * 1024,
                  int max_files = 5);

    // Writes one already-formatted line atomically (adds timestamp/level/thread
    // prefix + newline).
    void write(LogLevel level, std::string_view message);

private:
    Logger() = default;

    // Called with mutex_ held. Rotates the file if the next line would overflow.
    void rotate_if_needed(std::size_t incoming_bytes);

    std::mutex mutex_;
    std::atomic<LogLevel> level_{LogLevel::INFO};

    // File sink state (guarded by mutex_). When file_ is not open, output goes
    // to std::clog.
    std::ofstream file_;
    std::string file_path_;
    std::size_t max_bytes_ = 0;
    int max_files_ = 0;
    std::size_t current_bytes_ = 0;
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
