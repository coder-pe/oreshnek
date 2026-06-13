// oreshnek/include/oreshnek/http/Multipart.h
#ifndef ORESHNEK_HTTP_MULTIPART_H
#define ORESHNEK_HTTP_MULTIPART_H

#include <string>
#include <string_view>
#include <vector>

namespace Oreshnek {
namespace Http {

// One part of a multipart/form-data body. All views point into the request body
// that was passed to Multipart::parse(), so they are valid for as long as that
// body lives (the body is owned by the request for the duration of the handler).
struct MultipartPart {
    std::string_view name;         // form field name (Content-Disposition name=)
    std::string_view filename;     // filename= (empty when the part is not a file)
    std::string_view content_type; // part Content-Type (may be empty)
    std::string_view content;      // raw bytes of the part body

    bool is_file() const { return !filename.empty(); }
};

class Multipart {
public:
    // Parse a multipart/form-data body. `boundary` is the boundary token from the
    // Content-Type header (without the leading "--"). Returns the parts in order;
    // an empty vector if the body is malformed or the boundary is empty.
    static std::vector<MultipartPart> parse(std::string_view body, std::string_view boundary);

    // Extract the boundary token from a Content-Type header value
    // (e.g. "multipart/form-data; boundary=----abc"). Returns "" if absent.
    static std::string boundary_from_content_type(std::string_view content_type);
};

} // namespace Http
} // namespace Oreshnek

#endif // ORESHNEK_HTTP_MULTIPART_H
