// oreshnek/src/utils/Logger.cpp
#include "oreshnek/utils/Logger.h"

#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

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

LogLevel level_from_string(std::string_view name, LogLevel fallback) {
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "trace") return LogLevel::TRACE;
    if (lower == "debug") return LogLevel::DEBUG;
    if (lower == "info")  return LogLevel::INFO;
    if (lower == "warn" || lower == "warning") return LogLevel::WARN;
    if (lower == "error") return LogLevel::ERROR;
    if (lower == "off" || lower == "none") return LogLevel::OFF;
    return fallback;
}

bool Logger::set_file(const std::string& path, std::size_t max_bytes, int max_files) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path.empty()) {
        if (file_.is_open()) file_.close();
        file_path_.clear();
        return true;
    }
    // Open in append mode so restarts do not clobber existing logs.
    file_.open(path, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        return false;
    }
    file_path_ = path;
    max_bytes_ = max_bytes;
    max_files_ = max_files;
    std::error_code ec;
    auto existing = std::filesystem::file_size(path, ec);
    current_bytes_ = ec ? 0 : static_cast<std::size_t>(existing);
    return true;
}

void Logger::rotate_if_needed(std::size_t incoming_bytes) {
    if (!file_.is_open() || max_bytes_ == 0) return;
    if (current_bytes_ + incoming_bytes <= max_bytes_) return;

    file_.close();
    std::error_code ec;
    namespace fs = std::filesystem;

    // Discard the oldest, then shift each "<path>.i" to "<path>.(i+1)".
    if (max_files_ > 0) {
        fs::remove(file_path_ + "." + std::to_string(max_files_), ec);
        for (int i = max_files_ - 1; i >= 1; --i) {
            fs::rename(file_path_ + "." + std::to_string(i),
                       file_path_ + "." + std::to_string(i + 1), ec);
        }
        fs::rename(file_path_, file_path_ + ".1", ec);
    }

    file_.open(file_path_, std::ios::out | std::ios::trunc);
    current_bytes_ = 0;
}

void Logger::write(LogLevel level, std::string_view message) {
    if (!enabled(level)) return;

    // Format the timestamp/thread id outside the lock to keep the section short.
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

    std::ostringstream line;
    line << '[' << time_str.data() << "] [" << level_name(level) << "] [tid "
         << std::this_thread::get_id() << "] " << message << '\n';
    const std::string text = line.str();

    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        rotate_if_needed(text.size());
        file_ << text;
        file_.flush();
        current_bytes_ += text.size();
    } else {
        std::clog << text;
    }
}

}  // namespace Utils
}  // namespace Oreshnek
