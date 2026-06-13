// oreshnek/src/server/Server.cpp
#include "oreshnek/server/Server.h"
#include "oreshnek/utils/Logger.h"
#include <iostream>
#include <fcntl.h>    // For fcntl
#include <unistd.h>   // For close, pipe, read, write
#include <sys/socket.h> // For socket, bind, listen, accept
#include <netinet/in.h> // For sockaddr_in
#include <arpa/inet.h>  // For inet_ntoa
#include <errno.h>    // For errno
#include <cstring>    // For strerror
#include <vector>     // For cleanup_expired_connections
#include <utility>    // For std::swap

// Platform specific includes
#ifdef __linux__
#include <sys/sendfile.h>
#include <sys/epoll.h>
#elif __APPLE__
#include <sys/event.h>
#endif
#include <sys/stat.h> // For stat (to get file size and check existence)

namespace Oreshnek {
namespace Server {

Server::Server(size_t worker_threads)
    : listen_fd_(-1),
#ifdef __linux__
      epoll_fd_(-1),
#elif __APPLE__
      kqueue_fd_(-1),
#endif
      running_(false) {
    router_ = std::make_unique<Router>();
    thread_pool_ = std::make_unique<ThreadPool>(worker_threads);
}

Server::~Server() {
    stop(); // Ensure proper shutdown
}

void Server::set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        throw std::runtime_error("fcntl F_GETFL failed: " + std::string(strerror(errno)));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl F_SETFL failed: " + std::string(strerror(errno)));
    }
}

bool Server::setup_socket(const std::string& host, int port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        ORE_LOG(ERROR) << "Failed to create socket: " << strerror(errno);
        return false;
    }

    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ORE_LOG(ERROR) << "Failed to set SO_REUSEADDR: " << strerror(errno);
        close(listen_fd_);
        return false;
    }
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        ORE_LOG(ERROR) << "Failed to set SO_KEEPALIVE: " << strerror(errno);
    }

    try {
        set_non_blocking(listen_fd_);
    } catch (const std::runtime_error& e) {
        ORE_LOG(ERROR) << "Failed to set listen socket non-blocking: " << e.what();
        close(listen_fd_);
        return false;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, host.c_str(), &(addr.sin_addr)) <= 0) {
        ORE_LOG(ERROR) << "Invalid host address: " << host;
        close(listen_fd_);
        return false;
    }

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ORE_LOG(ERROR) << "Failed to bind socket to " << host << ":" << port << ": " << strerror(errno);
        close(listen_fd_);
        return false;
    }
    if (::listen(listen_fd_, BACKLOG) < 0) {
        ORE_LOG(ERROR) << "Failed to listen on socket: " << strerror(errno);
        close(listen_fd_);
        return false;
    }

    ORE_LOG(INFO) << "Server listening on " << host << ":" << port;
    return true;
}

#ifdef __linux__
bool Server::setup_epoll() {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        ORE_LOG(ERROR) << "Failed to create epoll instance: " << strerror(errno);
        return false;
    }
    epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        ORE_LOG(ERROR) << "Failed to add listen socket to epoll: " << strerror(errno);
        close(epoll_fd_);
        return false;
    }
    return true;
}
#elif __APPLE__
bool Server::setup_kqueue() {
    kqueue_fd_ = kqueue();
    if (kqueue_fd_ < 0) {
        ORE_LOG(ERROR) << "Failed to create kqueue instance: " << strerror(errno);
        return false;
    }
    struct kevent change_event;
    EV_SET(&change_event, listen_fd_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(kqueue_fd_, &change_event, 1, NULL, 0, NULL) < 0) {
        ORE_LOG(ERROR) << "Failed to add listen socket to kqueue: " << strerror(errno);
        close(kqueue_fd_);
        return false;
    }
    return true;
}
#endif

bool Server::setup_wakeup() {
    if (pipe(wakeup_pipe_) < 0) {
        ORE_LOG(ERROR) << "Failed to create wakeup pipe: " << strerror(errno);
        return false;
    }
    try {
        set_non_blocking(wakeup_pipe_[0]);
        set_non_blocking(wakeup_pipe_[1]);
    } catch (const std::runtime_error& e) {
        ORE_LOG(ERROR) << "Failed to set wakeup pipe non-blocking: " << e.what();
        return false;
    }

    // Register the read end as a persistent, level/edge-triggered source.
#ifdef __linux__
    epoll_event event;
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = wakeup_pipe_[0];
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_pipe_[0], &event) < 0) {
        ORE_LOG(ERROR) << "Failed to add wakeup pipe to epoll: " << strerror(errno);
        return false;
    }
