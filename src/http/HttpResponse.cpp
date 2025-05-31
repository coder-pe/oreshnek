// oreshnek/src/http/HttpResponse.cpp
#include "oreshnek/http/HttpResponse.h"
#include <sstream>
#include <ctime>   // For date header

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
    body_ = content;
    header("Content-Length", std::to_string(body_.length()));
    return *this;
}

HttpResponse& HttpResponse::json(const Json::JsonValue& json_val) {
    body_ = json_val.to_string();
    header("Content-Type", "application/json");
    header("Content-Length", std::to_string(body_.length()));
    return *this;
}

HttpResponse& HttpResponse::text(const std::string& content) {
    body_ = content;
    header("Content-Type", "text/plain");
    header("Content-Length", std::to_string(body_.length()));
    return *this;
}

HttpResponse& HttpResponse::html(const std::string& content) {
    body_ = content;
    header("Content-Type", "text/html");
    header("Content-Length", std::to_string(body_.length()));
    return *this;
}

std::string HttpResponse::to_string() const {
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

    // Body
    oss << body_;

    return oss.str();
}

void HttpResponse::reset() {
    status_ = HttpStatus::OK;
    headers_.clear();
    body_.clear();
    // Re-add default headers
    header("Server", "Oreshnek/1.0.0");
    header("Connection", "keep-alive");
}

} // namespace Http
} // namespace Oreshnek
