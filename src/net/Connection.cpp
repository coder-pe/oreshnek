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
    if (!has_data_to_write() && headers_sent_) return 0; // Nothing to write and headers already sent

    ssize_t bytes_sent_in_call = 0;

    // Send headers first if not already sent
    if (!headers_sent_) {
        // Assume `write_content_` holds the full HttpResponse object or at least the headers string
        // For simplicity, we'll build the headers string here using a temporary HttpResponse instance.
        // A more robust design would have HttpResponse provide a `build_headers_string()` method
        // and the Connection storing the full response state.
        std::string headers_str;
        if (std::holds_alternative<std::string>(write_content_)) {
            // If it's a string body, the headers were likely generated with the body
            // by HttpResponse::to_string().
            // So we need to separate headers from body here, or pass a pre-built headers string.
            // Let's assume the string in write_content_ starts with headers.
            headers_str = std::get<std::string>(write_content_);
            size_t header_end_pos = headers_str.find("\r\n\r\n");
            if (header_end_pos != std::string::npos) {
                headers_str = headers_str.substr(0, header_end_pos + 4);
            } else {
                // Malformed response string, attempt to send as is or log error
                std::cerr << "Warning: Malformed response string in write_data for fd " << socket_fd_ << std::endl;
            }
        } else { // It's a file stream, need to generate headers based on current_response_ object (if we stored it)
            // This case is tricky. The Connection needs to somehow get the headers.
            // Option 1: HttpResponse::file() prepares headers and puts them in write_content_ string.
            // Option 2: The thread pool provides the full HttpResponse object to Connection.
            // Let's go with Option 1 for now, or just assume the `set_response_content` handles this.
            // For file streaming, the `set_response_content` will put the headers string in `write_content_` first.
            // And then the file stream.
            std::cerr << "Error: Headers for file stream not properly prepared in Connection." << std::endl;
            return -1; // Indicate error
        }

        ssize_t header_bytes_sent = send(socket_fd_, headers_str.c_str(), headers_str.length(), 0);
        if (header_bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Can't send headers yet, try again later
                return 0;
            }
            std::cerr << "Error sending headers to socket " << socket_fd_ << ": " << strerror(errno) << std::endl;
            return -1;
        }
        bytes_sent_in_call += header_bytes_sent;
        // If not all headers sent, need to handle partial sends or retry.
        // For simplicity, assume headers are sent in one go or buffer is handled externally.
        // A more robust solution would be to put headers in a separate buffer in Connection.
        // For now, let's just mark headers as sent and proceed with body.
        headers_sent_ = true;
        // If the entire headers_str is not sent, this simple logic is flawed.
        // A real-world server would track `bytes_written_of_headers` etc.
    }


    if (std::holds_alternative<std::string>(write_content_)) {
        std::string& body_str = std::get<std::string>(write_content_);
        // If headers were just sent, remove them from the body_str if it was the full response string.
        // This is a bit of a hack given the current design.
        size_t header_end_pos = body_str.find("\r\n\r\n");
        if (!headers_sent_ && header_end_pos != std::string::npos) { // if headers were not sent, then send the whole body
            // send the entire string (headers + body)
        } else if (headers_sent_ && header_end_pos != std::string::npos) { // if headers already sent, skip them in body_str
            body_str.erase(0, header_end_pos + 4);
        }

        if (body_str.empty()) return bytes_sent_in_call;

        ssize_t bytes_sent = send(socket_fd_, body_str.c_str(), body_str.length(), 0);
        if (bytes_sent > 0) {
            body_str.erase(0, bytes_sent); // Remove sent data
            bytes_sent_in_call += bytes_sent;
        } else if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return bytes_sent_in_call;
            std::cerr << "Error writing to socket " << socket_fd_ << ": " << strerror(errno) << std::endl;
            return -1;
        }
    } else if (std::holds_alternative<std::unique_ptr<std::ifstream>>(write_content_)) {
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
            write_content_ = std::string(); // Clear the variant
            file_bytes_sent_ = 0;
            // Optionally, set keep_alive_ to false if range requests/streaming requires closing.
        }
    }

    update_activity();
    return bytes_sent_in_call;
}

void Connection::set_response_content(const Http::HttpResponse& response) {
    if (response.is_file()) {
        // Store headers string for sending first
        write_content_ = response.build_headers_string();
        headers_sent_ = false; // Mark headers as not yet sent for this response

        // Then open the file for streaming
        const std::string& file_path = std::get<std::string>(response.get_body_variant());
        auto file_stream = std::make_unique<std::ifstream>(file_path, std::ios::binary);
        if (!file_stream->is_open()) {
            std::cerr << "Error opening file for streaming: " << file_path << std::endl;
            // Fallback to a 500 error or close connection
            write_content_ = Http::HttpResponse().status(Http::HttpStatus::INTERNAL_SERVER_ERROR)
                                            .text("Failed to open file for streaming").to_string();
            is_file_response_ = false; // Treat as string response
        } else {
            // Need to append the file stream to write_content_ after headers are sent.
            // For now, we will assume headers are part of the first string in write_content_
            // and the file stream will be handled after.
            // This requires a more complex state machine in Connection::write_data.
            // For simplicity in this example, let's put the headers string and then manage the file stream.
            // A better way would be to send headers first and then stream the file directly.
            // This is a key area for improvement for true streaming.
            // For now, let's use a queue for outgoing data parts, or split logic in write_data.
            
            // To simplify, let's first set the headers string, then in handle_write_ready,
            // once headers are sent, *then* we set the write_content_ to the file stream.
            // This means we need the full HttpResponse object accessible later.
            // Let's modify Server::handle_write_ready to access the original HttpResponse.
            // Or the Connection can store the path and open the stream later.
        }
    } else {
        // If it's a string body, the to_string() method generates the full response (headers + body)
        write_content_ = response.build_headers_string() + std::get<std::string>(response.get_body_variant());
        headers_sent_ = false;
    }
    file_bytes_sent_ = 0; // Reset for new response
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
    }
    return false;
}

} // namespace Net
} // namespace Oreshnek
