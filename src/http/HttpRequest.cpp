// oreshnek/src/http/HttpRequest.cpp
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/json/JsonParser.h" // For parsing JSON body
#include "oreshnek/utils/Logger.h"
#include <sstream>

namespace Oreshnek {
namespace Http {

namespace {
// Repoint a single view from old_base to new_base, preserving its offset/length.
inline std::string_view shift_view(std::string_view v, const char* old_base, const char* new_base) {
    if (v.data() == nullptr) return v;
    return std::string_view(new_base + (v.data() - old_base), v.size());
}
// Rebuild a <view,view> map with every key/value repointed.
inline void shift_map(std::unordered_map<std::string_view, std::string_view>& m,
                      const char* old_base, const char* new_base) {
    std::unordered_map<std::string_view, std::string_view> rebuilt;
    rebuilt.reserve(m.size());
    for (const auto& [k, v] : m) {
        rebuilt.emplace(shift_view(k, old_base, new_base), shift_view(v, old_base, new_base));
    }
    m = std::move(rebuilt);
}
}  // namespace

void HttpRequest::rebase_views(const char* old_base, const char* new_base) {
    if (old_base == new_base) return;
    path_ = shift_view(path_, old_base, new_base);
    version_ = shift_view(version_, old_base, new_base);
    body_ = shift_view(body_, old_base, new_base);
    shift_map(headers_, old_base, new_base);
    shift_map(query_params_, old_base, new_base);
    // path_params_ keys point at Router-owned storage, not the request buffer,
    // and are only populated after make_owned(); leave them untouched here.
}

void HttpRequest::make_owned(const char* base, size_t len) {
    owned_storage_.assign(base, len);
    rebase_views(base, owned_storage_.data());
}

void HttpRequest::copy_from(const HttpRequest& other) {
    method_ = other.method_;
    path_ = other.path_;
    version_ = other.version_;
    headers_ = other.headers_;
    query_params_ = other.query_params_;
    path_params_ = other.path_params_;
    body_ = other.body_;
    owned_storage_ = other.owned_storage_;
    // If the source owned its bytes, our views still point into the source's
    // buffer; repoint them at our own copy so we don't dangle when it dies.
    if (!owned_storage_.empty()) {
        rebase_views(other.owned_storage_.data(), owned_storage_.data());
    }
}

void HttpRequest::move_from(HttpRequest&& other) noexcept {
    const char* old_base = other.owned_storage_.empty() ? nullptr : other.owned_storage_.data();
    method_ = other.method_;
    path_ = other.path_;
    version_ = other.version_;
    headers_ = std::move(other.headers_);
    query_params_ = std::move(other.query_params_);
    path_params_ = std::move(other.path_params_);
    body_ = other.body_;
    owned_storage_ = std::move(other.owned_storage_);
    // std::string move may relocate (SSO); repoint views if the buffer moved.
    if (old_base != nullptr) {
        rebase_views(old_base, owned_storage_.data());
    }
}

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
        // Log a warning, but still attempt to parse if a body exists.
        ORE_LOG(WARN) << "Attempting to parse JSON from non-JSON Content-Type: " << *content_type_header;
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
