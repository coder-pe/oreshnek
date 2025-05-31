// oreshnek/include/oreshnek/net/Connection.h
#ifndef ORESHNEK_NET_CONNECTION_H
#define ORESHNEK_NET_CONNECTION_H

#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"
#include "oreshnek/http/HttpParser.h"
#include <string>
#include <vector>
#include <chrono>
#include <memory> // For unique_ptr

namespace Oreshnek {
namespace Net {

class Connection {
public:
    static constexpr size_t READ_BUFFER_SIZE = 8192; // Common buffer size for network I/O

    int socket_fd_;
    std::vector<char> read_buffer_; // Buffer for incoming data
    size_t read_buffer_fill_ = 0; // Current fill level of the read buffer
    std::string write_buffer_; // Buffer for outgoing data

    Http::HttpParser http_parser_;
    Http::HttpRequest current_request_; // Holds the parsed request data
    
    std::chrono::steady_clock::time_point last_activity_;
    bool keep_alive_ = true;

    Connection(int fd);
    ~Connection();

    // Reset connection for reuse (e.g., in keep-alive scenarios)
    void reset();

    // Read data from socket into read_buffer_. Returns bytes read, 0 if connection closed, -1 on error.
    ssize_t read_data();

    // Write data from write_buffer_ to socket. Returns bytes written, 0 if nothing to write, -1 on error.
    ssize_t write_data();
    
    // Append data to the write buffer
    void append_to_write_buffer(std::string_view data);

    // Process the read buffer to parse an HTTP request.
    // Returns true if a complete request is parsed and available in current_request_.
    bool process_read_buffer();

    // Close the socket connection
    void close_connection();

    // Update last activity timestamp
    void update_activity() { last_activity_ = std::chrono::steady_clock::now(); }
    std::chrono::steady_clock::time_point get_last_activity() const { return last_activity_; }
    
    bool is_open() const { return socket_fd_ >= 0; }
};

} // namespace Net
} // namespace Oreshnek

#endif // ORESHNEK_NET_CONNECTION_H
