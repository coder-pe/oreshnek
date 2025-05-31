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
        // The `write_content_` will hold the full headers string initially for file responses.
        // For string responses, it's also set with headers + body.
        if (std::holds_alternative<std::string>(write_content_)) {
            std::string& headers_and_body_str = std::get<std::string>(write_content_);
            
            ssize_t header_bytes_to_send = headers_and_body_str.length();
            ssize_t header_bytes_sent = send(socket_fd_, headers_and_body_str.c_str(), header_bytes_to_send, 0);

            if (header_bytes_sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0; // Try again later
                }
                std::cerr << "Error sending headers to socket " << socket_fd_ << ": " << strerror(errno) << std::endl;
                return -1;
            } else {
                bytes_sent_in_call += header_bytes_sent;
                headers_and_body_str.erase(0, header_bytes_sent); // Remove sent data

                if (headers_and_body_str.empty()) {
                    // All headers and body sent for string response.
                    // If it was a file response's initial header string, transition to file stream.
                    if (std::holds_alternative<Http::FilePath>(write_content_)) { // Check if it was originally a file path
                        const std::string& file_path = std::get<Http::FilePath>(write_content_).path;
                        auto file_stream = std::make_unique<std::ifstream>(file_path, std::ios::binary);
                        if (!file_stream->is_open()) {
                            std::cerr << "Error re-opening file for streaming: " << file_path << std::endl;
                            // Fallback to error or close
                            write_content_ = std::string(); // Clear variant
                            return -1;
                        }
                        write_content_ = std::move(file_stream); // Transition to file stream
                        headers_sent_ = true; // Headers for this response are now sent
                        file_bytes_sent_ = 0;
                    } else {
                        // It was a regular string body, all sent.
                        headers_sent_ = true; // Mark headers as sent.
                    }
                }
            }
            if (!std::holds_alternative<std::unique_ptr<std::ifstream>>(write_content_)) {
                // If we didn't transition to a file stream, and there's still data in the string (partial send)
                // or it was a full string body that finished, return the bytes sent.
                // We'll be called again if more data needs to be sent or if it's not finished.
                return bytes_sent_in_call;
            }
        }
    }


    if (std::holds_alternative<std::unique_ptr<std::ifstream>>(write_content_)) {
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
            // Optionally, set keep_alive_ to false if range requests/streaming requires closing.
        }
    }

    update_activity();
    return bytes_sent_in_call;
}

void Connection::set_response_content(const Http::HttpResponse& response) {
    // Reset state for new response
    headers_sent_ = false;
    file_bytes_sent_ = 0;

    if (response.is_file()) {
        // Store headers string as the first part to send
        write_content_ = response.build_headers_string();
        
        // Then store the file path as the third alternative to indicate a file response
        // This will be transitioned to std::unique_ptr<std::ifstream> once headers are sent
        write_content_ = Http::FilePath(std::get<Http::FilePath>(response.get_body_variant()));

    } else {
        // If it's a string body, the to_string() method generates the full response (headers + body)
        write_content_ = response.build_headers_string() + std::get<std::string>(response.get_body_variant());
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
    if (std::holds_alternative<std::string>(write_content_)) {
        return !std::get<std::string>(write_content_).empty();
    } else if (std::holds_alternative<std::unique_ptr<std::ifstream>>(write_content_)) {
        const auto& stream_ptr = std::get<std::unique_ptr<std::ifstream>>(write_content_);
        return stream_ptr && stream_ptr->is_open() && !stream_ptr->eof();
    } else if (std::holds_alternative<Http::FilePath>(write_content_)) {
        // If it's a FilePath, it means headers (and file stream) still need to be sent
        return true;
    }
    return false;
}

} // namespace Net
} // namespace Oreshnek
