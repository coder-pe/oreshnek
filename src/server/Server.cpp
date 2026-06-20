// oreshnek/src/server/Server.cpp
#include "oreshnek/server/Server.h"
#include "oreshnek/net/TlsContext.h"
#include "oreshnek/utils/Logger.h"
#include <iostream>
#include <fcntl.h>    // For fcntl
#include <unistd.h>   // For close, pipe, read, write
#include <sys/socket.h> // For socket, bind, listen, accept
#include <netinet/in.h> // For sockaddr_in
#include <arpa/inet.h>  // For inet_ntoa
#include <errno.h>    // For errno
#include <cstring>    // For strerror
#include <cstdio>     // For snprintf (HTTP date / ETag formatting)
#include <ctime>      // For gmtime_r / strptime / timegm
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
#include <string>

// Avoid SIGPIPE when writing a timeout response to a half-closed peer. No-op on
// platforms without MSG_NOSIGNAL (macOS relies on SO_NOSIGPIPE / SIG_IGN).
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace Oreshnek {
namespace Server {

namespace {
// Format a time_t as an RFC 1123 HTTP date (locale-independent).
std::string http_date(time_t t) {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  kDays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// Parse an RFC 1123 HTTP date; (time_t)-1 on failure.
time_t parse_http_date(const std::string& s) {
    struct tm tm{};
    if (strptime(s.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm) != nullptr) {
        return timegm(&tm);
    }
    return static_cast<time_t>(-1);
}

// Strong validator from size + mtime: "<size>-<mtime>".
std::string make_etag(const struct stat& st) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"%llx-%llx\"",
                  static_cast<unsigned long long>(st.st_size),
                  static_cast<unsigned long long>(st.st_mtime));
    return buf;
}

// Applies request-driven semantics to a freshly produced response:
//  * HEAD requests: suppress the body (headers only).
//  * File responses: advertise Accept-Ranges, set cache validators (ETag /
//    Last-Modified) and answer a matching conditional GET with 304; and, if the
//    request carries a single byte Range, switch to 206 Partial Content (or 416).
// Only a single range is supported; multi-range requests fall back to 200.
void apply_http_semantics(const Http::HttpRequest& req, Http::HttpResponse& res) {
    if (req.method() == Http::HttpMethod::HEAD) {
        res.set_head_only(true);
    }
    if (!res.is_file()) return;

    struct stat st;
    if (::stat(res.file_path().c_str(), &st) != 0) return; // handler already validated existence
    const off_t size = st.st_size;
    res.header("Accept-Ranges", "bytes");

    // Cache validators so browsers/proxies can revalidate cheaply instead of
    // re-downloading the body on every refresh.
    const std::string etag = make_etag(st);
    res.header("ETag", etag);
    res.header("Last-Modified", http_date(st.st_mtime));

    // Conditional GET -> 304 Not Modified (no body). If-None-Match takes
    // precedence over If-Modified-Since (RFC 7232).
    const bool safe_method = req.method() == Http::HttpMethod::GET ||
                             req.method() == Http::HttpMethod::HEAD;
    bool not_modified = false;
    if (auto inm = req.header("If-None-Match")) {
        not_modified = (*inm == "*") || (inm->find(etag) != std::string_view::npos);
    } else if (auto ims = req.header("If-Modified-Since")) {
        const time_t since = parse_http_date(std::string(*ims));
        not_modified = (since != static_cast<time_t>(-1)) && (st.st_mtime <= since);
    }
    if (safe_method && not_modified) {
        res.status(Http::HttpStatus::NOT_MODIFIED);
        res.header("Content-Length", "0");
        res.set_head_only(true);   // 304 carries no body
        res.set_file_range(0, 0);  // do not stream the file
        return;
    }

    auto range_hdr = req.header("Range");
    if (!range_hdr) {
        res.set_file_range(0, size);
        return;
    }

    // Only "bytes=START-END" single ranges are supported.
    std::string spec(*range_hdr);
    const std::string prefix = "bytes=";
    if (spec.rfind(prefix, 0) != 0 || spec.find(',') != std::string::npos) {
        res.set_file_range(0, size);
        return;
    }
    std::string r = spec.substr(prefix.size());
    auto dash = r.find('-');
    if (dash == std::string::npos) {
        res.set_file_range(0, size);
        return;
    }

    const std::string s_start = r.substr(0, dash);
    const std::string s_end = r.substr(dash + 1);
    off_t start = 0, end = size - 1;
    bool ok = (size > 0);
    try {
        if (!s_start.empty()) {
            start = std::stoll(s_start);
            if (!s_end.empty()) end = std::stoll(s_end);
        } else if (!s_end.empty()) {
            off_t n = std::stoll(s_end); // suffix: final N bytes
            if (n <= 0) ok = false;
            else start = (n >= size) ? 0 : size - n;
        } else {
            ok = false; // "bytes=-"
        }
    } catch (const std::exception&) {
        ok = false;
    }
    if (ok) {
        if (end >= size) end = size - 1;
        if (start < 0 || start > end) ok = false;
    }

    if (!ok) {
        res.status(Http::HttpStatus::RANGE_NOT_SATISFIABLE);
        res.header("Content-Range", "bytes */" + std::to_string(size));
        res.header("Content-Length", "0");
        res.set_head_only(true);
        res.set_file_range(0, 0);
        return;
    }

    const off_t length = end - start + 1;
    res.status(Http::HttpStatus::PARTIAL_CONTENT);
    res.header("Content-Range",
               "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size));
    res.header("Content-Length", std::to_string(length));
    res.set_file_range(start, length);
}
}  // namespace

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

void Server::enable_tls(const std::string& cert_file, const std::string& key_file,
                        const std::string& min_version) {
    // Constructed eagerly so a bad certificate/key fails fast at startup.
    tls_ctx_ = std::make_unique<Net::TlsContext>(cert_file, key_file, min_version);
}

void Server::enable_rate_limit(double requests_per_second, double burst) {
    rate_limiter_ = std::make_unique<TokenBucketLimiter>(requests_per_second, burst);
    ORE_LOG(INFO) << "Rate limiting enabled: " << requests_per_second
                  << " req/s per IP (burst " << burst << ")";
}

void Server::enable_metrics(const std::string& path) {
    router_->add_route(Http::HttpMethod::GET, path,
                       [this](const Http::HttpRequest&, Http::HttpResponse& res) {
                           res.status(Http::HttpStatus::OK).body(metrics_.render());
                           res.header("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
                       });
    ORE_LOG(INFO) << "Metrics exposed at GET " << path;
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

// Async-signal-safe: only writes to atomics and a pipe (both safe). Requests a
// graceful drain; the event loop decides when to actually exit (run() flips
// running_ once in-flight work is drained or the grace period expires).
void Server::request_stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
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

        // The worker finished: leave the handler-timeout window, enter the write
        // phase (now governed by write_timeout).
        item.conn->worker_in_flight_ = false;
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
    draining_ = false;
    std::chrono::steady_clock::time_point drain_deadline;

    while (running_.load(std::memory_order_relaxed)) {
        // While draining we poll more frequently so the grace deadline and the
        // "all connections drained" condition are observed promptly.
        const int wait_ms = draining_ ? 100 : 1000;
#ifdef __linux__
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, wait_ms);
#elif __APPLE__
        struct timespec timeout{wait_ms / 1000, (wait_ms % 1000) * 1000000};
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

        // Transition into graceful drain on the first observed stop request.
        if (!draining_ && stop_requested_.load(std::memory_order_relaxed)) {
            draining_ = true;
            stop_accepting(); // No new connections from here on.
            drain_deadline = now + std::chrono::seconds(settings_.shutdown_grace_sec);
            ORE_LOG(INFO) << "Graceful shutdown initiated; draining "
                          << connections_.size() << " connection(s)";
        }

        // Enforce timeouts on a fixed cadence (and every iteration while draining
        // so stalled connections do not hold up shutdown).
        if (draining_ ||
            std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup).count() >= kCleanupIntervalSec) {
            enforce_timeouts();
            last_cleanup = now;
        }

        if (draining_) {
            bool work_in_flight = false;
            for (const auto& pair : connections_) {
                if (pair.second->processing_ || pair.second->has_data_to_write()) {
                    work_in_flight = true;
                    break;
                }
            }
            if (!work_in_flight) {
                running_.store(false, std::memory_order_relaxed); // Clean drain.
            } else if (now >= drain_deadline) {
                ORE_LOG(WARN) << "Shutdown grace expired; dropping "
                              << connections_.size() << " connection(s) with work in flight";
                running_.store(false, std::memory_order_relaxed);
            }
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
        auto conn = std::make_shared<Net::Connection>(client_fd);
        char ipbuf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &client_addr.sin_addr, ipbuf, sizeof(ipbuf)) != nullptr) {
            conn->client_ip_ = ipbuf;
        }
        if (tls_ctx_) {
            SSL* ssl = tls_ctx_->new_session(client_fd);
            if (ssl == nullptr) {
                close(client_fd);
                continue;
            }
            conn->set_ssl(ssl); // Handshake is driven lazily on the first event.
        }
        connections_[client_fd] = std::move(conn);
        metrics_.connections_accepted.fetch_add(1, std::memory_order_relaxed);
        metrics_.connections_active.fetch_add(1, std::memory_order_relaxed);
    }

    if (client_fd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ORE_LOG(ERROR) << "Error accepting connection: " << strerror(errno);
    }
}

