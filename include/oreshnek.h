#ifndef ORESHNEK_H
#define ORESHNEK_H

#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iostream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>

namespace MiniRest {

// Forward declarations
class HttpRequest;
class HttpResponse;
class Connection;
class Server;

// HTTP Method enum
enum class HttpMethod {
    GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS
};

// JSON Value types
enum class JsonType {
    NULL_VALUE, BOOL, NUMBER, STRING, ARRAY, OBJECT
};

// Fast JSON implementation
class JsonValue {
private:
    JsonType type_;
    union {
        bool bool_val;
        double num_val;
        std::string* str_val;
        std::vector<JsonValue>* arr_val;
        std::unordered_map<std::string, JsonValue>* obj_val;
    };

public:
    JsonValue() : type_(JsonType::NULL_VALUE) {}
    JsonValue(bool val) : type_(JsonType::BOOL), bool_val(val) {}
    JsonValue(double val) : type_(JsonType::NUMBER), num_val(val) {}
    JsonValue(const std::string& val) : type_(JsonType::STRING) {
        str_val = new std::string(val);
    }
    JsonValue(const char* val) : type_(JsonType::STRING) {
        str_val = new std::string(val);
    }
    
    ~JsonValue() { cleanup(); }
    
    JsonValue(const JsonValue& other) { copy_from(other); }
    JsonValue& operator=(const JsonValue& other) {
        if (this != &other) {
            cleanup();
            copy_from(other);
        }
        return *this;
    }
    
    // Array operations
    JsonValue& operator[](size_t index);
    const JsonValue& operator[](size_t index) const;
    void push_back(const JsonValue& val);
    
    // Object operations
    JsonValue& operator[](const std::string& key);
    const JsonValue& operator[](const std::string& key) const;
    
    // Type checks and getters
    JsonType type() const { return type_; }
    bool is_null() const { return type_ == JsonType::NULL_VALUE; }
    bool is_bool() const { return type_ == JsonType::BOOL; }
    bool is_number() const { return type_ == JsonType::NUMBER; }
    bool is_string() const { return type_ == JsonType::STRING; }
    bool is_array() const { return type_ == JsonType::ARRAY; }
    bool is_object() const { return type_ == JsonType::OBJECT; }
    
    bool as_bool() const { return bool_val; }
    double as_number() const { return num_val; }
    const std::string& as_string() const { return *str_val; }
    
    std::string to_string() const;
    static JsonValue parse(const std::string& json);

private:
    void cleanup();
    void copy_from(const JsonValue& other);
    void make_array();
    void make_object();
};

// HTTP Request class
class HttpRequest {
private:
    HttpMethod method_;
    std::string path_;
    std::string query_string_;
    std::unordered_map<std::string, std::string> headers_;
    std::unordered_map<std::string, std::string> params_;
    std::string body_;
    bool is_http2_;

public:
    HttpRequest() : method_(HttpMethod::GET), is_http2_(false) {}
    
    // Getters
    HttpMethod method() const { return method_; }
    const std::string& path() const { return path_; }
    const std::string& query_string() const { return query_string_; }
    const std::string& body() const { return body_; }
    bool is_http2() const { return is_http2_; }
    
    // Header operations
    const std::string& header(const std::string& name) const;
    void set_header(const std::string& name, const std::string& value);
    
    // Parameter operations
    const std::string& param(const std::string& name) const;
    void set_param(const std::string& name, const std::string& value);
    
    // JSON body parsing
    JsonValue json() const { return JsonValue::parse(body_); }
    
    // Parsing from raw HTTP data
    bool parse(const std::string& raw_data);
    
    friend class Connection;
};

// HTTP Response class
class HttpResponse {
private:
    int status_code_;
    std::string status_text_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
    bool is_http2_;

public:
    HttpResponse() : status_code_(200), status_text_("OK"), is_http2_(false) {
        headers_["Content-Type"] = "application/json";
    }
    
    // Status operations
    void status(int code);
    void status(int code, const std::string& text);
    
