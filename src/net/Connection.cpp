// oreshnek/src/net/Connection.cpp
#include "oreshnek/net/Connection.h"
#include <unistd.h> // For close, read, write
#include <sys/socket.h> // For recv, send
#include <errno.h>    // For errno
#include <cstring>    // For strerror
#include <iostream>   // For cerr
#include <filesystem> // For file size

namespace Oreshnek {
namespace Net {

Connection::Connection(int fd)
    : socket_fd_(fd),
      read_buffer_(READ_BUFFER_SIZE), // Allocate buffer
      read_buffer_fill_(0),
      file_bytes_sent_(0),
      headers_sent_(false),
      last_activity_(std::chrono::steady_clock::now()) {
    write_content_ = std::string(); // Initialize with an empty string
}

Connection::~Connection() {
    close_connection();
}

void Connection::reset() {
    read_buffer_fill_ = 0;
    write_content_ = std::string(); // Reset to empty string
    file_bytes_sent_ = 0;
    headers_sent_ = false;
    http_parser_.reset();
    current_request_ = Http::HttpRequest(); // Reset HttpRequest
    keep_alive_ = true; // Assume keep-alive by default for new requests
    raw_headers_to_send_.clear(); // Clear raw headers
    file_path_to_stream_.clear(); // Clear file path
    update_activity();
}

ssize_t Connection::read_data() {
    if (socket_fd_ < 0) return 0; // Connection already closed

    // Shift unread data to the beginning of the buffer
    if (read_buffer_fill_ > 0 && read_buffer_fill_ < read_buffer_.size()) {
        std::memmove(read_buffer_.data(), read_buffer_.data(), read_buffer_fill_);
    }

    size_t available_space = read_buffer_.size() - read_buffer_fill_;
    if (available_space == 0) {
        // Buffer full, can't read more for now. This should ideally not happen
        // if parse_request is called after each read and consumes data.
        std::cerr << "Warning: Read buffer full for fd " << socket_fd_ << std::endl;
        return 0;
    }

    ssize_t bytes_read = recv(socket_fd_, read_buffer_.data() + read_buffer_fill_, available_space, 0);

    if (bytes_read > 0) {
        read_buffer_fill_ += bytes_read;
        update_activity();
    } else if (bytes_read == 0) {
        // Client closed connection gracefully
        return 0;
    } else { // bytes_read < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data currently available, but connection is still open
            return 0;
        }
        // Real error
        std::cerr << "Error reading from socket " << socket_fd_ << ": " << strerror(errno) << std::endl;
        return -1;
    }
    return bytes_read;
}

ssize_t Connection::write_data() {
    if (socket_fd_ < 0) return 0; // Connection already closed

    ssize_t bytes_sent_in_call = 0;

    // Send headers first if not already sent
    if (!headers_sent_) {
        if (!raw_headers_to_send_.empty()) {
            ssize_t header_bytes_to_send = raw_headers_to_send_.length();
            ssize_t header_bytes_sent = send(socket_fd_, raw_headers_to_send_.c_str(), header_bytes_to_send, 0);

            if (header_bytes_sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0; // Try again later
                }
                std::cerr << "Error sending headers to socket " << socket_fd_ << ": " << strerror(errno) << std::endl;
                return -1;
            } else {
                bytes_sent_in_call += header_bytes_sent;
                raw_headers_to_send_.erase(0, header_bytes_sent); // Remove sent data

                if (raw_headers_to_send_.empty()) {
                    headers_sent_ = true; // All headers sent
                    // If it's a file response, now open the file for streaming
                    if (!file_path_to_stream_.empty()) {
                        auto file_stream = std::make_unique<std::ifstream>(file_path_to_stream_, std::ios::binary);
                        if (!file_stream->is_open()) {
                            std::cerr << "Error opening file for streaming: " << file_path_to_stream_ << std::endl;
                            write_content_ = std::string(); // Clear variant
                            file_path_to_stream_.clear();
                            return -1;
                        }
                        write_content_ = std::move(file_stream); // Transition to file stream
                        file_bytes_sent_ = 0;
                    }
                }
            }
        }
    }


