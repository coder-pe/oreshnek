// oreshnek/src/net/Connection.cpp
#include "oreshnek/net/Connection.h"
#include <unistd.h> // For close, read, write, unlink
#include <sys/socket.h> // For recv, send
#include <sys/stat.h>   // For fstat
#include <fcntl.h>      // For open
#include <cstdlib>      // For mkstemp
#include <errno.h>    // For errno
#include <cstring>    // For strerror
#include <algorithm>  // For std::min
#include <cctype>     // For std::tolower
#include <climits>    // For INT_MAX
#include <openssl/ssl.h> // For TLS (SSL_read/SSL_write/SSL_accept)
#include "oreshnek/utils/Logger.h"

#ifdef __linux__
#include <sys/sendfile.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/socket.h>
#endif

// Avoid SIGPIPE on writes to a peer that closed the connection. Linux supports
// the per-call MSG_NOSIGNAL flag; on platforms without it (e.g. macOS) this is a
// no-op and the SO_NOSIGPIPE socket option / SIG_IGN is relied upon instead.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace Oreshnek {
namespace Net {

Connection::Connection(int fd)
    : socket_fd_(fd),
      read_buffer_(READ_BUFFER_SIZE), // Allocate buffer
      read_buffer_fill_(0),
      last_activity_(std::chrono::steady_clock::now()) {
}

Connection::~Connection() {
    close_connection();
}

void Connection::reset() {
    read_buffer_fill_ = 0;
    http_parser_.reset();
    current_request_ = Http::HttpRequest(); // Reset HttpRequest
    keep_alive_ = true; // Assume keep-alive by default for new requests
    processing_ = false;
    worker_in_flight_ = false;
    continue_sent_ = false;
    // Streaming state is already torn down at hand-off (finish_body_spool) or on
    // abort (close_connection); reset defensively for keep-alive reuse.
    body_mode_ = BodyMode::Buffered;
    body_remaining_ = 0;
    clear_response_state();
    update_activity();
}

void Connection::clear_response_state() {
    headers_sent_ = false;
    raw_headers_to_send_.clear();
    write_body_.clear();
    write_body_offset_ = 0;
    head_only_ = false;
    if (file_fd_ >= 0) {
        close(file_fd_);
        file_fd_ = -1;
    }
    file_offset_ = 0;
    file_remaining_ = 0;
}

int Connection::continue_tls_handshake() {
    if (ssl_ == nullptr) { tls_handshake_done_ = true; return 1; }
    int r = SSL_accept(ssl_);
    if (r == 1) {
        tls_handshake_done_ = true;
        update_activity();
        return 1;
    }
    switch (SSL_get_error(ssl_, r)) {
        case SSL_ERROR_WANT_READ:  tls_want_ = TlsWant::Read;  return 0;
        case SSL_ERROR_WANT_WRITE: tls_want_ = TlsWant::Write; return 0;
        default:
            ORE_LOG(WARN) << "TLS handshake failed on fd " << socket_fd_;
            return -1;
    }
}

ssize_t Connection::read_data() {
    if (socket_fd_ < 0) return 0; // Connection already closed

    size_t available_space = read_buffer_.size() - read_buffer_fill_;
    if (available_space == 0) {
        // Buffer full, can't read more for now. This should ideally not happen
        // if parse_request is called after each read and consumes data.
        ORE_LOG(WARN) << "Read buffer full for fd " << socket_fd_;
        return 0;
    }

    // TLS path: drain SSL_read fully, since edge-triggered epoll/kqueue will not
    // re-notify for bytes already buffered inside OpenSSL.
    if (ssl_ != nullptr) {
        size_t total = 0;
        for (;;) {
            size_t space = read_buffer_.size() - read_buffer_fill_;
            if (space == 0) break; // Buffer full; process what we have.
            int n = SSL_read(ssl_, read_buffer_.data() + read_buffer_fill_,
                             static_cast<int>(std::min<size_t>(space, INT_MAX)));
            if (n > 0) {
                read_buffer_fill_ += static_cast<size_t>(n);
                total += static_cast<size_t>(n);
                continue;
            }
            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ)  { tls_want_ = TlsWant::Read;  break; }
            if (err == SSL_ERROR_WANT_WRITE) { tls_want_ = TlsWant::Write; break; }
            if (err == SSL_ERROR_ZERO_RETURN) { // peer sent close_notify
                return total > 0 ? static_cast<ssize_t>(total) : 0;
            }
            if (total > 0) break; // Surface the error on the next read.
            ORE_LOG(ERROR) << "SSL_read error on socket " << socket_fd_;
            return -1;
        }
        if (total > 0) { update_activity(); return static_cast<ssize_t>(total); }
        return kReadWouldBlock;
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
            // No data currently available, but connection is still open.
            return kReadWouldBlock;
        }
        // Real error
        ORE_LOG(ERROR) << "Error reading from socket " << socket_fd_ << ": " << strerror(errno);
        return -1;
    }
    return bytes_read;
}