#elif __APPLE__
    struct kevent change_event;
    EV_SET(&change_event, wakeup_pipe_[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (kevent(kqueue_fd_, &change_event, 1, NULL, 0, NULL) < 0) {
        ORE_LOG(ERROR) << "Failed to add wakeup pipe to kqueue: " << strerror(errno);
        return false;
    }
#endif
    return true;
}

bool Server::listen(const std::string& host, int port) {
    if (!setup_socket(host, port)) {
        return false;
    }
#ifdef __linux__
    if (!setup_epoll()) {
        close(listen_fd_);
        return false;
    }
#elif __APPLE__
    if (!setup_kqueue()) {
        close(listen_fd_);
        return false;
    }
#endif
    if (!setup_wakeup()) {
        return false;
    }
    running_ = true;
    return true;
}

// Async-signal-safe: only writes to an atomic and a pipe (both safe).
void Server::request_stop() {
    running_.store(false, std::memory_order_relaxed);
    notify_event_loop();
}

void Server::notify_event_loop() {
    if (wakeup_pipe_[1] < 0) return;
    const char byte = 1;
    // Best-effort: a full pipe already means "wake up", so ignore EAGAIN/EINTR.
    ssize_t n;
    do {
        n = write(wakeup_pipe_[1], &byte, 1);
    } while (n < 0 && errno == EINTR);
}

void Server::drain_wakeup() {
    char buf[256];
    while (read(wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
        // discard
    }
}

void Server::process_completions() {
    std::queue<CompletedResponse> ready;
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        std::swap(ready, completed_);
    }

    while (!ready.empty()) {
        CompletedResponse item = std::move(ready.front());
        ready.pop();

        // Verify the connection is still the live owner of this fd (guard
        // against close + fd reuse while the worker was running).
        auto it = connections_.find(item.fd);
        if (it == connections_.end() || it->second != item.conn || !item.conn->is_open()) {
            continue; // Connection went away; drop the response.
        }

        item.conn->set_response_content(item.response);
        rearm(item.fd, /*read=*/false); // Closes the connection on failure.
    }
}

bool Server::rearm(int fd, bool read) {
#ifdef __linux__
    epoll_event event;
    event.events = (read ? EPOLLIN : EPOLLOUT) | EPOLLET | EPOLLONESHOT;
    event.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
        ORE_LOG(ERROR) << "Failed to re-arm fd " << fd << ": " << strerror(errno);
        close_connection(fd);
        return false;
    }
#elif __APPLE__
    struct kevent change_event;
    EV_SET(&change_event, fd, read ? EVFILT_READ : EVFILT_WRITE,
           EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, NULL);
    if (kevent(kqueue_fd_, &change_event, 1, NULL, 0, NULL) < 0) {
        ORE_LOG(ERROR) << "Failed to re-arm fd " << fd << ": " << strerror(errno);
        close_connection(fd);
        return false;
    }
#endif
    return true;
}

