#include "oreshnek.h"
#include <algorithm>
#include <cctype>

namespace MiniRest {

// ==================== JsonValue Implementation ====================

void JsonValue::cleanup() {
    switch (type_) {
        case JsonType::STRING:
            delete str_val;
            break;
        case JsonType::ARRAY:
            delete arr_val;
            break;
        case JsonType::OBJECT:
            delete obj_val;
            break;
        default:
            break;
    }
}

void JsonValue::copy_from(const JsonValue& other) {
    type_ = other.type_;
    switch (type_) {
        case JsonType::BOOL:
            bool_val = other.bool_val;
            break;
        case JsonType::NUMBER:
            num_val = other.num_val;
            break;
        case JsonType::STRING:
            str_val = new std::string(*other.str_val);
            break;
        case JsonType::ARRAY:
            arr_val = new std::vector<JsonValue>(*other.arr_val);
            break;
        case JsonType::OBJECT:
            obj_val = new std::unordered_map<std::string, JsonValue>(*other.obj_val);
            break;
        default:
            break;
    }
}

void JsonValue::make_array() {
    if (type_ != JsonType::ARRAY) {
        cleanup();
        type_ = JsonType::ARRAY;
        arr_val = new std::vector<JsonValue>();
    }
}

void JsonValue::make_object() {
    if (type_ != JsonType::OBJECT) {
        cleanup();
        type_ = JsonType::OBJECT;
        obj_val = new std::unordered_map<std::string, JsonValue>();
    }
}

JsonValue& JsonValue::operator[](size_t index) {
    make_array();
    if (index >= arr_val->size()) {
        arr_val->resize(index + 1);
    }
    return (*arr_val)[index];
}

const JsonValue& JsonValue::operator[](size_t index) const {
    static JsonValue null_val;
    if (type_ != JsonType::ARRAY || index >= arr_val->size()) {
        return null_val;
    }
    return (*arr_val)[index];
}

void JsonValue::push_back(const JsonValue& val) {
    make_array();
    arr_val->push_back(val);
}

JsonValue& JsonValue::operator[](const std::string& key) {
    make_object();
    return (*obj_val)[key];
}

const JsonValue& JsonValue::operator[](const std::string& key) const {
    static JsonValue null_val;
    if (type_ != JsonType::OBJECT) {
        return null_val;
    }
    auto it = obj_val->find(key);
    return (it != obj_val->end()) ? it->second : null_val;
}

std::string JsonValue::to_string() const {
    switch (type_) {
        case JsonType::NULL_VALUE:
            return "null";
        case JsonType::BOOL:
            return bool_val ? "true" : "false";
        case JsonType::NUMBER:
            return std::to_string(num_val);
        case JsonType::STRING:
            return "\"" + *str_val + "\"";
        case JsonType::ARRAY: {
            std::string result = "[";
            for (size_t i = 0; i < arr_val->size(); ++i) {
                if (i > 0) result += ",";
                result += (*arr_val)[i].to_string();
            }
            result += "]";
            return result;
        }
        case JsonType::OBJECT: {
            std::string result = "{";
            bool first = true;
            for (const auto& pair : *obj_val) {
                if (!first) result += ",";
                result += "\"" + pair.first + "\":" + pair.second.to_string();
                first = false;
            }
            result += "}";
            return result;
        }
    }
    return "";
}

JsonValue JsonValue::parse(const std::string& json) {
    // Simplified JSON parser - in production use a proper parser
    JsonValue result;
    if (json.empty()) return result;
    
    std::string trimmed = json;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
    
    if (trimmed == "null") {
        return JsonValue();
    } else if (trimmed == "true") {
        return JsonValue(true);
    } else if (trimmed == "false") {
        return JsonValue(false);
    } else if (trimmed[0] == '"' && trimmed.back() == '"') {
        return JsonValue(trimmed.substr(1, trimmed.length() - 2));
    } else if (std::isdigit(trimmed[0]) || trimmed[0] == '-') {
        return JsonValue(std::stod(trimmed));
    }
    
    return result;
}

// ==================== HttpRequest Implementation ====================

const std::string& HttpRequest::header(const std::string& name) const {
    static const std::string empty;
    auto it = headers_.find(name);
    return (it != headers_.end()) ? it->second : empty;
}

void HttpRequest::set_header(const std::string& name, const std::string& value) {
    headers_[name] = value;
}

const std::string& HttpRequest::param(const std::string& name) const {
    static const std::string empty;
    auto it = params_.find(name);
    return (it != params_.end()) ? it->second : empty;
}

void HttpRequest::set_param(const std::string& name, const std::string& value) {
    params_[name] = value;
}

bool HttpRequest::parse(const std::string& raw_data) {
    std::istringstream stream(raw_data);
    std::string line;
    
    // Parse request line
    if (!std::getline(stream, line)) return false;
    
    std::istringstream request_line(line);
    std::string method_str, path_with_query, version;
    request_line >> method_str >> path_with_query >> version;
    
    // Parse method
    if (method_str == "GET") method_ = HttpMethod::GET;
    else if (method_str == "POST") method_ = HttpMethod::POST;
    else if (method_str == "PUT") method_ = HttpMethod::PUT;
    else if (method_str == "DELETE") method_ = HttpMethod::DELETE;
    else if (method_str == "PATCH") method_ = HttpMethod::PATCH;
    else if (method_str == "HEAD") method_ = HttpMethod::HEAD;
    else if (method_str == "OPTIONS") method_ = HttpMethod::OPTIONS;
    else return false;
    
    // Parse path and query string
    size_t query_pos = path_with_query.find('?');
    if (query_pos != std::string::npos) {
        path_ = path_with_query.substr(0, query_pos);
        query_string_ = path_with_query.substr(query_pos + 1);
    } else {
        path_ = path_with_query;
    }
    
    // Check HTTP version
    is_http2_ = (version == "HTTP/2.0");
    
    // Parse headers
    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string name = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            
            // Trim whitespace
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t\r") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r") + 1);
            
            headers_[name] = value;
        }
    }
    
    // Read body if present
    std::string content_length_str = header("Content-Length");
    if (!content_length_str.empty()) {
        size_t content_length = std::stoul(content_length_str);
        body_.resize(content_length);
        stream.read(&body_[0], content_length);
    }
    
    return true;
}