ssize_t Connection::write_data() {
    if (socket_fd_ < 0) return 0; // Connection already closed

    // TLS path: sendfile() cannot encrypt, so file bodies are read into a buffer
    // and written through SSL_write like any other body.
    if (ssl_ != nullptr) {
        ssize_t sent = 0;
        if (!headers_sent_) {
            while (!raw_headers_to_send_.empty()) {
                int n = SSL_write(ssl_, raw_headers_to_send_.data(),
                                  static_cast<int>(std::min<size_t>(raw_headers_to_send_.size(), INT_MAX)));
                if (n > 0) { sent += n; raw_headers_to_send_.erase(0, static_cast<size_t>(n)); continue; }
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_WRITE) { tls_want_ = TlsWant::Write; return sent; }
                if (err == SSL_ERROR_WANT_READ)  { tls_want_ = TlsWant::Read;  return sent; }
                ORE_LOG(ERROR) << "SSL_write (headers) error on socket " << socket_fd_;
                return -1;
            }
            headers_sent_ = true;
        }
        if (head_only_) { update_activity(); return sent; }

        if (file_fd_ >= 0) {
            char buf[16384];
            while (file_remaining_ > 0) {
                size_t want = std::min<size_t>(static_cast<size_t>(file_remaining_), sizeof(buf));
                ssize_t r = pread(file_fd_, buf, want, file_offset_);
                if (r <= 0) break; // Unexpected EOF (file shrank); stop.
                int n = SSL_write(ssl_, buf, static_cast<int>(r));
                if (n > 0) {
                    file_offset_ += n;
                    file_remaining_ -= n;
                    sent += n;
                    continue;
                }
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_WRITE) { tls_want_ = TlsWant::Write; return sent; }
                if (err == SSL_ERROR_WANT_READ)  { tls_want_ = TlsWant::Read;  return sent; }
                ORE_LOG(ERROR) << "SSL_write (file) error on socket " << socket_fd_;
                return -1;
            }
            if (file_remaining_ <= 0 && file_fd_ >= 0) { close(file_fd_); file_fd_ = -1; }
            update_activity();
            return sent;
        }

        if (write_body_offset_ < write_body_.size()) {
            int n = SSL_write(ssl_, write_body_.data() + write_body_offset_,
                              static_cast<int>(std::min<size_t>(write_body_.size() - write_body_offset_, INT_MAX)));
            if (n > 0) {
                write_body_offset_ += static_cast<size_t>(n);
                sent += n;
            } else {
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_WRITE) { tls_want_ = TlsWant::Write; return sent; }
                if (err == SSL_ERROR_WANT_READ)  { tls_want_ = TlsWant::Read;  return sent; }
                ORE_LOG(ERROR) << "SSL_write (body) error on socket " << socket_fd_;
                return -1;
            }
        }
        update_activity();
        return sent;
    }

    ssize_t bytes_sent_in_call = 0;

    // 1) Send headers first.
    if (!headers_sent_) {
        if (!raw_headers_to_send_.empty()) {
            ssize_t n = send(socket_fd_, raw_headers_to_send_.c_str(),
                             raw_headers_to_send_.length(), MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                ORE_LOG(ERROR) << "Error sending headers to socket " << socket_fd_ << ": " << strerror(errno);
                return -1;
            }
            bytes_sent_in_call += n;
            raw_headers_to_send_.erase(0, n);
        }
        if (!raw_headers_to_send_.empty()) {
            return bytes_sent_in_call; // Headers not fully flushed yet.
        }
        headers_sent_ = true;
    }

    // HEAD responses carry no body.
    if (head_only_) {
        update_activity();
        return bytes_sent_in_call;
    }

    // 2) Send a file body with zero-copy sendfile().
    if (file_fd_ >= 0) {
        while (file_remaining_ > 0) {
            size_t count = static_cast<size_t>(
                std::min<off_t>(file_remaining_, static_cast<off_t>(FILE_SEND_CHUNK)));
#ifdef __linux__
            off_t off = file_offset_;
            ssize_t n = ::sendfile(socket_fd_, file_fd_, &off, count);
            if (n > 0) {
                file_offset_ = off;
                file_remaining_ -= n;
                bytes_sent_in_call += n;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return bytes_sent_in_call;
            if (n == 0) break; // Unexpected EOF (file shrank); stop.
            ORE_LOG(ERROR) << "sendfile error on socket " << socket_fd_ << ": " << strerror(errno);
            return -1;
#elif defined(__APPLE__)
            off_t len = static_cast<off_t>(count);
            int r = ::sendfile(file_fd_, socket_fd_, file_offset_, &len, nullptr, 0);
            if (len > 0) {
                file_offset_ += len;
                file_remaining_ -= len;
                bytes_sent_in_call += len;
            }
            if (r == 0) {
                if (len == 0) break; // Nothing more could be read.
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return bytes_sent_in_call;
            ORE_LOG(ERROR) << "sendfile error on socket " << socket_fd_ << ": " << strerror(errno);
            return -1;
#endif
        }
        if (file_remaining_ <= 0 && file_fd_ >= 0) {
            close(file_fd_);
            file_fd_ = -1;
        }
        update_activity();
        return bytes_sent_in_call;
    }

    // 3) Send an in-memory string body (tracking an offset, no front-erase).
    if (write_body_offset_ < write_body_.size()) {
        ssize_t n = send(socket_fd_, write_body_.data() + write_body_offset_,
                         write_body_.size() - write_body_offset_, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return bytes_sent_in_call;
            ORE_LOG(ERROR) << "Error writing body to socket " << socket_fd_ << ": " << strerror(errno);
            return -1;
        }
        write_body_offset_ += static_cast<size_t>(n);
        bytes_sent_in_call += n;
    }

    update_activity();
    return bytes_sent_in_call;
}

void Connection::set_response_content(const Http::HttpResponse& response) {
    clear_response_state();
    raw_headers_to_send_ = response.build_headers_string();
    head_only_ = response.head_only();

    if (response.is_file()) {
        const std::string& path = response.file_path();
        file_fd_ = ::open(path.c_str(), O_RDONLY);
        if (file_fd_ < 0) {
            ORE_LOG(ERROR) << "Error opening file for response: " << path << ": " << strerror(errno);
            file_remaining_ = 0;
            return;
        }
        file_offset_ = static_cast<off_t>(response.file_offset());
        off_t length = static_cast<off_t>(response.file_length());
        if (length < 0) {
            // Whole file from the given offset: derive the size.
            struct stat st;
            if (fstat(file_fd_, &st) == 0) {
                length = st.st_size - file_offset_;
                if (length < 0) length = 0;
            } else {
                length = 0;
            }
        }
        file_remaining_ = length;
    } else {
        write_body_ = std::get<std::string>(response.get_body_variant());
    }
}


bool Connection::parse_next(size_t& consumed) {
    consumed = 0;
    if (read_buffer_fill_ == 0) return false; // No data to process

    // Parse the whole pending buffer from a clean state. We do not consume the
    // bytes here, so re-parsing the same prefix across successive reads (until a
    // request is complete) is idempotent.
    http_parser_.reset();
    current_request_ = Http::HttpRequest();

    std::string_view buffer_view(read_buffer_.data(), read_buffer_fill_);
    bool request_complete = http_parser_.parse_request(buffer_view, consumed, current_request_);

    if (http_parser_.get_state() == Http::ParsingState::ERROR) {
        ORE_LOG(WARN) << "HTTP parsing error for fd " << socket_fd_ << ": "
                      << http_parser_.get_error_message();
        return false;
    }
    return request_complete;
}

bool Connection::parser_failed() const {
    return http_parser_.get_state() == Http::ParsingState::ERROR;
}

void Connection::consume(size_t n) {
    if (n == 0) return;
    if (n >= read_buffer_fill_) {
        read_buffer_fill_ = 0;
        return;
    }
    std::memmove(read_buffer_.data(), read_buffer_.data() + n, read_buffer_fill_ - n);
    read_buffer_fill_ -= n;
}

Http::ParseHeadersResult Connection::peek_headers(size_t& header_len,
                                                  std::uint64_t& content_length, bool& chunked) {
    // A throwaway parser/request over the buffered bytes: does not touch the
    // connection's own parser or current_request_.
    Http::HttpParser tmp;
    Http::HttpRequest tmp_req;
    std::string_view view(read_buffer_.data(), read_buffer_fill_);
    Http::ParseHeadersResult r = tmp.parse_headers_only(view, header_len, tmp_req);
    if (r == Http::ParseHeadersResult::Ready) {
        content_length = tmp.content_length();
        chunked = tmp.is_chunked();
    }
    return r;
}

bool Connection::begin_body_spool(const std::string& dir, size_t header_len,
                                  std::uint64_t content_length) {
    // Keep the header block so the request can be rebuilt once the body lands.
    upload_header_bytes_.assign(read_buffer_.data(), header_len);

    std::string tmpl = dir + "/upload-XXXXXX";
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    int fd = ::mkstemp(path.data());
    if (fd < 0) {
        ORE_LOG(ERROR) << "mkstemp failed in '" << dir << "': " << strerror(errno);
        return false;
    }
    body_file_fd_ = fd;
    body_file_path_.assign(path.data());
    body_remaining_ = content_length;
    body_mode_ = BodyMode::Streaming;

    // Move any body bytes already buffered after the headers to the front, then
    // spool them.
    if (read_buffer_fill_ > header_len) {
        size_t body_in_buf = read_buffer_fill_ - header_len;
        std::memmove(read_buffer_.data(), read_buffer_.data() + header_len, body_in_buf);
        read_buffer_fill_ = body_in_buf;
    } else {
        read_buffer_fill_ = 0;
    }
    return spool_from_buffer() >= 0;
}

ssize_t Connection::spool_from_buffer() {
    if (body_mode_ != BodyMode::Streaming || body_file_fd_ < 0) return 0;

    size_t to_write = static_cast<size_t>(
        std::min<std::uint64_t>(read_buffer_fill_, body_remaining_));
    size_t off = 0;
    while (off < to_write) {
        ssize_t n = ::write(body_file_fd_, read_buffer_.data() + off, to_write - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ORE_LOG(ERROR) << "write to spool file failed: " << strerror(errno);
            return -1;
        }
        off += static_cast<size_t>(n);
    }
    body_remaining_ -= to_write;

    // Preserve any surplus (a pipelined next request) at the front of the buffer.
    size_t leftover = read_buffer_fill_ - to_write;
    if (leftover > 0) {
        std::memmove(read_buffer_.data(), read_buffer_.data() + to_write, leftover);
    }
    read_buffer_fill_ = leftover;
    update_activity();
    return static_cast<ssize_t>(to_write);
}

void Connection::finish_body_spool(bool keep) {
    if (body_file_fd_ >= 0) {
        close(body_file_fd_);
        body_file_fd_ = -1;
    }
    if (!keep && !body_file_path_.empty()) {
        ::unlink(body_file_path_.c_str());
    }
    body_file_path_.clear();
    body_remaining_ = 0;
    body_mode_ = BodyMode::Buffered;
    upload_header_bytes_.clear();
}

void Connection::maybe_send_100_continue() {
    if (continue_sent_ || socket_fd_ < 0) return;
    // Only once the headers are parsed and a body is awaited.
    if (http_parser_.get_state() != Http::ParsingState::BODY) return;

    auto expect = current_request_.header("Expect");
    if (!expect) return;
    // Case-insensitive check for "100-continue".
    std::string value(*expect);
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (value.find("100-continue") == std::string::npos) return;

    static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (ssl_ != nullptr) {
        SSL_write(ssl_, kContinue, sizeof(kContinue) - 1); // best-effort, over TLS
    } else {
        ::send(socket_fd_, kContinue, sizeof(kContinue) - 1, MSG_NOSIGNAL); // best-effort
    }
    continue_sent_ = true;
}

void Connection::close_connection() {
    if (file_fd_ >= 0) {
        close(file_fd_);
        file_fd_ = -1;
    }
    // A spool file still owned here means the upload was aborted mid-flight
    // (client vanished / error before hand-off): drop the partial temp file.
    if (body_file_fd_ >= 0) {
        close(body_file_fd_);
        body_file_fd_ = -1;
    }
    if (!body_file_path_.empty()) {
        ::unlink(body_file_path_.c_str());
        body_file_path_.clear();
    }
    body_mode_ = BodyMode::Buffered;
    body_remaining_ = 0;
    if (ssl_ != nullptr) {
        // Best-effort close_notify; SSL_set_fd uses BIO_NOCLOSE so SSL_free does
        // not close the socket (we close it ourselves below).
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (socket_fd_ >= 0) {
        ORE_LOG(DEBUG) << "Closing connection " << socket_fd_;
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

bool Connection::has_data_to_write() const {
    if (!raw_headers_to_send_.empty()) return true; // Headers still pending.
    if (head_only_) return false;                   // HEAD: no body.
    if (file_fd_ >= 0 && file_remaining_ > 0) return true;
    return write_body_offset_ < write_body_.size();
}

} // namespace Net
} // namespace Oreshnek
