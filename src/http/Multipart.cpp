// oreshnek/src/http/Multipart.cpp
#include "oreshnek/http/Multipart.h"

#include <cctype>

namespace Oreshnek {
namespace Http {

namespace {

// Extract the value of a quoted attribute, e.g. name="value", from a
// Content-Disposition line. Returns an empty view if not present.
std::string_view quoted_attr(std::string_view line, std::string_view attr) {
    // Look for `attr="`.
    std::string needle = std::string(attr) + "=\"";
    size_t pos = line.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    size_t end = line.find('"', pos);
    if (end == std::string_view::npos) return {};
    return line.substr(pos, end - pos);
}

bool iequals_prefix(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Parse the header block of a single part, filling name/filename/content_type.
void parse_part_headers(std::string_view headers, MultipartPart& part) {
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find("\r\n", pos);
        std::string_view line =
            (eol == std::string_view::npos) ? headers.substr(pos) : headers.substr(pos, eol - pos);
        if (iequals_prefix(line, "content-disposition:")) {
            part.name = quoted_attr(line, "name");
            part.filename = quoted_attr(line, "filename");
        } else if (iequals_prefix(line, "content-type:")) {
            part.content_type = trim(line.substr(std::string_view("content-type:").size()));
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 2;
    }
}

}  // namespace

std::string Multipart::boundary_from_content_type(std::string_view content_type) {
    size_t pos = content_type.find("boundary=");
    if (pos == std::string_view::npos) return {};
    pos += std::string_view("boundary=").size();
    std::string_view b = content_type.substr(pos);
    // A boundary may be quoted; it ends at a quote, ';' or whitespace.
    if (!b.empty() && b.front() == '"') {
        b.remove_prefix(1);
        size_t end = b.find('"');
        return std::string(end == std::string_view::npos ? b : b.substr(0, end));
    }
    size_t end = b.find_first_of("; \t\r\n");
    return std::string(end == std::string_view::npos ? b : b.substr(0, end));
}

std::vector<MultipartPart> Multipart::parse(std::string_view body, std::string_view boundary) {
    std::vector<MultipartPart> parts;
    if (boundary.empty()) return parts;

    const std::string delim = "--" + std::string(boundary);
    const std::string next_delim = "\r\n" + delim;

    // The body must start at the first boundary.
    size_t pos = body.find(delim);
    if (pos == std::string_view::npos) return parts;
    pos += delim.size();

    while (true) {
        if (pos + 2 > body.size()) break;
        // "--" after a boundary marks the end of the body.
        if (body[pos] == '-' && body[pos + 1] == '-') break;
        // Otherwise a CRLF must follow the boundary.
        if (body.substr(pos, 2) != "\r\n") break;
        pos += 2;

        size_t hdr_end = body.find("\r\n\r\n", pos);
        if (hdr_end == std::string_view::npos) break;
        std::string_view headers = body.substr(pos, hdr_end - pos);
        size_t content_start = hdr_end + 4;

        size_t next = body.find(next_delim, content_start);
        if (next == std::string_view::npos) break; // no closing delimiter
        MultipartPart part;
        part.content = body.substr(content_start, next - content_start);
        parse_part_headers(headers, part);
        if (!part.name.empty()) parts.push_back(part);

        pos = next + next_delim.size();
    }

    return parts;
}

} // namespace Http
} // namespace Oreshnek
