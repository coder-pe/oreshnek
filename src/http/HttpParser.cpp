// oreshnek/src/http/HttpParser.cpp
#include "oreshnek/http/HttpParser.h"
#include <algorithm> // For std::min
#include <iostream>  // For debugging
#include <string_view>

namespace Oreshnek {
namespace Http {

HttpParser::HttpParser() : state_(ParsingState::REQUEST_LINE), body_expected_length_(0), is_chunked_(false) {}

void HttpParser::reset() {
    state_ = ParsingState::REQUEST_LINE;
    body_expected_length_ = 0;
    is_chunked_ = false;
    error_message_.clear();
}

bool HttpParser::parse_request(std::string_view raw_buffer, size_t& bytes_processed, HttpRequest& request) {
    bytes_processed = 0;
    std::string_view current_data = raw_buffer;

    while (state_ != ParsingState::COMPLETE && state_ != ParsingState::ERROR && !current_data.empty()) {
        size_t consumed_bytes_in_step = 0;
        bool step_complete = false;

        switch (state_) {
            case ParsingState::REQUEST_LINE:
                step_complete = parse_request_line(current_data, request);
                break;
            case ParsingState::HEADERS:
                step_complete = parse_headers(current_data, request);
                break;
            case ParsingState::BODY:
                step_complete = parse_body(current_data, request);
                break;
            default:
                // Should not happen
                state_ = ParsingState::ERROR;
                error_message_ = "Invalid parser state.";
                return false;
        }

        if (state_ == ParsingState::ERROR) {
            return false;
        }

        // Calculate how much data was consumed in this step
        // (raw_buffer.length() - current_data.length()) represents consumed data from original buffer
        consumed_bytes_in_step = raw_buffer.length() - current_data.length() - bytes_processed;
        bytes_processed += consumed_bytes_in_step;

        if (!step_complete && consumed_bytes_in_step == 0) {
            // No progress made, need more data
            return false;
        }
    }

    return state_ == ParsingState::COMPLETE;
}

bool HttpParser::parse_request_line(std::string_view& data, HttpRequest& request) {
    size_t eol_pos = data.find("\r\n");
    if (eol_pos == std::string_view::npos) {
        // Need more data for the request line
        return false;
    }

    std::string_view line = data.substr(0, eol_pos);
    data.remove_prefix(eol_pos + 2); // Consume line and CRLF

    size_t first_space = line.find(' ');
    size_t second_space = line.find(' ', first_space + 1);

    if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
        state_ = ParsingState::ERROR;
        error_message_ = "Invalid request line format: " + std::string(line);
        return false;
    }

    std::string_view method_str = line.substr(0, first_space);
    std::string_view path_and_query_str = line.substr(first_space + 1, second_space - (first_space + 1));
    request.version_ = line.substr(second_space + 1);

    // Determine HttpMethod
    if (method_str == "GET") request.method_ = HttpMethod::GET;
    else if (method_str == "POST") request.method_ = HttpMethod::POST;
    else if (method_str == "PUT") request.method_ = HttpMethod::PUT;
    else if (method_str == "DELETE") request.method_ = HttpMethod::DELETE;
    else if (method_str == "PATCH") request.method_ = HttpMethod::PATCH;
    else if (method_str == "HEAD") request.method_ = HttpMethod::HEAD;
    else if (method_str == "OPTIONS") request.method_ = HttpMethod::OPTIONS;
    else {
        request.method_ = HttpMethod::UNKNOWN;
        state_ = ParsingState::ERROR;
        error_message_ = "Unsupported HTTP method: " + std::string(method_str);
        return false;
    }

    // Parse path and query parameters
    parse_query_parameters(path_and_query_str, request);

