#include "oreshnek.h"
#include <signal.h>
#include <sys/types.h>

namespace MiniRest {

// ==================== Server Implementation ====================

void Server::set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        throw std::runtime_error("fcntl F_GETFL failed");
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl F_SETFL failed");
    }
}

bool Server::setup_socket(const std::string& host, int port) {
    // Create socket
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Enable address reuse
    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return false;
    }
    
    // Enable keepalive
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set SO_KEEPALIVE: " << strerror(errno) << std::endl;
    }
    
    // Set non-blocking
    set_non_blocking(listen_fd_);
    
    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (host == "0.0.0.0" || host.empty()) {
        server_addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
            std::cerr << "Invalid host address: " << host << std::endl;
            close(listen_fd_);
            return false;
        }
    }
    
    if (bind(listen_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind socket: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return false;
    }
    
    // Start listening
    if (::listen(listen_fd_, BACKLOG) < 0) {
        std::cerr << "Failed to listen on socket: " << strerror(errno) << std::endl;
        close(listen_fd_);
        return false;
    }
    
    return true;
}

bool Server::setup_epoll() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        std::cerr << "Failed to create epoll: " << strerror(errno) << std::endl;
        return false;
    }
    
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET; // Edge-triggered for better performance
    event.data.fd = listen_fd_;
    
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        std::cerr << "Failed to add listen socket to epoll: " << strerror(errno) << std::endl;
        close(epoll_fd_);
        return false;
    }
    
    return true;
}

void Server::handle_new_connection() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    while (true) {
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // No more connections to accept
            }
            std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            break;
        }
        
        // Set client socket non-blocking
        set_non_blocking(client_fd);
        
        // Add to epoll
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.fd = client_fd;
        
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) < 0) {
            std::cerr << "Failed to add client to epoll: " << strerror(errno) << std::endl;
            close(client_fd);
            continue;
        }
        
        // Create connection object
        auto conn = std::make_unique<Connection>(client_fd);
        
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[client_fd] = std::move(conn);
        }
    }
}

void Server::handle_client_data(int client_fd) {
    std::unique_ptr<Connection> conn;
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) {
            return; // Connection not found
        }
        conn = std::move(it->second);
        connections_.erase(it);
    }
    
    if (!conn || !conn->is_alive()) {
        return;
    }
    
    HttpRequest request;
    if (conn->read_request(request)) {
        // Process request in thread pool
        thread_pool_->enqueue([this, conn = std::move(conn), request = std::move(request)]() mutable {
            process_request(std::move(conn), std::move(request));
        });
    } else {
        // Re-add to epoll if connection is still alive and we need more data
        if (conn->is_alive()) {
            struct epoll_event event;
            event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
            event.data.fd = client_fd;
            
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client_fd, &event) == 0) {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_[client_fd] = std::move(conn);
            }
        }
    }
}

void Server::process_request(std::unique_ptr<Connection> conn, HttpRequest request) {
    HttpResponse response;
    
    try {
        // Route the request
        if (!router_->route(request, response)) {
            // Router already set 404 or 405 response
        }
    } catch (const std::exception& e) {
        response.status(500);
        JsonValue error_json;
        error_json["error"] = JsonValue("Internal Server Error");
        error_json["message"] = JsonValue(e.what());
        response.json(error_json);
    }
    
    // Send response
    bool sent = conn->write_response(response);
    
    // Handle connection persistence
    std::string connection_header = request.header("Connection");
    std::transform(connection_header.begin(), connection_header.end(), 
                   connection_header.begin(), ::tolower);
    
    bool keep_alive = (connection_header == "keep-alive") || 
                     (request.is_http2() && connection_header != "close");
    
    if (sent && keep_alive && conn->is_alive() && !conn->is_expired()) {
        // Re-add connection to epoll for keep-alive
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        event.data.fd = conn->fd();
        
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn->fd(), &event) == 0) {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[conn->fd()] = std::move(conn);
        }
    } else {
        // Close connection
        conn->close_connection();
    }
}

void Server::cleanup_expired_connections() {
    std::vector<int> expired_fds;
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            if (!it->second->is_alive() || it->second->is_expired()) {
                expired_fds.push_back(it->first);
                it->second->close_connection();
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Remove from epoll
    for (int fd : expired_fds) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    }
}

bool Server::listen(const std::string& host, int port) {
    if (!setup_socket(host, port)) {
        return false;
    }
    
    if (!setup_epoll()) {
        close(listen_fd_);
        return false;
    }
    
    std::cout << "Server listening on " << host << ":" << port << std::endl;
    return true;
}

void Server::run() {
    if (listen_fd_ < 0 || epoll_fd_ < 0) {
        std::cerr << "Server not properly initialized" << std::endl;
        return;
    }
    
    running_ = true;
    
    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    
    struct epoll_event events[MAX_EVENTS];
    auto last_cleanup = std::chrono::steady_clock::now();
    
    std::cout << "Server started and running..." << std::endl;
    
    while (running_) {
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, 1000); // 1 second timeout
        
        if (num_events < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal
            }
            std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
            break;
        }
        
        // Process events
        for (int i = 0; i < num_events; ++i) {
            int fd = events[i].data.fd;
            
            if (fd == listen_fd_) {
                // New connection
                handle_new_connection();
            } else {
                // Client data
                handle_client_data(fd);
            }
        }
        
        // Periodic cleanup of expired connections
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup).count() > 30) {
            cleanup_expired_connections();
            last_cleanup = now;
        }
    }
    
    std::cout << "Server shutting down..." << std::endl;
}

void Server::stop() {
    running_ = false;
    
    // Close all connections
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& pair : connections_) {
            pair.second->close_connection();
        }
        connections_.clear();
    }
    
    // Close server sockets
    if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
    
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    
    // Shutdown thread pool
    if (thread_pool_) {
        thread_pool_->shutdown();
    }
}

} // namespace MiniRest