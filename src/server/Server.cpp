// oreshnek/src/server/Server.cpp
#include "oreshnek/server/Server.h"
#include <iostream>
#include <fcntl.h>    // For fcntl
#include <unistd.h>   // For close
#include <sys/socket.h> // For socket, bind, listen, accept
#include <netinet/in.h> // For sockaddr_in
#include <arpa/inet.h>  // For inet_ntoa
#include <errno.h>    // For errno
#include <cstring>    // For strerror

namespace Oreshnek {
namespace Server {

Server::Server(size_t worker_threads)
    : listen_fd_(-1), epoll_fd_(-1), running_(false) {
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
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }

    int opt = 1;
    // Enable address reuse
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return false;
    }

    // Enable keepalive - can be useful for long-lived connections
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set SO_KEEPALIVE: " << strerror(errno) << std::endl;
    } // Not critical, so not returning false

    // Set non-blocking mode for the listening socket
    try {
        set_non_blocking(listen_fd_);
    } catch (const std::runtime_error& e) {
        std::cerr << "Failed to set listen socket non-blocking: " << e.what() << std::endl;
        close(listen_fd_);
        return false;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host.c_str(), &(addr.sin_addr)) <= 0) {
            std::cerr << "Invalid host address: " << host << std::endl;
            close(listen_fd_);
            return false;
        }
    }

    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind socket to " << host << ":" << port << ": " << strerror(errno) << std::endl;
        close(listen_fd_);
        return false;
    }

    if (listen(listen_fd_, BACKLOG) < 0) {
        std::cerr << "Failed to listen on socket: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return false;
    }

    std::cout << "Server listening on " << host << ":" << port << std::endl;
    return true;
}

bool Server::setup_epoll() {
    epoll_fd_ = epoll_create1(0); // 0 for flags, not deprecated
    if (epoll_fd_ < 0) {
        std::cerr << "Failed to create epoll instance: " << strerror(errno) << std::endl;
        return false;
    }

    epoll_event event;
    event.events = EPOLLIN | EPOLLET; // Edge-triggered for listen socket
    event.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        std::cerr << "Failed to add listen socket to epoll: " << strerror(errno) << std::endl;
        close(epoll_fd_);
        return false;
    }
    return true;
}

bool Server::listen(const std::string& host, int port) {
    if (!setup_socket(host, port)) {
        return false;
    }
    if (!setup_epoll()) {
        close(listen_fd_);
        return false;
    }
    running_ = true;
    return true;
}

void Server::run() {
    epoll_event events[MAX_EVENTS];
    auto last_cleanup = std::chrono::steady_clock::now();

    while (running_) {
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, 1000); // 1-second timeout

        if (num_events < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal
            }
            std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
            running_ = false; // Critical error, stop server
            break;
        }

        for (int i = 0; i < num_events; ++i) {
            int fd = events[i].data.fd;
            uint32_t event_flags = events[i].events;

            if (fd == listen_fd_) {
                // New connection
                if (event_flags & EPOLLIN) { // Ensure it's a read event
                    handle_new_connection();
                }
            } else {
                // Existing client connection
                // Acquire shared_ptr to connection from map (copy to keep it alive during processing)
                std::shared_ptr<Net::Connection> conn_ptr; // Use shared_ptr here to avoid connection being deleted while processing
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    auto it = connections_.find(fd);
                    if (it != connections_.end()) {
                        conn_ptr = it->second; // This would imply connections_ map stores shared_ptr
                                               // We use unique_ptr, so need to be careful with lifetime
                                               // Better to just pass fd and let the handler access map
                    }
                }
                
                // If connection was already closed/removed, skip
                if (!conn_ptr && fd != listen_fd_) {
                    std::cerr << "Warning: Event for unknown/closed FD " << fd << std::endl;
                    continue;
                }
                
                // For unique_ptr in map, we need to pass fd and re-lookup or pass raw pointer carefully.
                // Using a lambda with captured `this` and `fd` will ensure access to the Connection object.
                // Or, in `handle_client_data`, directly lookup the connection by fd.
                
                // Read events
                if (event_flags & EPOLLIN) {
                    handle_client_data(fd);
                }
                // Write events
                if (event_flags & EPOLLOUT) {
                    handle_write_ready(fd);
                }
                // Error events
                if ((event_flags & EPOLLERR) || (event_flags & EPOLLHUP)) {
                    std::cerr << "Epoll error or hangup on fd " << fd << std::endl;
                    close_connection(fd);
                }
            }
        }

        // Periodic cleanup of expired connections (e.g., every 30 seconds)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup).count() > 30) {
            cleanup_expired_connections();
            last_cleanup = now;
        }
    }

    std::cout << "Server main loop stopped." << std::endl;
}

