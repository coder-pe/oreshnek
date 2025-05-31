// oreshnek/src/http/HttpRequest.cpp
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/json/JsonParser.h" // For parsing JSON body
#include <sstream>

namespace Oreshnek {
namespace Http {

std::optional<std::string_view> HttpRequest::header(std::string_view name) const {
    // For case-insensitive lookup, consider storing keys in a normalized (e.g., lowercase) format
    // or using a case-insensitive string_view comparator for the map.
    // For now, it's a direct lookup.
    auto it = headers_.find(name);
    if (it != headers_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string_view> HttpRequest::query(std::string_view name) const {
    auto it = query_params_.find(name);
    if (it != query_params_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string_view> HttpRequest::param(std::string_view name) const {
    auto it = path_params_.find(name);
    if (it != path_params_.end()) {
        return it->second;
    }
    return std::nullopt;
}

Json::JsonValue HttpRequest::json() const {
    if (body_.empty()) {
        throw std::runtime_error("HTTP Request body is empty, cannot parse JSON.");
    }
    // Check Content-Type header if present
    auto content_type_header = header("Content-Type");
    if (content_type_header && content_type_header->find("application/json") == std::string_view::npos) {
        // Log a warning, but still attempt to parse if body exists
        // Or throw an error depending on desired strictness
        std::cerr << "Warning: Attempting to parse JSON from non-JSON Content-Type: "
                  << *content_type_header << std::endl;
    }
    
    return Json::JsonParser::parse(body_);
}

std::string HttpRequest::to_string() const {
    std::ostringstream oss;
    oss << "Method: " << http_method_to_string(method_) << "\n";
    oss << "Path: " << path_ << "\n";
    oss << "Version: " << version_ << "\n";
    oss << "Headers:\n";
    for (const auto& [key, value] : headers_) {
        oss << "  " << key << ": " << value << "\n";
    }
    if (!query_params_.empty()) {
        oss << "Query Params:\n";
        for (const auto& [key, value] : query_params_) {
            oss << "  " << key << ": " << value << "\n";
        }
    }
    if (!path_params_.empty()) {
        oss << "Path Params:\n";
        for (const auto& [key, value] : path_params_) {
            oss << "  " << key << ": " << value << "\n";
        }
    }
    oss << "Body Size: " << body_.length() << " bytes\n";
    if (!body_.empty()) {
        if (body_.length() < 512) { // Print small bodies
            oss << "Body: " << body_ << "\n";
        } else {
            oss << "Body: (too large to print, showing first 256 bytes) " << body_.substr(0, 256) << "...\n";
        }
    }
    return oss.str();
}

} // namespace Http
} // namespace Oreshnek
