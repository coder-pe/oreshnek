// oreshnek/include/oreshnek/net/Connection.h
#ifndef ORESHNEK_NET_CONNECTION_H
#define ORESHNEK_NET_CONNECTION_H

#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h" // Include this to get FilePath definition
#include "oreshnek/http/HttpParser.h"
#include <string>
#include <vector>
#include <chrono>
#include <memory> // For unique_ptr
#include <fstream> // For file streaming
#include <variant> // For std::variant

namespace Oreshnek {
namespace Net {

class Connection {
public:
    static constexpr size_t READ_BUFFER_SIZE = 8192; // Common buffer size for network I/O
    static constexpr size_t WRITE_BUFFER_CHUNK_SIZE = 4096; // Chunk size for sending file data

    int socket_fd_;
    std::vector<char> read_buffer_; // Buffer for incoming data
    size_t read_buffer_fill_ = 0; // Current fill level of the read buffer

    // This will now hold either a string response or an active file stream
    // Add FilePath to the variant to differentiate the original source
    std::variant<std::string, std::unique_ptr<std::ifstream>, Http::FilePath> write_content_;
    size_t file_bytes_sent_ = 0; // For file streaming: track bytes sent
    bool headers_sent_ = false; // To ensure headers are sent only once

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

    // Write data to socket. Handles both string bodies and file streams.
    // Returns bytes written, 0 if nothing to write, -1 on error.
    ssize_t write_data();
    
    // Set the content to be written (either a string or a file path)
    void set_response_content(const Http::HttpResponse& response); // Add Http:: prefix

    // Process the read buffer to parse an HTTP request.
    // Returns true if a complete request is parsed and available in current_request_.
    bool process_read_buffer();

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
