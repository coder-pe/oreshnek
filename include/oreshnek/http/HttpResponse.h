// oreshnek/include/oreshnek/http/HttpResponse.h
#ifndef ORESHNEK_HTTP_HTTPRESPONSE_H
#define ORESHNEK_HTTP_HTTPRESPONSE_H

#include "oreshnek/http/HttpEnums.h"
#include "oreshnek/json/JsonValue.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace Oreshnek {
namespace Http {

class HttpResponse {
private:
    HttpStatus status_ = HttpStatus::OK;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
    bool headers_sent_ = false; // Flag to ensure headers are only built once

public:
    HttpResponse(); // Default constructor

    // Setters
    HttpResponse& status(HttpStatus status);
    HttpResponse& header(const std::string& name, const std::string& value);
    HttpResponse& body(const std::string& content);

    // Convenience methods for common response types
    HttpResponse& json(const Json::JsonValue& json_val);
    HttpResponse& text(const std::string& content);
    HttpResponse& html(const std::string& content);

    // Getters
    HttpStatus get_status() const { return status_; }
    const std::unordered_map<std::string, std::string>& get_headers() const { return headers_; }
    const std::string& get_body() const { return body_; }

    // Build the full HTTP response string
    std::string to_string() const;

    // Reset the response for reuse (e.g., in a pool of responses)
    void reset();
};

} // namespace Http
} // namespace Oreshnek

#endif // ORESHNEK_HTTP_HTTPRESPONSE_H
