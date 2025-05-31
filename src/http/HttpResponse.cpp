// oreshnek/src/http/HttpResponse.cpp
#include "oreshnek/http/HttpResponse.h"
#include <sstream>
#include <ctime>   // For date header
#include <filesystem> // For file size
#include <iostream> // For std::cerr

namespace Oreshnek {
namespace Http {

HttpResponse::HttpResponse() {
    // Default headers, can be overridden
    header("Server", "Oreshnek/1.0.0");
    header("Connection", "keep-alive");
}

HttpResponse& HttpResponse::status(HttpStatus status) {
    status_ = status;
    return *this;
}

HttpResponse& HttpResponse::header(const std::string& name, const std::string& value) {
    headers_[name] = value;
    return *this;
}

HttpResponse& HttpResponse::body(const std::string& content) {
    body_content_ = content; // std::string automatically
    is_file_response_ = false;
    header("Content-Length", std::to_string(std::get<std::string>(body_content_).length()));
    return *this;
}

HttpResponse& HttpResponse::body(std::string&& content) {
    body_content_ = std::move(content); // std::string automatically
    is_file_response_ = false;
    header("Content-Length", std::to_string(std::get<std::string>(body_content_).length()));
    return *this;
}

HttpResponse& HttpResponse::file(const std::string& file_path, const std::string& content_type) {
    body_content_ = FilePath(file_path); // Now stores a FilePath object
    is_file_response_ = true;
    header("Content-Type", content_type);

    // Try to get file size for Content-Length header
    try {
        std::error_code ec;
        uintmax_t file_size = std::filesystem::file_size(file_path, ec);
        if (!ec) {
            header("Content-Length", std::to_string(file_size));
        } else {
            std::cerr << "Warning: Could not get file size for " << file_path << ": " << ec.message() << std::endl;
            // Content-Length might be omitted, or set to a placeholder, or error response sent.
        }
    } catch (const std::exception& e) {
        std::cerr << "Error accessing file " << file_path << ": " << e.what() << std::endl;
    }
    return *this;
}

HttpResponse& HttpResponse::json(const Json::JsonValue& json_val) {
    return body(json_val.to_string()).header("Content-Type", "application/json");
}

HttpResponse& HttpResponse::text(const std::string& content) {
    return body(content).header("Content-Type", "text/plain");
}

HttpResponse& HttpResponse::html(const std::string& content) {
    return body(content).header("Content-Type", "text/html");
}

const std::string& HttpResponse::get_body() const {
    if (is_file_response_) {
        // This is a potentially expensive operation if the file is large
        // It reads the entire file into a string.
        // This method should primarily be used for non-file responses or small files.
        // For actual file serving, Server/Connection should use the file path directly.
        const std::string& file_path = std::get<FilePath>(body_content_).path; // Access path member
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for get_body(): " + file_path);
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        // Caching this would be an option if this getter is called frequently,
        // but for a true streaming scenario, this function should be avoided for large files.
        static std::string file_content_buffer; // Static mutable buffer, not thread-safe if called directly
        file_content_buffer = buffer.str();
        return file_content_buffer;
    }
    return std::get<std::string>(body_content_);
}

std::string HttpResponse::build_headers_string() const {
    std::ostringstream oss;

    // Status line
    oss << "HTTP/1.1 " << static_cast<int>(status_) << " " << http_status_to_string(status_) << "\r\n";

    // Headers
    // Add Date header (RFC 7231, Section 7.1.1.2)
    char buf[128];
    time_t now = time(nullptr);
    struct tm *gmt = gmtime(&now);
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", gmt);
    oss << "Date: " << buf << "\r\n";

    for (const auto& pair : headers_) {
        oss << pair.first << ": " << pair.second << "\r\n";
    }

    oss << "\r\n"; // End of headers
    return oss.str();
}

void HttpResponse::reset() {
    status_ = HttpStatus::OK;
    headers_.clear();
    body_content_ = std::string(); // Reset to empty string (assigns to the std::string alternative)
    is_file_response_ = false;
    // Re-add default headers
    header("Server", "Oreshnek/1.0.0");
    header("Connection", "keep-alive");
}

} // namespace Http
} // namespace Oreshnek