void Server::run() {
#ifdef __linux__
    epoll_event events[MAX_EVENTS];
#elif __APPLE__
    struct kevent events[MAX_EVENTS];
#endif
    auto last_cleanup = std::chrono::steady_clock::now();

    while (running_.load(std::memory_order_relaxed)) {
#ifdef __linux__
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, 1000);
#elif __APPLE__
        struct timespec timeout{1, 0};
        int num_events = kevent(kqueue_fd_, NULL, 0, events, MAX_EVENTS, &timeout);
#endif
        if (num_events < 0) {
            if (errno == EINTR) continue;
#ifdef __linux__
            ORE_LOG(ERROR) << "epoll_wait failed: " << strerror(errno);
#elif __APPLE__
            ORE_LOG(ERROR) << "kevent failed: " << strerror(errno);
#endif
            running_.store(false, std::memory_order_relaxed);
            break;
        }

        for (int i = 0; i < num_events; ++i) {
#ifdef __linux__
            int fd = events[i].data.fd;
            uint32_t flags = events[i].events;

            if (fd == wakeup_pipe_[0]) {
                drain_wakeup();
                process_completions();
            } else if (fd == listen_fd_) {
                if (flags & EPOLLIN) handle_new_connection();
            } else {
                if ((flags & EPOLLERR) || (flags & EPOLLHUP)) {
                    close_connection(fd);
                    continue;
                }
                if (flags & EPOLLIN)  handle_client_data(fd);
                if (flags & EPOLLOUT) handle_write_ready(fd);
            }
#elif __APPLE__
            int fd = (int)events[i].ident;
            int16_t filter = events[i].filter;
            uint16_t flags = events[i].flags;

            if (fd == wakeup_pipe_[0]) {
                drain_wakeup();
                process_completions();
            } else if (fd == listen_fd_) {
                if (filter == EVFILT_READ) handle_new_connection();
            } else {
                if (filter == EVFILT_READ)  handle_client_data(fd);
                if (filter == EVFILT_WRITE) handle_write_ready(fd);
                if (flags & EV_EOF) {
                    // Only close once any pending readable data has been handled.
                    close_connection(fd);
                }
            }
#endif
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup).count() > 30) {
            cleanup_expired_connections();
            last_cleanup = now;
        }
    }

    // Tear down resources owned by the event-loop thread *on this thread*, so a
    // concurrent stop()/destructor on the owner thread never races us on the
    // connection map or the multiplexer fd. The owner is expected to join this
    // thread after calling request_stop().
    connections_.clear();
#ifdef __linux__
    if (epoll_fd_ >= 0) { close(epoll_fd_); epoll_fd_ = -1; }
#elif __APPLE__
    if (kqueue_fd_ >= 0) { close(kqueue_fd_); kqueue_fd_ = -1; }
#endif
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }

    ORE_LOG(INFO) << "Server main loop stopped.";
}

void Server::stop() {
    // Signal the loop and tear down the worker pool. The event-loop thread tears
    // down its own connections/fds in run(); the caller must have joined it (or
    // never started it) before this completes. We only touch state here that is
    // not concurrently used once the loop has exited and workers are joined.
    request_stop();
    if (thread_pool_) {
        thread_pool_->shutdown(); // Joins worker threads.
    }

    // Cover the "run() was never started" path; no-ops if run() already cleaned up.
    connections_.clear();
#ifdef __linux__
    if (epoll_fd_ >= 0) { close(epoll_fd_); epoll_fd_ = -1; }
#elif __APPLE__
    if (kqueue_fd_ >= 0) { close(kqueue_fd_); kqueue_fd_ = -1; }
#endif
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    // Safe to close now: all workers are joined, so no one can call notify_event_loop().
    if (wakeup_pipe_[0] >= 0) { close(wakeup_pipe_[0]); wakeup_pipe_[0] = -1; }
    if (wakeup_pipe_[1] >= 0) { close(wakeup_pipe_[1]); wakeup_pipe_[1] = -1; }
    ORE_LOG(INFO) << "Server fully stopped.";
}

void Server::handle_new_connection() {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd;

    while ((client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &client_len)) >= 0) {
        try {
            set_non_blocking(client_fd);
        } catch (const std::runtime_error& e) {
            ORE_LOG(ERROR) << "Failed to set client socket non-blocking: " << e.what();
            close(client_fd);
            continue;
        }

#ifdef SO_NOSIGPIPE
        // macOS/BSD: suppress SIGPIPE on writes to a closed peer at the socket
        // level (Linux uses MSG_NOSIGNAL per send() instead).
        int nosigpipe = 1;
        setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

#ifdef __linux__
        epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.fd = client_fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) {
            ORE_LOG(ERROR) << "Failed to add client socket to epoll: " << strerror(errno);
            close(client_fd);
            continue;
        }
#elif __APPLE__
        struct kevent change_event;
        EV_SET(&change_event, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, NULL);
        if (kevent(kqueue_fd_, &change_event, 1, NULL, 0, NULL) < 0) {
            ORE_LOG(ERROR) << "Failed to add client socket to kqueue: " << strerror(errno);
            close(client_fd);
            continue;
        }