// ==================== HttpResponse Implementation ====================

void HttpResponse::status(int code) {
    status_code_ = code;
    // Set default status text based on code
    switch (code) {
        case 200: status_text_ = "OK"; break;
        case 201: status_text_ = "Created"; break;
        case 400: status_text_ = "Bad Request"; break;
        case 401: status_text_ = "Unauthorized"; break;
        case 404: status_text_ = "Not Found"; break;
        case 500: status_text_ = "Internal Server Error"; break;
        default: status_text_ = "Unknown"; break;
    }
}

void HttpResponse::status(int code, const std::string& text) {
    status_code_ = code;
    status_text_ = text;
}

void HttpResponse::header(const std::string& name, const std::string& value) {
    headers_[name] = value;
}

const std::string& HttpResponse::header(const std::string& name) const {
    static const std::string empty;
    auto it = headers_.find(name);
    return (it != headers_.end()) ? it->second : empty;
}

void HttpResponse::body(const std::string& content) {
    body_ = content;
    headers_["Content-Length"] = std::to_string(body_.size());
}

void HttpResponse::json(const JsonValue& value) {
    body_ = value.to_string();
    headers_["Content-Type"] = "application/json";
    headers_["Content-Length"] = std::to_string(body_.size());
}

std::string HttpResponse::to_string() const {
    std::string response;
    
    // Status line
    response += is_http2_ ? "HTTP/2.0 " : "HTTP/1.1 ";
    response += std::to_string(status_code_) + " " + status_text_ + "\r\n";
    
    // Headers
    for (const auto& header : headers_) {
        response += header.first + ": " + header.second + "\r\n";
    }
    
    response += "\r\n";
    response += body_;
    
    return response;
}

// ==================== Router Implementation ====================

std::vector<std::string> Router::split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string current;
    
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    
    if (!current.empty()) {
        parts.push_back(current);
    }
    
    return parts;
}

