// oreshnek/src/utils/Logger.cpp
#include "oreshnek/utils/Logger.h"

#include <array>
#include <chrono>
#include <ctime>
#include <iostream>

namespace Oreshnek {
namespace Utils {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

namespace {
const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default:              return "OFF";
    }
}
}  // namespace

void Logger::write(LogLevel level, std::string_view message) {
    if (!enabled(level)) return;

    // Format the timestamp outside the lock to keep the critical section short.
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::array<char, 32> time_str{};
    std::strftime(time_str.data(), time_str.size(), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::lock_guard<std::mutex> lock(mutex_);
    std::clog << '[' << time_str.data() << "] [" << level_name(level) << "] " << message << '\n';
}

}  // namespace Utils
}  // namespace Oreshnek