#endif
        connections_[client_fd] = std::make_shared<Net::Connection>(client_fd);
    }

    if (client_fd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ORE_LOG(ERROR) << "Error accepting connection: " << strerror(errno);
    }
}

void Server::dispatch_next(int fd, const std::shared_ptr<Net::Connection>& conn) {
    if (conn->processing_) return; // A request is already in flight; wait for it.

    size_t consumed = 0;
    if (conn->parse_next(consumed)) {
        // Take an owning copy of the request so it can safely outlive the
        // socket buffer and be handed to a worker thread.
        auto request = std::make_shared<Http::HttpRequest>(std::move(conn->current_request_));
        request->make_owned(conn->read_buffer_.data(), consumed);
        conn->consume(consumed);
        conn->processing_ = true;

        thread_pool_->enqueue([this, fd, conn, request]() {
            Http::HttpResponse res;
            RouteHandler handler;
            std::unordered_map<std::string_view, std::string_view> path_params;

            if (router_->find_route(request->method(), request->path(), path_params, handler)) {
                request->path_params_ = std::move(path_params);
                try {
                    handler(*request, res);
                } catch (const std::exception& e) {
                    ORE_LOG(ERROR) << "Handler exception: " << e.what();
                    Json::JsonValue err;
                    err["error"] = "Server error";
                    res.status(Http::HttpStatus::INTERNAL_SERVER_ERROR).json(err);
                }
            } else {
                Json::JsonValue err;
                err["error"] = "Not Found";
                res.status(Http::HttpStatus::NOT_FOUND).json(err);
            }

            {
                std::lock_guard<std::mutex> lock(completed_mutex_);
                completed_.push(CompletedResponse{fd, conn, std::move(res)});
            }
            notify_event_loop();
        });
        return;
    }

    if (conn->parser_failed()) {
        close_connection(fd);
        return;
    }

    // Incomplete request: wait for more data.
    rearm(fd, /*read=*/true);
}

void Server::handle_client_data(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    std::shared_ptr<Net::Connection> conn = it->second;

    if (!conn->is_open()) {
        close_connection(fd);
        return;
    }

    ssize_t bytes_read = conn->read_data();
    if (bytes_read == 0) {
        close_connection(fd); // Peer closed the connection.
        return;
    }
    if (bytes_read == -1) {
        close_connection(fd); // Hard read error.
        return;
    }
    // bytes_read > 0 (got data) or kReadWouldBlock (-2, nothing new): in both
    // cases try to make progress on whatever is already buffered.
    dispatch_next(fd, conn);
}

void Server::handle_write_ready(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    std::shared_ptr<Net::Connection> conn = it->second;

    if (!conn->is_open()) {
        close_connection(fd);
        return;
    }

    ssize_t bytes_written = conn->write_data();
    if (bytes_written < 0) {
        close_connection(fd);
        return;
    }

    if (conn->has_data_to_write()) {
        rearm(fd, /*read=*/false); // More to send.
        return;
    }

    // Full response sent.
    if (!conn->keep_alive_) {
        close_connection(fd);
        return;
    }

    conn->clear_response_state();
    conn->processing_ = false;
    conn->update_activity();
    // Service the next pipelined request if present, otherwise wait for reads.
    dispatch_next(fd, conn);
}

void Server::close_connection(int fd) {
#ifdef __linux__
    if (epoll_fd_ >= 0) {
        epoll_event event;
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &event);
    }
#endif
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    std::shared_ptr<Net::Connection> conn = std::move(it->second);
    connections_.erase(it);
    // Close the socket now. If a worker still holds a shared_ptr, the object
    // stays alive but its fd is already closed (is_open() == false), so the
    // pending response will be dropped safely in process_completions().
    conn->close_connection();
}

void Server::cleanup_expired_connections() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> fds_to_close;
    for (const auto& pair : connections_) {
        if (pair.second->processing_) continue; // Don't reap work in flight.
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - pair.second->get_last_activity()).count() > 60) {
            fds_to_close.push_back(pair.first);
        }
    }
    for (int fd : fds_to_close) {
        close_connection(fd);
    }
}

} // namespace Server
} // namespace Oreshnek