void Router::add_route(HttpMethod method, const std::string& path, RouteHandler handler) {
    auto parts = split_path(path);
    RouteNode* current = root_.get();
    
    for (const auto& part : parts) {
        if (part.front() == ':') {
            // Parameter route
            if (!current->param_child) {
                current->param_child = std::make_unique<RouteNode>();
            }
            current->param_child->param_name = part.substr(1);
            current = current->param_child.get();
        } else {
            // Static route
            if (current->children.find(part) == current->children.end()) {
                current->children[part] = std::make_unique<RouteNode>();
            }
            current = current->children[part].get();
        }
    }
    
    current->handlers[method] = handler;
}

bool Router::route(const HttpRequest& req, HttpResponse& res) {
    auto parts = split_path(req.path());
    RouteNode* current = root_.get();
    HttpRequest* mutable_req = const_cast<HttpRequest*>(&req);
    
    for (const auto& part : parts) {
        bool found = false;
        
        // Try static route first
        auto it = current->children.find(part);
        if (it != current->children.end()) {
            current = it->second.get();
            found = true;
        } else if (current->param_child) {
            // Try parameter route
            mutable_req->set_param(current->param_child->param_name, part);
            current = current->param_child.get();
            found = true;
        }
        
        if (!found) {
            res.status(404);
            res.json(JsonValue("Not Found"));
            return false;
        }
    }
    
    // Check if handler exists for this method
    auto handler_it = current->handlers.find(req.method());
    if (handler_it != current->handlers.end()) {
        handler_it->second(req, res);
        return true;
    }
    
    res.status(405);
    res.json(JsonValue("Method Not Allowed"));
    return false;
}

// ==================== Connection Implementation ====================

bool Connection::is_expired() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity_);
    return elapsed.count() > KEEP_ALIVE_TIMEOUT;
}

void Connection::set_non_blocking() {
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
}

bool Connection::read_request(HttpRequest& request) {
    char temp_buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = recv(socket_fd_, temp_buffer, BUFFER_SIZE - 1, 0)) > 0) {
        temp_buffer[bytes_read] = '\0';
        buffer_ += temp_buffer;
        
        // Check if we have complete request (HTTP/1.1)
        size_t header_end = buffer_.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            update_activity();
            
            // Parse the request
            std::string request_data = buffer_.substr(0, header_end + 4);
            
            // Check if there's a body
            auto it = buffer_.find("Content-Length:");
            if (it != std::string::npos) {
                size_t content_length = 0;
                std::istringstream stream(buffer_.substr(it));
                std::string header_line;
                std::getline(stream, header_line);
                
                size_t colon_pos = header_line.find(':');
                if (colon_pos != std::string::npos) {
                    std::string value = header_line.substr(colon_pos + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    content_length = std::stoul(value);
                }
                
                size_t body_start = header_end + 4;
                if (buffer_.size() >= body_start + content_length) {
                    request_data = buffer_.substr(0, body_start + content_length);
                } else {
                    // Need more data for body
                    continue;
                }
            }
            
            bool parsed = request.parse(request_data);
            
            // Clear processed data from buffer
            buffer_.clear();
            
            return parsed;
        }
    }
    
    if (bytes_read == 0) {
        // Connection closed by client
        keep_alive_ = false;
        return false;
    }
    
    if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Real error occurred
        keep_alive_ = false;
        return false;
    }
    
    return false; // Need more data
}

bool Connection::write_response(const HttpResponse& response) {
    std::string response_str = response.to_string();
    ssize_t total_sent = 0;
    ssize_t response_size = response_str.size();
    
    while (total_sent < response_size) {
        ssize_t sent = send(socket_fd_, response_str.c_str() + total_sent, 
                           response_size - total_sent, MSG_NOSIGNAL);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; // Try again
            }
            return false;
        }
        
        total_sent += sent;
    }
    
    update_activity();
    return true;
}

void Connection::close_connection() {
    keep_alive_ = false;
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

// ==================== ThreadPool Implementation ====================

ThreadPool::ThreadPool(size_t threads) : stop_(false) {
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                    
                    if (stop_ && tasks_.empty()) return;
                    
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    
    condition_.notify_all();
    
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace MiniRest