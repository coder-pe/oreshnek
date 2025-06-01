// oreshnek/include/oreshnek/http/HttpResponse.h
#ifndef ORESHNEK_HTTP_HTTPRESPONSE_H
#define ORESHNEK_HTTP_HTTPRESPONSE_H

#include "oreshnek/http/HttpEnums.h"
#include "oreshnek/json/JsonValue.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <variant> // For std::variant (C++17) to hold different body types
#include <fstream> // For std::ifstream

namespace Oreshnek {
namespace Http {

// Define a distinct type for file paths to avoid std::variant ambiguity
struct FilePath {
    std::string path;
    // Constructor to allow implicit conversion from std::string
    FilePath(const std::string& p) : path(p) {}
    FilePath(std::string&& p) : path(std::move(p)) {}
};

class HttpResponse {
private:
    HttpStatus status_ = HttpStatus::OK;
    std::unordered_map<std::string, std::string> headers_;
    // Use std::variant to hold either a string body or a file path for streaming
    std::variant<std::string, FilePath> body_content_; // Stores either direct content or file path
    bool is_file_response_ = false; // Flag to indicate if content_ is a file path

public:
    HttpResponse(); // Default constructor

    // Setters
    HttpResponse& status(HttpStatus status);
    HttpResponse& header(const std::string& name, const std::string& value);

    // Set the body directly with a string
    HttpResponse& body(const std::string& content);
    HttpResponse& body(std::string&& content); // Move overload

    // Set the body to be a file to be streamed
    HttpResponse& file(const std::string& file_path, const std::string& content_type = "application/octet-stream");

    // Convenience methods for common response types
    HttpResponse& json(const Json::JsonValue& json_val);
    HttpResponse& text(const std::string& content);
    HttpResponse& html(const std::string& content);

    // Getters
    HttpStatus get_status() const { return status_; }
    const std::unordered_map<std::string, std::string>& get_headers() const { return headers_; }
    // Get the body as a string. This will *read the file into string* if it's a file response.
    // Use get_body_variant() for direct access to the underlying storage without reading files.
    // REMOVED: const std::string& get_body() const; // This method is problematic for large files and streaming

    // New getter to know if it's a file response
    bool is_file() const { return is_file_response_; }
    // Get the variant directly for more efficient handling in Connection/Server
    const std::variant<std::string, FilePath>& get_body_variant() const { return body_content_; }


    // Build the full HTTP response string (excluding large file body)
    std::string build_headers_string() const;

    // Reset the response for reuse (e.g., in a pool of responses)
    void reset();
};

} // namespace Http
} // namespace Oreshnek

#endif // ORESHNEK_HTTP_HTTPRESPONSE_H
