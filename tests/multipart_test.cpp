// tests/multipart_test.cpp
//
// Unit tests for the multipart/form-data parser.

#include "oreshnek/http/Multipart.h"

#include <iostream>
#include <string>

using Oreshnek::Http::Multipart;
using Oreshnek::Http::MultipartPart;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}

const MultipartPart* find_part(const std::vector<MultipartPart>& parts, std::string_view name) {
    for (const auto& p : parts) {
        if (p.name == name) return &p;
    }
    return nullptr;
}
}  // namespace

int main() {
    const std::string boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";

    // boundary extraction
    check(Multipart::boundary_from_content_type(
              "multipart/form-data; boundary=" + boundary) == boundary,
          "boundary extracted");
    check(Multipart::boundary_from_content_type("multipart/form-data; boundary=\"abc\"") == "abc",
          "quoted boundary extracted");
    check(Multipart::boundary_from_content_type("text/plain").empty(),
          "no boundary -> empty");

    // A body with two text fields and one file part. The file content itself
    // contains a line that looks like text and CRLFs, to ensure binary safety.
    std::string file_data = "binary\r\ndata\r\nwith--dashes and \"quotes\"";
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"title\"\r\n\r\n";
    body += "My Video\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"category\"\r\n\r\n";
    body += "education\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"video\"; filename=\"clip.mp4\"\r\n";
    body += "Content-Type: video/mp4\r\n\r\n";
    body += file_data + "\r\n";
    body += "--" + boundary + "--\r\n";

    auto parts = Multipart::parse(body, boundary);
    check(parts.size() == 3, "three parts parsed, got " + std::to_string(parts.size()));

    const MultipartPart* title = find_part(parts, "title");
    check(title && title->content == "My Video", "title field content");
    check(title && !title->is_file(), "title is not a file");

    const MultipartPart* category = find_part(parts, "category");
    check(category && category->content == "education", "category field content");

    const MultipartPart* video = find_part(parts, "video");
    check(video && video->is_file(), "video is a file part");
    check(video && video->filename == "clip.mp4", "video filename");
    check(video && video->content_type == "video/mp4", "video content-type");
    check(video && video->content == file_data,
          "video binary content preserved exactly (" +
              std::to_string(video ? video->content.size() : 0) + " bytes)");

    // Malformed / empty inputs.
    check(Multipart::parse(body, "").empty(), "empty boundary -> no parts");
    check(Multipart::parse("garbage without boundary", boundary).empty(),
          "missing boundary in body -> no parts");

    if (g_failures == 0) {
        std::cout << "[OK] all multipart tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
