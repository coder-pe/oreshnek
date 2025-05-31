// oreshnek/src/net/Connection.cpp
#include "oreshnek/net/Connection.h"
#include <unistd.h> // For close, read, write
#include <sys/socket.h> // For recv, send
#include <errno.h>    // For errno
#include <cstring>    // For strerror
#include <iostream>   // For cerr

namespace Oreshnek {
namespace Net {

Connection::Connection(int fd)
    : socket_fd_(fd),
      read_buffer_(READ_BUFFER_SIZE), // Allocate buffer
      read_buffer_fill_(0),
      last_activity_(std::chrono::steady_clock::now()) {
    // Constructor sets up initial state.
}

Connection::~Connection() {
    close_connection();
}

void Connection::reset() {
    read_buffer_fill_ = 0;
    write_buffer_.clear();
    http_parser_.reset();
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
    if (write_buffer_.empty()) return 0;

    ssize_t bytes_sent = send(socket_fd_, write_buffer_.data(), write_buffer_.length(), 0);

    if (bytes_sent > 0) {
        write_buffer_.erase(0, bytes_sent); // Remove sent data
        update_activity();
    } else if (bytes_sent == 0) {
        // Should not happen for non-empty buffer unless socket is in a strange state
        return 0;
    } else { // bytes_sent < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Buffer full, try again later
            return 0;
        }
        // Real error
        std::cerr << "Error writing to socket " << socket_fd_ << ": " << strerror(errno) << std::endl;
        return -1;
    }
    return bytes_sent;
}

void Connection::append_to_write_buffer(std::string_view data) {
    write_buffer_.append(data.data(), data.length());
}

bool Connection::process_read_buffer() {
    if (read_buffer_fill_ == 0) return false; // No data to process

    size_t bytes_consumed = 0;
    std::string_view buffer_view(read_buffer_.data(), read_buffer_fill_);

    bool request_complete = http_parser_.parse_request(buffer_view, bytes_consumed, current_request_);

    if (http_parser_.get_state() == Http::ParsingState::ERROR) {
        std::cerr << "HTTP parsing error for fd " << socket_fd_ << ": " << http_parser_.get_error_message() << std::endl;
        // Optionally, reset parser and close connection on error
        reset(); // Or simply close connection to prevent further issues
        return false;
    }

    if (bytes_consumed > 0) {
        // Shift remaining data to the beginning of the buffer
        // Note: For large requests, this could be slow. Consider a circular buffer or more complex management.
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

} // namespace Net
} // namespace Oreshnek