    // Header operations
    void header(const std::string& name, const std::string& value);
    const std::string& header(const std::string& name) const;
    
    // Body operations
    void body(const std::string& content);
    void json(const JsonValue& value);
    
    // Build HTTP response string
    std::string to_string() const;
    
    friend class Connection;
};

// Route handler type
using RouteHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

// Efficient routing trie node
struct RouteNode {
    std::unordered_map<HttpMethod, RouteHandler> handlers;
    std::unordered_map<std::string, std::unique_ptr<RouteNode>> children;
    std::unique_ptr<RouteNode> param_child;
    std::string param_name;
    
    RouteNode() = default;
};

// High-performance router
class Router {
private:
    std::unique_ptr<RouteNode> root_;
    
public:
    Router() : root_(std::make_unique<RouteNode>()) {}
    
    void add_route(HttpMethod method, const std::string& path, RouteHandler handler);
    bool route(const HttpRequest& req, HttpResponse& res);
    
private:
    std::vector<std::string> split_path(const std::string& path);
};

// Connection class for managing client connections
class Connection {
private:
    int socket_fd_;
    std::string buffer_;
    std::atomic<bool> keep_alive_;
    std::chrono::steady_clock::time_point last_activity_;
    bool is_http2_;
    
    static constexpr size_t BUFFER_SIZE = 8192;
    static constexpr int KEEP_ALIVE_TIMEOUT = 60; // seconds

public:
    Connection(int fd) : socket_fd_(fd), keep_alive_(true), is_http2_(false) {
        last_activity_ = std::chrono::steady_clock::now();
        set_non_blocking();
    }
    
    ~Connection() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
        }
    }
    
    int fd() const { return socket_fd_; }
    bool is_alive() const { return keep_alive_.load(); }
    bool is_expired() const;
    
    bool read_request(HttpRequest& request);
    bool write_response(const HttpResponse& response);
    void close_connection();
    
private:
    void set_non_blocking();
    void update_activity() { last_activity_ = std::chrono::steady_clock::now(); }
};

// Thread pool for handling requests
class ThreadPool {
private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;

public:
    ThreadPool(size_t threads = std::thread::hardware_concurrency());
    ~ThreadPool();
    
    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) return;
            tasks_.emplace(std::forward<F>(f));
        }
        condition_.notify_one();
    }
    
    void shutdown();
};

// Main server class with async I/O
class Server {
private:
    int listen_fd_;
    int epoll_fd_;
    std::atomic<bool> running_;
    std::unique_ptr<Router> router_;
    std::unique_ptr<ThreadPool> thread_pool_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::mutex connections_mutex_;
    
    static constexpr int MAX_EVENTS = 1024;
    static constexpr int BACKLOG = 1024;

public:
    Server() : listen_fd_(-1), epoll_fd_(-1), running_(false) {
        router_ = std::make_unique<Router>();
        thread_pool_ = std::make_unique<ThreadPool>();
    }
    
    ~Server() { stop(); }
    
    // Route registration methods
    void get(const std::string& path, RouteHandler handler) {
        router_->add_route(HttpMethod::GET, path, handler);
    }
    
    void post(const std::string& path, RouteHandler handler) {
        router_->add_route(HttpMethod::POST, path, handler);
    }
    
    void put(const std::string& path, RouteHandler handler) {
        router_->add_route(HttpMethod::PUT, path, handler);
    }
    
    void del(const std::string& path, RouteHandler handler) {
        router_->add_route(HttpMethod::DELETE, path, handler);
    }
    
    // Server control
    bool listen(const std::string& host = "0.0.0.0", int port = 8080);
    void run();
    void stop();
    
private:
    bool setup_socket(const std::string& host, int port);
    bool setup_epoll();
    void handle_new_connection();
    void handle_client_data(int client_fd);
    void process_request(std::unique_ptr<Connection> conn, HttpRequest request);
    void cleanup_expired_connections();
    void set_non_blocking(int fd);
};

} // namespace MiniRest

#endif // ORESHNEK_H