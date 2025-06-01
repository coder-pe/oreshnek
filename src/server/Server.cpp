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

// For sendfile (Linux specific)
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#include <sys/stat.h> // For stat (to get file size and check existence)

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

    if (::listen(listen_fd_, BACKLOG) < 0) { // Use '::' to explicitly call the global listen function
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
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT; 
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
    Net::Connection* conn_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            std::cerr << "Error: handle_client_data called for non-existent FD " << fd << std::endl;
            return;
        }
        conn_ptr = it->second.get(); // Get raw pointer, ownership remains in map
    }

    if (!conn_ptr->is_open()) {
        std::cerr << "Warning: Received data for already closed connection " << fd << std::endl;
        close_connection(fd); // Ensure it's fully closed and removed from map
        return;
    }

    ssize_t bytes_read = conn_ptr->read_data();
    if (bytes_read == 0) {
        // Client closed connection gracefully
        std::cerr << "Client on fd " << fd << " closed connection gracefully." << std::endl;
        close_connection(fd); 
        return;
    } else if (bytes_read < 0) {
        // Error reading (excluding EAGAIN/EWOULDBLOCK which return 0)
        close_connection(fd);
        return;
    }

    // Attempt to parse requests from the connection's read buffer
    bool request_parsed_this_call = false;
    while (conn_ptr->process_read_buffer()) {
        // Full request parsed! Enqueue it to the thread pool for processing
        Http::HttpRequest request_copy = conn_ptr->current_request_; // Create a copy of the request
        request_parsed_this_call = true;

        thread_pool_->enqueue([this, request_copy, fd, conn_ptr]() mutable {
            Http::HttpResponse res; // Create a fresh response for this request

            RouteHandler handler;
            std::unordered_map<std::string_view, std::string_view> path_params;

            if (router_->find_route(request_copy.method(), request_copy.path(), path_params, handler)) {
                request_copy.path_params_ = std::move(path_params); // Assign found path parameters
                try {
                    handler(request_copy, res); // Execute the route handler
                } catch (const std::exception& e) {
                    std::cerr << "Handler exception for " << Http::http_method_to_string(request_copy.method()) << " " << request_copy.path() << ": " << e.what() << std::endl;
                    res.status(Http::HttpStatus::INTERNAL_SERVER_ERROR)
                       .json(Json::JsonValue::object()["error"] = Json::JsonValue("Server error"));
                }
            } else {
                std::cerr << "No route found for " << Http::http_method_to_string(request_copy.method()) << " " << request_copy.path() << std::endl;
                res.status(Http::HttpStatus::NOT_FOUND)
                   .json(Json::JsonValue::object()["error"] = Json::JsonValue("Not Found"));
            }

            // After handler, prepare response to be written
            // Set the response content on the connection
            conn_ptr->set_response_content(res);

            conn_ptr->current_request_ = Http::HttpRequest(); // Clear request for next one
            conn_ptr->http_parser_.reset(); // Reset parser for next request on same connection

            // Re-arm connection for writing and subsequent reads
            epoll_event event;
            event.events = EPOLLOUT | EPOLLET | EPOLLONESHOT; // Request write event

            event.data.fd = fd;
            
            // Protect epoll_ctl with connections_mutex_
            {
                std::lock_guard<std::mutex> map_lock(this->connections_mutex_);
                if (epoll_ctl(this->epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
                    std::cerr << "Failed to modify epoll for fd " << fd << " (from thread pool): " << strerror(errno) << std::endl;
                    this->close_connection(fd);
                }
            }
        }); // End of enqueue lambda
    } 

    // If no full request was parsed in this call, re-arm for EPOLLIN to read more data.
    // This handles cases where only partial data was received.
    if (!request_parsed_this_call && conn_ptr->is_open()) {
        epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.fd = fd;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
                std::cerr << "Failed to modify epoll for read on fd " << fd << " after partial read: " << strerror(errno) << std::endl;
                close_connection(fd);
            }
        }
    }
}


void Server::handle_write_ready(int fd) {
    Net::Connection* conn_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            std::cerr << "Error: handle_write_ready called for non-existent FD " << fd << std::endl;
            return;
        }
        conn_ptr = it->second.get(); // Get raw pointer, ownership remains in map
    }

    if (!conn_ptr->is_open()) {
        std::cerr << "Warning: Received write event for already closed connection " << fd << std::endl;
        close_connection(fd); // Ensure it's fully closed and removed from map
        return;
    }

    // Attempt to write data
    ssize_t bytes_written = conn_ptr->write_data();

    if (bytes_written < 0) {
        close_connection(fd);
        return;
    }

    // Check if all data for the current response has been sent
    if (!conn_ptr->has_data_to_write()) {
        // All data sent. Re-arm for reading or close if not keep-alive.
        if (conn_ptr->keep_alive_) { 
            epoll_event event;
            event.events = EPOLLIN | EPOLLET | EPOLLONESHOT; // Read more data for next request
            event.data.fd = fd;
            conn_ptr->reset(); // Reset the connection for the next request

            { // Protect epoll_ctl with connections_mutex_
                std::lock_guard<std::mutex> lock(connections_mutex_);
                if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
                    std::cerr << "Failed to modify epoll for read on fd " << fd << ": " << strerror(errno) << std::endl;
                    close_connection(fd);
                }
            }
        } else {
            // Not keep-alive, close connection
            close_connection(fd);
        }
    } else {
        // Still data to send, re-arm for EPOLLOUT
        epoll_event event;
        event.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
        event.data.fd = fd;
        { // Protect epoll_ctl with connections_mutex_
            std::lock_guard<std::mutex> lock(connections_mutex_);
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
                std::cerr << "Failed to modify epoll for EPOLLOUT on fd " << fd << ": " << strerror(errno) << std::endl;
                close_connection(fd);
            }
        }
    }
}

void Server::close_connection(int fd) {
    std::unique_ptr<Net::Connection> conn_to_close;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            conn_to_close = std::move(it->second); // Take ownership and remove from map
            connections_.erase(it);
        }
    }

    if (conn_to_close) {
        conn_to_close->close_connection(); // Ensure socket is closed
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
