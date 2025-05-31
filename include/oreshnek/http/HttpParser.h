// oreshnek/include/oreshnek/http/HttpParser.h
#ifndef ORESHNEK_HTTP_HTTPPARSER_H
#define ORESHNEK_HTTP_HTTPPARSER_H

#include "oreshnek/http/HttpRequest.h"
#include <string_view>
#include <memory> // For unique_ptr

namespace Oreshnek {
namespace Http {

// State for parsing HTTP requests incrementally
enum class ParsingState {
    REQUEST_LINE,
    HEADERS,
    BODY,
    COMPLETE,
    ERROR
};

class HttpParser {
public:
    HttpParser();
    ~HttpParser() = default;

    // Resets the parser state for a new request
    void reset();

    // Parses a chunk of incoming data. Returns true if a full request is parsed.
    // raw_buffer is the total buffer, data_chunk is the new incoming chunk.
    // bytes_processed indicates how many bytes from raw_buffer were consumed.
    bool parse_request(std::string_view raw_buffer, size_t& bytes_processed, HttpRequest& request);

    ParsingState get_state() const { return state_; }
    const std::string& get_error_message() const { return error_message_; }

private:
    ParsingState state_;
    size_t body_expected_length_ = 0; // From Content-Length header
    bool is_chunked_ = false; // From Transfer-Encoding header
    // No direct buffer management here; HttpParser works on views of an external buffer.

    // Helper functions for parsing
    bool parse_request_line(std::string_view& data, HttpRequest& request);
    bool parse_headers(std::string_view& data, HttpRequest& request);
    bool parse_body(std::string_view& data, HttpRequest& request);
    bool parse_query_parameters(std::string_view& path_and_query, HttpRequest& request);

    std::string error_message_;
};

} // namespace Http
} // namespace Oreshnek

#endif // ORESHNEK_HTTP_HTTPPARSER_H