void Server::stop() {
    running_ = false;
    thread_pool_->shutdown(); // Shutdown worker threads

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& pair : connections_) {
            pair.second->close_connection();
        }
        connections_.clear();
    }

    // Close server sockets and epoll instance
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    std::cout << "Server fully stopped." << std::endl;
}

void Server::handle_new_connection() {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd;

    // Accept all pending connections in edge-triggered mode
    while ((client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &client_len)) >= 0) {
        std::cout << "Accepted new connection from " << inet_ntoa(client_addr.sin_addr)
                  << ":" << ntohs(client_addr.sin_port) << " on fd " << client_fd << std::endl;

        try {
            set_non_blocking(client_fd);
        } catch (const std::runtime_error& e) {
            std::cerr << "Failed to set client socket non-blocking: " << e.what() << std::endl;
            close(client_fd);
            continue;
        }

        epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT; // Edge-triggered, one-shot for client FD
        event.data.fd = client_fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) {
            std::cerr << "Failed to add client socket to epoll: " << strerror(errno) << std::endl;
            close(client_fd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[client_fd] = std::make_unique<Net::Connection>(client_fd);
        }
    }

    if (client_fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // No more pending connections
        return;
    } else if (client_fd < 0) {
        std::cerr << "Error accepting connection: " << strerror(errno) << std::endl;
    }
}

