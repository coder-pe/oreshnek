// oreshnek/include/oreshnek/net/Connection.h
#ifndef ORESHNEK_NET_CONNECTION_H
#define ORESHNEK_NET_CONNECTION_H

#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h" // Include this to get FilePath definition
#include "oreshnek/http/HttpParser.h"
#include <string>
#include <vector>
#include <chrono>
#include <sys/types.h> // For off_t

namespace Oreshnek {
namespace Net {

class Connection {
public:
    // Changed buffer size from 8KB to 1MB to handle larger requests/uploads
    static constexpr size_t READ_BUFFER_SIZE = 1024 * 1024; // 1MB
    // Maximum bytes handed to a single sendfile() call.
    static constexpr size_t FILE_SEND_CHUNK = 256 * 1024;

    // Sentinel returned by read_data() when the socket has no data right now
    // (EAGAIN/EWOULDBLOCK) but is still open. Distinct from 0 (peer closed).
    static constexpr ssize_t kReadWouldBlock = -2;

    int socket_fd_;
    std::vector<char> read_buffer_; // Buffer for incoming data
    size_t read_buffer_fill_ = 0; // Current fill level of the read buffer

    // --- Outgoing response state (touched only by the event-loop thread) ---
    std::string raw_headers_to_send_; // Serialized status line + headers
    bool headers_sent_ = false;       // Headers are sent before any body

    // In-memory string body (JSON/HTML/text). The offset avoids the O(n^2)
    // cost of erasing from the front on each partial write.
    std::string write_body_;
    size_t write_body_offset_ = 0;

    // File body served with zero-copy sendfile(). file_fd_ >= 0 when active.
    int file_fd_ = -1;
    off_t file_offset_ = 0;    // Current offset within the file
    off_t file_remaining_ = 0; // Bytes still to send

    bool head_only_ = false;   // HEAD request: emit headers, suppress body

    Http::HttpParser http_parser_;
    Http::HttpRequest current_request_; // Holds the parsed request data
    
    std::chrono::steady_clock::time_point last_activity_;
    bool keep_alive_ = true;

    // True while a request from this connection is being handled by a worker or
    // its response is still being written. Guards against dispatching more than
    // one request at a time (preserves HTTP/1.1 response ordering) and against
    // closing a connection that has work in flight. Touched only by the event loop.
    bool processing_ = false;

    Connection(int fd);
    ~Connection();

    // Reset connection for reuse (e.g., in keep-alive scenarios)
    void reset();

    // Clear only the outgoing-response state (headers/body/file stream), leaving
    // any buffered pipelined request data in read_buffer_ intact.
    void clear_response_state();

    // Read data from socket into read_buffer_. Returns bytes read, 0 if connection closed, -1 on error.
    ssize_t read_data();

    // Write data to socket. Handles both string bodies and file streams.
    // Returns bytes written, 0 if nothing to write, -1 on error.
    ssize_t write_data();
    
    // Set the content to be written (either a string or a file path)
    void set_response_content(const Http::HttpResponse& response); // Add Http:: prefix

    // Try to parse one complete request from the front of read_buffer_ WITHOUT
    // mutating the buffer. On success, current_request_ holds views into
    // read_buffer_ and `consumed` is the number of bytes this request occupies.
    // The caller must take ownership of the request (HttpRequest::make_owned)
    // and then call consume() before parsing the next one.
    // Returns false if more data is needed; check parser_failed() for errors.
    bool parse_next(size_t& consumed);

    // Whether the last parse_next() left the parser in an error state.
    bool parser_failed() const;

    // Drop `n` bytes from the front of the read buffer.
    void consume(size_t n);

    // Close the socket connection
    void close_connection();

    // Update last activity timestamp
    void update_activity() { last_activity_ = std::chrono::steady_clock::now(); }
    std::chrono::steady_clock::time_point get_last_activity() const { return last_activity_; }
    
    bool is_open() const { return socket_fd_ >= 0; }
    bool has_data_to_write() const; // Check if there's any pending data (string or file)
};

} // namespace Net
} // namespace Oreshnek

#endif // ORESHNEK_NET_CONNECTION_H