    state_ = ParsingState::HEADERS;
    return true;
}

bool HttpParser::parse_query_parameters(std::string_view& path_and_query, HttpRequest& request) {
    size_t query_start = path_and_query.find('?');
    if (query_start == std::string_view::npos) {
        request.path_ = path_and_query; // No query string
        return true;
    }

    request.path_ = path_and_query.substr(0, query_start);
    std::string_view query_str = path_and_query.substr(query_start + 1);

    size_t start = 0;
    while (start < query_str.length()) {
        size_t eq_pos = query_str.find('=', start);
        if (eq_pos == std::string_view::npos) {
            // Malformed query parameter, or a key without value
            request.query_params_[query_str.substr(start)] = "";
            break;
        }

        std::string_view key = query_str.substr(start, eq_pos - start);
        size_t amp_pos = query_str.find('&', eq_pos + 1);
        std::string_view value = query_str.substr(eq_pos + 1, amp_pos - (eq_pos + 1));

        request.query_params_[key] = value;

        if (amp_pos == std::string_view::npos) {
            break; // No more parameters
        }
        start = amp_pos + 1;
    }
    return true;
}


bool HttpParser::parse_headers(std::string_view& data, HttpRequest& request) {
    while (true) {
        size_t eol_pos = data.find("\r\n");
        if (eol_pos == std::string_view::npos) {
            // Need more data for headers
            return false;
        }

        std::string_view line = data.substr(0, eol_pos);
        data.remove_prefix(eol_pos + 2); // Consume line and CRLF

        if (line.empty()) {
            // Empty line indicates end of headers
            // Check for Content-Length or Transfer-Encoding
            auto content_length_header = request.header("Content-Length");
            if (content_length_header) {
                try {
                    body_expected_length_ = std::stoul(std::string(*content_length_header));
                } catch (const std::exception& e) {
                    state_ = ParsingState::ERROR;
                    error_message_ = "Invalid Content-Length header: " + std::string(*content_length_header);
                    return false;
                }
            } else {
                auto transfer_encoding_header = request.header("Transfer-Encoding");
                if (transfer_encoding_header && *transfer_encoding_header == "chunked") {
                    is_chunked_ = true;
                    // Chunked encoding requires special handling for body parsing
                    state_ = ParsingState::ERROR; // Not implemented yet
                    error_message_ = "Chunked Transfer-Encoding not implemented.";
                    return false;
                }
            }

            if (body_expected_length_ > 0 || is_chunked_) {
                state_ = ParsingState::BODY;
            } else {
                state_ = ParsingState::COMPLETE;
            }
            return true;
        }

        // Parse header: Key: Value
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string_view::npos) {
            state_ = ParsingState::ERROR;
            error_message_ = "Invalid header format: " + std::string(line);
            return false;
        }

        std::string_view key = line.substr(0, colon_pos);
        std::string_view value = line.substr(colon_pos + 1);

        // Trim whitespace from value (leading only, trailing typically not an issue after substr)
        size_t value_start = 0;
        while (value_start < value.length() && std::isspace(static_cast<unsigned char>(value[value_start]))) {
            value_start++;
        }
        value.remove_prefix(value_start);

        // Normalize header key (e.g., to lowercase for case-insensitive lookup)
        // For string_view, direct hashing is better for performance if case-insensitivity is handled at lookup.
        // For now, we store them as is from the buffer. If header() method needs case-insensitivity,
        // it should normalize the input `name` or iterate.
        request.headers_[key] = value;
    }
}

bool HttpParser::parse_body(std::string_view& data, HttpRequest& request) {
    if (is_chunked_) {
        // This part needs a dedicated chunked decoder implementation.
        // For now, if we encounter chunked, we marked it as error in parse_headers.
        state_ = ParsingState::ERROR;
        error_message_ = "Chunked Transfer-Encoding is not implemented.";
        return false;
    }

    if (body_expected_length_ > 0) {
        if (data.length() < body_expected_length_) {
            // Need more data for the body
            return false;
        }
        request.body_ = data.substr(0, body_expected_length_);
        data.remove_prefix(body_expected_length_);
        state_ = ParsingState::COMPLETE;
        return true;
    } else {
        // No Content-Length, no chunked. Body is empty.
        state_ = ParsingState::COMPLETE;
        return true;
    }
}

} // namespace Http
} // namespace Oreshnek
