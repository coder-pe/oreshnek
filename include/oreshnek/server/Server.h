// oreshnek/include/oreshnek/server/Server.h
#ifndef ORESHNEK_SERVER_SERVER_H
#define ORESHNEK_SERVER_SERVER_H

#include "oreshnek/server/Router.h"
#include "oreshnek/server/ThreadPool.h"
#include "oreshnek/net/Connection.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"

#include <string>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>

#ifdef __linux__
#include <sys/epoll.h> // For epoll structures on Linux
#elif __APPLE__
#include <sys/event.h> // For kqueue on macOS
#include <sys/select.h> // For select fallback
#endif

namespace Oreshnek {
namespace Server {

class Server {
private:
    int listen_fd_; // Listening socket file descriptor
#ifdef __linux__
    int epoll_fd_;  // Epoll instance file descriptor
#elif __APPLE__
    int kqueue_fd_; // Kqueue instance file descriptor
#endif
    std::atomic<bool> running_; // Flag to control server loop

    std::unique_ptr<Router> router_;
    std::unique_ptr<ThreadPool> thread_pool_;

    // Map of active connections, indexed by their socket FD
    std::unordered_map<int, std::unique_ptr<Net::Connection>> connections_;
    std::mutex connections_mutex_; // Protects access to connections_ map

    static constexpr int MAX_EVENTS = 1024;
    static constexpr int BACKLOG = 1024; // Listen backlog for new connections

public:
    Server(size_t worker_threads = std::thread::hardware_concurrency());
    ~Server();

    // Route registration methods
    void get(const std::string& path, RouteHandler handler) {
        router_->add_route(Http::HttpMethod::GET, path, std::move(handler));
    }
    void post(const std::string& path, RouteHandler handler) {
        router_->add_route(Http::HttpMethod::POST, path, std::move(handler));
    }
    void put(const std::string& path, RouteHandler handler) {
        router_->add_route(Http::HttpMethod::PUT, path, std::move(handler));
    }
    void del(const std::string& path, RouteHandler handler) {
        router_->add_route(Http::HttpMethod::DELETE, path, std::move(handler));
    }
    void patch(const std::string& path, RouteHandler handler) {
        router_->add_route(Http::HttpMethod::PATCH, path, std::move(handler));
    }

    // Server control
    bool listen(const std::string& host = "0.0.0.0", int port = 8080);
    void run();
    void stop();

private:
    // Helper functions for socket and event system setup
    bool setup_socket(const std::string& host, int port);
#ifdef __linux__
    bool setup_epoll();
#elif __APPLE__
    bool setup_kqueue();
#endif
    void set_non_blocking(int fd); // Declared in original server.cpp, useful utility

    // Event handlers
    void handle_new_connection();
    void handle_client_data(int fd);
    void handle_write_ready(int fd);
    void close_connection(int fd); // Helper to safely close and remove from map

    // Connection cleanup
    void cleanup_expired_connections();
};

} // namespace Server
} // namespace Oreshnek

#endif // ORESHNEK_SERVER_SERVER_H
