// oreshnek/include/oreshnek/http/HttpRequest.h
#ifndef ORESHNEK_HTTP_HTTPREQUEST_H
#define ORESHNEK_HTTP_HTTPREQUEST_H

#include "oreshnek/http/HttpEnums.h"
#include "oreshnek/json/JsonValue.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional> // For C++17 optional return types

namespace Oreshnek {
namespace Http {

class HttpRequest {
public:
    HttpMethod method_ = HttpMethod::UNKNOWN;
    std::string_view path_;
    std::string_view version_; // E.g., "HTTP/1.1"

    // Headers stored as string_views pointing into the raw buffer
    std::unordered_map<std::string_view, std::string_view> headers_;

    // Query parameters (e.g., ?key=value)
    std::unordered_map<std::string_view, std::string_view> query_params_;

    // Path parameters (e.g., /users/:id) - set by the Router
    std::unordered_map<std::string_view, std::string_view> path_params_;

    // Raw body (points into the raw buffer)
    std::string_view body_;

    // Constructor is private; requests are built by HttpParser
private:
    HttpRequest() = default; // Only HttpParser should create these
    friend class HttpParser; // HttpParser can access private members

public:
    // Public getters for access
    HttpMethod method() const { return method_; }
    std::string_view path() const { return path_; }
    std::string_view version() const { return version_; }

    // Get header by name (case-insensitive recommended, but for string_view we'll stick to exact match or convert key)
    // For performance, exact match is better. If case-insensitivity is needed, normalize keys during parsing.
    std::optional<std::string_view> header(std::string_view name) const;

    // Get query parameter
    std::optional<std::string_view> query(std::string_view name) const;

    // Get path parameter
    std::optional<std::string_view> param(std::string_view name) const;

    // Get the raw body as a string_view
    std::string_view body() const { return body_; }

    // Parse JSON body. Will throw if body is not valid JSON.
    Json::JsonValue json() const;

    // For debugging/logging
    std::string to_string() const;
};

} // namespace Http
} // namespace Oreshnek

#endif // ORESHNEK_HTTP_HTTPREQUEST_H
