// oreshnek/include/oreshnek/http/HttpRequest.h
#ifndef ORESHNEK_HTTP_HTTPREQUEST_H
#define ORESHNEK_HTTP_HTTPREQUEST_H

#include "oreshnek/http/HttpEnums.h"
#include <nlohmann/json.hpp>
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

    HttpRequest() = default; // Only HttpParser should create these
    friend class HttpParser; // HttpParser can access private members

    // Copy/move must keep the string_views consistent with owned_storage_.
    HttpRequest(const HttpRequest& other) { copy_from(other); }
    HttpRequest(HttpRequest&& other) noexcept { move_from(std::move(other)); }
    HttpRequest& operator=(const HttpRequest& other) {
        if (this != &other) copy_from(other);
        return *this;
    }
    HttpRequest& operator=(HttpRequest&& other) noexcept {
        if (this != &other) move_from(std::move(other));
        return *this;
    }

    // Detach this request from the external socket buffer by copying the
    // request bytes [base, base + len) into owned storage and re-pointing every
    // string_view at that copy. After this call the request can safely outlive
    // the connection's read buffer and be handed to another thread.
    void make_owned(const char* base, size_t len);

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
    nlohmann::json json() const;

    // For debugging/logging
    std::string to_string() const;

private:
    // When non-empty, all string_views above point into this buffer instead of
    // an external socket buffer. Empty in the zero-copy hot path.
    std::string owned_storage_;

    // Shift every view by (new_base - old_base), rebuilding the maps.
    void rebase_views(const char* old_base, const char* new_base);
    void copy_from(const HttpRequest& other);
    void move_from(HttpRequest&& other) noexcept;
};

} // namespace Http
} // namespace Oreshnek

#endif // ORESHNEK_HTTP_HTTPREQUEST_H