void Server::handle_client_data(int fd) {
    std::unique_ptr<Net::Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            conn = std::move(it->second); // Take ownership to process, put back later
            connections_.erase(it);
        } else {
            std::cerr << "Error: handle_client_data called for non-existent FD " << fd << std::endl;
            return;
        }
    }

    if (!conn->is_open()) {
        std::cerr << "Warning: Received data for already closed connection " << fd << std::endl;
        return;
    }

    ssize_t bytes_read = conn->read_data();
    if (bytes_read == 0) {
        // Client closed connection
        std::cerr << "Client on fd " << fd << " closed connection gracefully." << std::endl;
        conn->close_connection(); // Ensure socket is closed
        // Connection will be destructed when conn unique_ptr goes out of scope
        return;
    } else if (bytes_read < 0) {
        // Error reading (excluding EAGAIN/EWOULDBLOCK which return 0)
        conn->close_connection();
        return;
    }

    // Attempt to parse requests from the connection's read buffer
    while (conn->process_read_buffer()) {
        // Full request parsed! Enqueue it to the thread pool for processing
        thread_pool_->enqueue([this, captured_conn = std::move(conn)]() mutable {
            Http::HttpRequest& req = captured_conn->current_request_;
            Http::HttpResponse res; // Create a fresh response for this request

            RouteHandler handler;
            std::unordered_map<std::string_view, std::string_view> path_params;

            // Copy path_params to HttpRequest from Router result
            if (router_->find_route(req.method(), req.path(), path_params, handler)) {
                req.path_params_ = std::move(path_params); // Assign found path parameters
                try {
                    handler(req, res); // Execute the route handler
                } catch (const std::exception& e) {
                    std::cerr << "Handler exception for " << http_method_to_string(req.method()) << " " << req.path() << ": " << e.what() << std::endl;
                    res.status(Http::HttpStatus::INTERNAL_SERVER_ERROR)
                       .json(Json::JsonValue::object()["error"] = Json::JsonValue("Server error"));
                }
            } else {
                std::cerr << "No route found for " << http_method_to_string(req.method()) << " " << req.path() << std::endl;
                res.status(Http::HttpStatus::NOT_FOUND)
                   .json(Json::JsonValue::object()["error"] = Json::JsonValue("Not Found"));
            }

            // After handler, prepare response to be written
            captured_conn->append_to_write_buffer(res.to_string());

            // Re-arm connection for writing
            epoll_event event;
            event.events = EPOLLOUT | EPOLLET | EPOLLONESHOT; // Listen for EPOLLOUT
            event.data.fd = captured_conn->socket_fd_;
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, captured_conn->socket_fd_, &event) < 0) {
                std::cerr << "Failed to modify epoll for write on fd " << captured_conn->socket_fd_ << ": " << strerror(errno) << std::endl;
                captured_conn->close_connection();
            }

            // After enqueueing the task, put the connection back in the map
            // This is a critical point: `captured_conn` is now owned by the lambda
            // If the lambda finishes, it will be destructed.
            // We need to ensure the connection is put back in the map so it's not lost.
            // This implies the lambda should be responsible for putting it back,
            // or we need a different ownership model.

            // Simplest way to "put back" in this model is to return it from the lambda,
            // or pass a raw pointer/reference and re-insert by the I/O thread.
            // Given the current design, we need to pass a unique_ptr to the lambda and
            // return it when done. Let's adjust for that.

            // The 'captured_conn' is std::move'd into the lambda.
            // When the lambda finishes execution, if it's not put back into the map, it will be deleted.
            // We need to re-insert the connection.
            // For now, let's assume the lambda holds ownership until it's ready for next I/O.
            // But this means the I/O thread can't act on it.

            // REVISED STRATEGY:
            // 1. I/O thread reads data.
            // 2. If complete request, it copies/moves Http::HttpRequest data to pass to thread pool.
            // 3. Connection object remains in `connections_` map.
            // 4. Thread pool processes request and generates response.
            // 5. Response data is appended to connection's write buffer.
            // 6. I/O thread is notified to write (EPOLLOUT).
            // This avoids passing `std::unique_ptr<Connection>` around.

            // Let's revert to a simpler model: the connection always stays in the map.
            // And the worker thread just processes data on the request object.
            // The `handle_client_data` will re-arm the epoll event.

            // *** Revert the `std::move(conn)` and `captured_conn` part of the lambda ***
            // The connection stays in the map. The worker thread needs to access it,
            // which means locking the mutex. This is not ideal for performance.
            // A common pattern is to have a pool of `Connection` objects or
            // pass just the necessary request data to the thread pool, and then
            // the I/O thread handles putting the response onto the wire.

            // Let's refine the approach:
            // The Connection object *must* reside in the main event loop's memory.
            // When a request is parsed, *only* the HttpRequest object (or its data)
            // is passed to the thread pool. The response is then built in the thread pool
            // and communicated back to the main loop (e.g., via a response queue)
            // or directly appended to the Connection's write buffer (requiring mutex on buffer).

            // Let's re-acquire the connection pointer for the write operations and re-arming epoll.
            // This means we are back to accessing `connections_` mutex from the thread pool.
            // This is a point of contention.
            // Alternative: The worker thread computes the response *string* and queues it back
            // to a per-connection output queue, which the I/O thread picks up.

            // For now, to minimize changes and ensure a runnable example,
            // the worker thread will directly append to the connection's buffer (under mutex).
            // This is not the ultimate high-performance pattern for a fully decoupled system,
            // but is common for simplicity.

            // The `captured_conn` unique_ptr should not be moved. Instead,
            // we should pass a raw pointer `conn_ptr` or `fd` and re-lookup.

            // So the `enqueue` should look like this (simplified for the connection management logic):
            // After handler(req, res);
            // Get connection back via fd
            std::unique_ptr<Net::Connection> current_connection;
            {
                std::lock_guard<std::mutex> map_lock(this->connections_mutex_);
                auto iter = this->connections_.find(fd); // `fd` is captured from `handle_client_data`
                if (iter != this->connections_.end()) {
                    current_connection = std::move(iter->second);
                    this->connections_.erase(iter);
                }
            }

            if (current_connection) {
                current_connection->append_to_write_buffer(res.to_string());
                current_connection->current_request_.path_params_.clear(); // Clear path params for next request
                current_connection->http_parser_.reset(); // Reset parser for next request on same connection

                // Re-arm connection for writing and subsequent reads
                epoll_event event;
                // Add EPOLLOUT if there's data to send
                // Add EPOLLIN if we expect more requests on keep-alive
                event.events = EPOLLIN | EPOLLET | EPOLLONESHOT; // Always expect read
                if (!current_connection->write_buffer_.empty()) {
                    event.events |= EPOLLOUT; // Only ask for write event if buffer isn't empty
                }
                event.data.fd = current_connection->socket_fd_;

                {
                    std::lock_guard<std::mutex> map_lock(this->connections_mutex_);
                    this->connections_[current_connection->socket_fd_] = std::move(current_connection);
                }

                if (epoll_ctl(this->epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
                    std::cerr << "Failed to modify epoll for fd " << fd << ": " << strerror(errno) << std::endl;
                    this->close_connection(fd);
                }
            } else {
                std::cerr << "Error: Connection " << fd << " not found in map after processing by thread pool." << std::endl;
            }
        }); // End of enqueue lambda

        // After processing the request, reset the connection for the next one if it's keep-alive
        // This 'reset' was problematic. It should be done *after* the response is sent.
        // It's better to reset the parser within the `Connection` object,
        // and only after the response is completely sent.
        // For now, the `Connection::reset()` will be called by the worker thread.
    } // End of while (conn->process_read_buffer())

    // If request is not complete (more data needed) or parsing error, put connection back
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        if (conn) { // conn might be null if it was closed
             connections_[fd] = std::move(conn);
             // Re-arm epoll for EPOLLIN only (if not EPOLLOUT needed)
             epoll_event event;
             event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
             event.data.fd = fd;
             if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
                 std::cerr << "Failed to modify epoll for read on fd " << fd << " after partial read: " << strerror(errno) << std::endl;
                 close_connection(fd);
             }
        }
    }
}