    if (headers_sent_) { // Only attempt to send body if headers are sent
        if (std::holds_alternative<std::string>(write_content_)) {
            // This is for string bodies
            std::string& body_str = std::get<std::string>(write_content_);
            if (!body_str.empty()) {
                ssize_t body_bytes_sent = send(socket_fd_, body_str.c_str(), body_str.length(), 0);
                if (body_bytes_sent < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return bytes_sent_in_call;
                    std::cerr << "Error writing string body to socket " << socket_fd_ << ": " << strerror(errno) << std::endl;
                    return -1;
                } else {
                    bytes_sent_in_call += body_bytes_sent;
                    body_str.erase(0, body_bytes_sent); // Remove sent data
                }
            }
        } else if (std::holds_alternative<std::unique_ptr<std::ifstream>>(write_content_)) {
            // This is for file streaming
            std::ifstream* file_stream = std::get<std::unique_ptr<std::ifstream>>(write_content_).get();
            if (!file_stream || !file_stream->is_open()) {
                std::cerr << "Error: File stream not open for fd " << socket_fd_ << std::endl;
                return -1;
            }

            std::vector<char> buffer(WRITE_BUFFER_CHUNK_SIZE);
            file_stream->read(buffer.data(), WRITE_BUFFER_CHUNK_SIZE);
            ssize_t bytes_to_send = file_stream->gcount();

            if (bytes_to_send > 0) {
                ssize_t bytes_sent = send(socket_fd_, buffer.data(), bytes_to_send, 0);
                if (bytes_sent > 0) {
                    file_bytes_sent_ += bytes_sent;
                    // If partial send, rewind file stream
                    if (bytes_sent < bytes_to_send) {
                        file_stream->seekg(file_stream->tellg() - (bytes_to_send - bytes_sent));
                    }
                    bytes_sent_in_call += bytes_sent;
                } else if (bytes_sent < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return bytes_sent_in_call;
                    std::cerr << "Error writing file data to socket " << socket_fd_ << ": " << strerror(errno) << std::endl;
                    return -1;
                }
            } else if (file_stream->eof()) {
                // End of file, all data sent. Close the stream.
                file_stream->close();
                write_content_ = std::string(); // Clear the variant to empty string
                file_bytes_sent_ = 0;
                file_path_to_stream_.clear(); // Clear the path
            }
        }
    }

    update_activity();
    return bytes_sent_in_call;
}

void Connection::set_response_content(const Http::HttpResponse& response) {
    // Reset state for new response
    headers_sent_ = false;
    file_bytes_sent_ = 0;
    raw_headers_to_send_ = response.build_headers_string(); // Store headers string

    if (response.is_file()) {
        file_path_to_stream_ = std::get<Http::FilePath>(response.get_body_variant()).path; // Store file path
        write_content_ = std::string(); // Initialize write_content_ to an empty string (will be replaced by ifstream later)
    } else {
        file_path_to_stream_.clear(); // Not a file response
        write_content_ = std::get<std::string>(response.get_body_variant()); // Store the body string directly
    }
}


bool Connection::process_read_buffer() {
    if (read_buffer_fill_ == 0) return false; // No data to process

    size_t bytes_consumed = 0;
    std::string_view buffer_view(read_buffer_.data(), read_buffer_fill_);

    bool request_complete = http_parser_.parse_request(buffer_view, bytes_consumed, current_request_);

    if (http_parser_.get_state() == Http::ParsingState::ERROR) {
        std::cerr << "HTTP parsing error for fd " << socket_fd_ << ": " << http_parser_.get_error_message() << std::endl;
        reset(); // Or simply close connection to prevent further issues
        return false;
    }

    if (bytes_consumed > 0) {
        // Shift remaining data to the beginning of the buffer
        std::memmove(read_buffer_.data(), read_buffer_.data() + bytes_consumed, read_buffer_fill_ - bytes_consumed);
        read_buffer_fill_ -= bytes_consumed;
    }

    return request_complete;
}

void Connection::close_connection() {
    if (socket_fd_ >= 0) {
        std::cout << "Closing connection " << socket_fd_ << std::endl;
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

bool Connection::has_data_to_write() const {
    // If headers haven't been sent yet, we have headers to write
    if (!raw_headers_to_send_.empty()) {
        return true;
    }
    // If headers are sent, check the actual body content
    if (std::holds_alternative<std::string>(write_content_)) {
        return !std::get<std::string>(write_content_).empty();
    } else if (std::holds_alternative<std::unique_ptr<std::ifstream>>(write_content_)) {
        const auto& stream_ptr = std::get<std::unique_ptr<std::ifstream>>(write_content_);
        return stream_ptr && stream_ptr->is_open() && !stream_ptr->eof();
    }
    return false;
}

} // namespace Net
} // namespace Oreshnek
