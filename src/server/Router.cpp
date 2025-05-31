// oreshnek/src/server/Router.cpp
#include "oreshnek/server/Router.h"
#include <sstream> // For splitting paths
#include <iostream> // For debugging

namespace Oreshnek {
namespace Server {

Router::Router() : root_(std::make_unique<RouterNode>()) {}

void Router::add_route(Http::HttpMethod method, std::string_view path, RouteHandler handler) {
    if (path.empty() || path[0] != '/') {
        throw std::runtime_error("Invalid route path: Must start with '/'");
    }

    // Handle root path separately to avoid issues with empty segments
    if (path == "/") {
        root_->handlers[method] = std::move(handler);
        return;
    }

    std::vector<std::string_view> segments = split_path_to_segments(path.substr(1)); // Remove leading '/'
    add_route_recursive(root_.get(), method, segments.begin(), segments.end(), std::move(handler));
}

void Router::add_route_recursive(RouterNode* current_node, Http::HttpMethod method,
                                 std::vector<std::string_view>::const_iterator path_segment_it,
                                 std::vector<std::string_view>::const_iterator path_segment_end,
                                 RouteHandler handler) {
    if (path_segment_it == path_segment_end) {
        // Reached the end of the path, register the handler here
        current_node->handlers[method] = std::move(handler);
        return;
    }

    std::string_view segment = *path_segment_it;

    if (segment.length() > 0 && segment[0] == ':') {
        // This is a path parameter
        if (!current_node->param_child) {
            current_node->param_child = std::make_unique<RouterNode>();
            current_node->param_name = segment.substr(1); // Store parameter name without ':'
        } else if (current_node->param_name != segment.substr(1)) {
             // Consistency check: ensure the same parameter name is used if param_child exists
             // Or allow multiple param names and just use the last one encountered (more complex).
             // For simplicity, enforcing same param name here.
             std::cerr << "Warning: Route " << segment << " has conflicting parameter name '"
                       << current_node->param_name << "' vs '" << segment.substr(1) << "'" << std::endl;
        }
        add_route_recursive(current_node->param_child.get(), method, ++path_segment_it, path_segment_end, std::move(handler));
    } else {
        // Regular path segment
        if (!current_node->children.count(segment)) {
            current_node->children[segment] = std::make_unique<RouterNode>();
        }
        add_route_recursive(current_node->children[segment].get(), method, ++path_segment_it, path_segment_end, std::move(handler));
    }
}

bool Router::find_route(Http::HttpMethod method, std::string_view path,
                        std::unordered_map<std::string_view, std::string_view>& path_params_out,
                        RouteHandler& matched_handler_out) const {
    if (path.empty() || path[0] != '/') {
        return false; // Invalid path
    }

    // Handle root path directly
    if (path == "/") {
        auto it = root_->handlers.find(method);
        if (it != root_->handlers.end()) {
            matched_handler_out = it->second;
            return true;
        }
        return false;
    }

    std::vector<std::string_view> segments = split_path_to_segments(path.substr(1)); // Remove leading '/'
    return match_route_recursive(root_.get(), segments.begin(), segments.end(), method, path_params_out, matched_handler_out);
}

bool Router::match_route_recursive(const RouterNode* current_node,
                                   std::vector<std::string_view>::const_iterator path_segment_it,
                                   std::vector<std::string_view>::const_iterator path_segment_end,
                                   Http::HttpMethod method,
                                   std::unordered_map<std::string_view, std::string_view>& path_params_out,
                                   RouteHandler& matched_handler_out) const {
    if (!current_node) {
        return false;
    }

    if (path_segment_it == path_segment_end) {
        // Reached the end of the path, check if a handler exists for the method
        auto it = current_node->handlers.find(method);
        if (it != current_node->handlers.end()) {
            matched_handler_out = it->second;
            return true;
        }
        return false;
    }

    std::string_view segment = *path_segment_it;

    // Try to match a static segment
    auto static_child_it = current_node->children.find(segment);
    if (static_child_it != current_node->children.end()) {
        if (match_route_recursive(static_child_it->second.get(), ++path_segment_it, path_segment_end, method, path_params_out, matched_handler_out)) {
            return true;
        }
    }

    // If static match failed, try to match a parameter segment
    if (current_node->param_child) {
        path_params_out[current_node->param_name] = segment; // Add parameter value
        if (match_route_recursive(current_node->param_child.get(), ++path_segment_it, path_segment_end, method, path_params_out, matched_handler_out)) {
            return true;
        }
        // If parameter path didn't match, remove it from params_out for backtracking
        path_params_out.erase(current_node->param_name);
    }

    return false;
}

std::vector<std::string_view> Router::split_path_to_segments(std::string_view path) const {
    std::vector<std::string_view> segments;
    size_t start = 0;
    size_t end = path.find('/');

    while (end != std::string_view::npos) {
        if (end > start) {
            segments.push_back(path.substr(start, end - start));
        }
        start = end + 1;
        end = path.find('/', start);
    }
    if (start < path.length()) {
        segments.push_back(path.substr(start));
    }
    return segments;
}

} // namespace Server
} // namespace Oreshnek
