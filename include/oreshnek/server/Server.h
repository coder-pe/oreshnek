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
#include <queue>

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

    // Map of active connections, indexed by their socket FD.
    // Only the event-loop thread mutates this map or the Connection objects.
    // shared_ptr lets an in-flight worker keep a connection alive even if the
    // event loop closes and removes it, preventing use-after-free.
    std::unordered_map<int, std::shared_ptr<Net::Connection>> connections_;

    // A response produced by a worker thread, waiting for the event loop to
    // write it out. The fd is captured so the event loop can verify the
    // connection it points to has not been closed and the fd reused.
    struct CompletedResponse {
        int fd;
        std::shared_ptr<Net::Connection> conn;
        Http::HttpResponse response;
    };
    std::queue<CompletedResponse> completed_;
    std::mutex completed_mutex_; // Protects completed_

    // Self-pipe used by worker threads to wake the event loop when a response
    // is ready (and by the signal handler to break out of the wait).
    int wakeup_pipe_[2] = {-1, -1};

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

    // Async-signal-safe: asks the event loop to exit. Safe to call from a
    // signal handler (only sets an atomic flag and writes one byte to a pipe).
    void request_stop();

private:
    // Helper functions for socket and event system setup
    bool setup_socket(const std::string& host, int port);
#ifdef __linux__
    bool setup_epoll();
#elif __APPLE__
    bool setup_kqueue();
#endif
    void set_non_blocking(int fd); // Declared in original server.cpp, useful utility

    // Event handlers (all run on the event-loop thread only)
    void handle_new_connection();
    void handle_client_data(int fd);
    void handle_write_ready(int fd);
    void close_connection(int fd); // Helper to safely close and remove from map

    // Parse the next buffered request (if any) and hand it to a worker. At most
    // one request per connection is in flight at a time to preserve ordering.
    void dispatch_next(int fd, const std::shared_ptr<Net::Connection>& conn);

    // Re-arm a connection's fd in the event multiplexer for the given direction.
    // Returns false (and closes the connection) on failure. read=true arms for
    // read readiness, otherwise for write readiness.
    bool rearm(int fd, bool read);

    // Wakeup-pipe plumbing for handing worker results back to the event loop.
    bool setup_wakeup();
    void notify_event_loop();   // called by worker threads / signal handler path
    void drain_wakeup();        // consume pending wakeup bytes
    void process_completions(); // write out responses queued by workers

    // Connection cleanup
    void cleanup_expired_connections();
};

} // namespace Server
} // namespace Oreshnek

#endif // ORESHNEK_SERVER_SERVER_H