void Server::dispatch_next(int fd, const std::shared_ptr<Net::Connection>& conn) {
    if (conn->processing_) return; // A request is already in flight; wait for it.

    size_t consumed = 0;
    if (conn->parse_next(consumed)) {
        metrics_.requests_total.fetch_add(1, std::memory_order_relaxed);

        // Rate limit per client IP before doing the owning copy or spawning a
        // worker: a throttled request is answered with 429 directly here.
        if (rate_limiter_ && !rate_limiter_->allow(conn->client_ip_)) {
            conn->consume(consumed);
            conn->processing_ = true;
            metrics_.rate_limited_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.record_status(429);
            Http::HttpResponse res;
            nlohmann::json err;
            err["error"] = "Too Many Requests";
            res.status(Http::HttpStatus::TOO_MANY_REQUESTS).json(err);
            res.header("Retry-After", "1");
            conn->set_response_content(res);
            rearm(fd, /*read=*/false);
            return;
        }

        // Take an owning copy of the request so it can safely outlive the
        // socket buffer and be handed to a worker thread.
        auto request = std::make_shared<Http::HttpRequest>(std::move(conn->current_request_));
        request->make_owned(conn->read_buffer_.data(), consumed);
        conn->consume(consumed);
        conn->processing_ = true;
        conn->worker_in_flight_ = true;
        const auto t_start = std::chrono::steady_clock::now();
        conn->processing_since_ = t_start;

        thread_pool_->enqueue([this, fd, conn, request, t_start]() {
            Http::HttpResponse res;
            RouteHandler handler;
            std::unordered_map<std::string_view, std::string_view> path_params;

            // Run the middleware chain first. Any middleware may short-circuit
            // (return false) with a response already populated (auth rejection,
            // CORS preflight, ...), in which case the handler is skipped.
            bool proceed = true;
            for (const auto& mw : middlewares_) {
                try {
                    if (!mw(*request, res)) { proceed = false; break; }
                } catch (const std::exception& e) {
                    ORE_LOG(ERROR) << "Middleware exception: " << e.what();
                    nlohmann::json err;
                    err["error"] = "Server error";
                    res.status(Http::HttpStatus::INTERNAL_SERVER_ERROR).json(err);
                    proceed = false;
                    break;
                }
            }

            // HEAD reuses the GET handler; the body is stripped later.
            Http::HttpMethod method = request->method();
            bool found = proceed &&
                         router_->find_route(method, request->path(), path_params, handler);
            if (proceed && !found && method == Http::HttpMethod::HEAD) {
                found = router_->find_route(Http::HttpMethod::GET, request->path(), path_params, handler);
            }

            if (!proceed) {
                // A middleware already produced the response; fall through to the
                // semantics/completion handling below.
            } else if (found) {
                request->path_params_ = std::move(path_params);
                try {
                    handler(*request, res);
                } catch (const std::exception& e) {
                    ORE_LOG(ERROR) << "Handler exception: " << e.what();
                    nlohmann::json err;
                    err["error"] = "Server error";
                    res.status(Http::HttpStatus::INTERNAL_SERVER_ERROR).json(err);
                }
            } else {
                nlohmann::json err;
                err["error"] = "Not Found";
                res.status(Http::HttpStatus::NOT_FOUND).json(err);
            }

            // Apply request-driven response semantics (Range for file responses,
            // HEAD body suppression) before handing the response back.
            apply_http_semantics(*request, res);

            // Record metrics for this request (atomic; safe off the loop thread).
            metrics_.record_status(static_cast<int>(res.get_status()));
            metrics_.observe_duration(
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count());

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

    // Incomplete request: if the client is waiting for "100 Continue" before
    // sending the body, send it now, then wait for more data. Under TLS a read
    // may have blocked needing writability, so re-arm in the requested direction.
    conn->maybe_send_100_continue();
    const bool want_read =
        !(conn->uses_tls() && conn->tls_want() == Net::Connection::TlsWant::Write);
    rearm(fd, want_read);
}

bool Server::drive_tls_handshake(int fd, const std::shared_ptr<Net::Connection>& conn) {
    int r = conn->continue_tls_handshake();
    if (r == 1) return true; // Handshake complete; caller proceeds with I/O.
    if (r == 0) {
        rearm(fd, conn->tls_want() == Net::Connection::TlsWant::Read);
        return false;
    }
    close_connection(fd); // r < 0: handshake error.
    return false;
}

void Server::handle_client_data(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    std::shared_ptr<Net::Connection> conn = it->second;

    if (!conn->is_open()) {
        close_connection(fd);
        return;
    }

    // Complete the TLS handshake before any HTTP I/O.
    if (conn->uses_tls() && !conn->tls_handshake_done()) {
        if (!drive_tls_handshake(fd, conn)) return;
        // Handshake just finished on this readable event; fall through to read.
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

    // A writable event during the handshake (SSL_accept wanted to write).
    if (conn->uses_tls() && !conn->tls_handshake_done()) {
        if (!drive_tls_handshake(fd, conn)) return;
        // Handshake finished; now wait for the client's request.
        rearm(fd, /*read=*/true);
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
    if (!conn->keep_alive_ || draining_) {
        // During a graceful drain we do not reuse connections: close once the
        // in-flight response has been fully flushed.
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
    metrics_.connections_active.fetch_sub(1, std::memory_order_relaxed);
    // Close the socket now. If a worker still holds a shared_ptr, the object
    // stays alive but its fd is already closed (is_open() == false), so the
    // pending response will be dropped safely in process_completions().
    conn->close_connection();
}

void Server::stop_accepting() {
    if (listen_fd_ < 0) return;
#ifdef __linux__
    if (epoll_fd_ >= 0) {
        epoll_event ev{};
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, listen_fd_, &ev);
    }
#endif
    // Closing the fd also removes it from kqueue on BSD/macOS.
    close(listen_fd_);
    listen_fd_ = -1;
}

void Server::send_minimal_response(int fd, const char* bytes, size_t len) {
    auto it = connections_.find(fd);
    if (it == connections_.end() || !it->second->is_open()) return;
    const auto& conn = it->second;
    if (conn->uses_tls()) {
        // Only meaningful once the TLS session exists; otherwise just close.
        if (conn->tls_handshake_done()) {
            SSL_write(conn->ssl_, bytes, static_cast<int>(len)); // best-effort over TLS
        }
    } else {
        ::send(conn->socket_fd_, bytes, len, MSG_NOSIGNAL); // best-effort
    }
}

void Server::send_request_timeout(int fd) {
    static const char k408[] =
        "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    send_minimal_response(fd, k408, sizeof(k408) - 1);
}

void Server::send_handler_timeout(int fd) {
    static const char k504[] =
        "HTTP/1.1 504 Gateway Timeout\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    send_minimal_response(fd, k504, sizeof(k504) - 1);
}

void Server::enforce_timeouts() {
    const auto now = std::chrono::steady_clock::now();
    // Collect first, mutate after: close_connection() erases from connections_.
    std::vector<int> read_timeouts;     // -> 408
    std::vector<int> handler_timeouts;  // -> 504
    std::vector<int> plain_closes;      // idle / stalled write

    for (const auto& pair : connections_) {
        const auto& conn = pair.second;

        if (conn->worker_in_flight_) {
            // A worker is running the handler. It cannot be cancelled safely, so
            // on deadline we drop the connection (504); its late result is
            // discarded by process_completions' liveness guard.
            if (settings_.handler_timeout_sec > 0) {
                const long busy = std::chrono::duration_cast<std::chrono::seconds>(
                                      now - conn->processing_since_).count();
                if (busy > settings_.handler_timeout_sec) handler_timeouts.push_back(pair.first);
            }
            continue;
        }

        const long idle_sec = std::chrono::duration_cast<std::chrono::seconds>(
                                  now - conn->get_last_activity()).count();

        if (conn->has_data_to_write()) {
            // Response being written but the peer is not draining it.
            if (settings_.write_timeout_sec > 0 && idle_sec > settings_.write_timeout_sec) {
                plain_closes.push_back(pair.first);
            }
        } else if (conn->processing_) {
            continue; // Transient state with no data queued yet; leave it alone.
        } else if (conn->read_buffer_fill_ > 0) {
            // A request is partially buffered but not yet complete.
            if (settings_.read_timeout_sec > 0 && idle_sec > settings_.read_timeout_sec) {
                read_timeouts.push_back(pair.first);
            }
        } else {
            // Idle keep-alive connection awaiting the next request.
            if (settings_.idle_timeout_sec > 0 && idle_sec > settings_.idle_timeout_sec) {
                plain_closes.push_back(pair.first);
            }
        }
    }

    for (int fd : read_timeouts) {
        send_request_timeout(fd);
        close_connection(fd);
    }
    for (int fd : handler_timeouts) {
        metrics_.handler_timeouts_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.record_status(504);
        send_handler_timeout(fd);
        close_connection(fd);
    }
    for (int fd : plain_closes) {
        close_connection(fd);
    }

    // Bound the rate-limiter's memory by dropping idle (refilled) buckets.
    if (rate_limiter_) rate_limiter_->evict_idle();
}

} // namespace Server
} // namespace Oreshnek