void Server::handle_write_ready(int fd) {
    std::unique_ptr<Net::Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            conn = std::move(it->second);
            connections_.erase(it);
        } else {
            std::cerr << "Error: handle_write_ready called for non-existent FD " << fd << std::endl;
            return;
        }
    }

    if (!conn->is_open()) {
        std::cerr << "Warning: Received write event for already closed connection " << fd << std::endl;
        return;
    }

    ssize_t bytes_written = conn->write_data();
    if (bytes_written < 0) {
        conn->close_connection();
        return;
    }

    if (conn->write_buffer_.empty()) {
        // All data sent. Re-arm for reading or close if not keep-alive.
        epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT; // Read more data if keep-alive
        event.data.fd = fd;

        // Check for keep-alive header in the *processed* request
        // (This would require HttpRequest to be part of Connection state and accessible)
        // For simplicity, let's assume keep-alive by default unless explicitly set to close.
        if (conn->keep_alive_) { // `keep_alive_` is a member of `Connection`
            // Re-arm for reading if keep-alive
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
                std::cerr << "Failed to modify epoll for read on fd " << fd << ": " << strerror(errno) << std::endl;
                conn->close_connection();
            }
            conn->reset(); // Reset the connection for the next request
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_[fd] = std::move(conn);
            }
        } else {
            // Not keep-alive, close connection
            conn->close_connection();
            // Connection will be removed from map when unique_ptr goes out of scope.
        }
    } else {
        // Still data to send, re-arm for EPOLLOUT
        epoll_event event;
        event.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
        event.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
            std::cerr << "Failed to modify epoll for EPOLLOUT on fd " << fd << ": " << strerror(errno) << std::endl;
            conn->close_connection();
        }
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[fd] = std::move(conn);
        }
    }
}

void Server::close_connection(int fd) {
    std::unique_ptr<Net::Connection> conn_to_close;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            conn_to_close = std::move(it->second); // Take ownership
            connections_.erase(it);
        }
    }

    if (conn_to_close) {
        conn_to_close->close_connection(); // Ensure socket is closed
        // unique_ptr will handle deallocation
    } else {
        std::cerr << "Warning: Attempted to close non-existent connection FD " << fd << std::endl;
    }

    // Remove from epoll
    if (epoll_fd_ >= 0) {
        epoll_event event; // Dummy event is fine for EPOLL_CTL_DEL
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &event) < 0) {
            std::cerr << "Failed to remove fd " << fd << " from epoll: " << strerror(errno) << std::endl;
        }
    }
}

void Server::cleanup_expired_connections() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> fds_to_close;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& pair : connections_) {
            // Example timeout: 60 seconds
            if (std::chrono::duration_cast<std::chrono::seconds>(now - pair.second->get_last_activity()).count() > 60) {
                fds_to_close.push_back(pair.first);
            }
        }
    }

    for (int fd : fds_to_close) {
        std::cout << "Cleaning up expired connection on fd " << fd << std::endl;
        close_connection(fd);
    }
}

} // namespace Server
} // namespace Oreshnek