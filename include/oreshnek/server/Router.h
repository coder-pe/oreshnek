// oreshnek/include/oreshnek/server/Router.h
#ifndef ORESHNEK_SERVER_ROUTER_H
#define ORESHNEK_SERVER_ROUTER_H

#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"
#include "oreshnek/http/HttpEnums.h"
#include <functional>
#include <string> // Use std::string for map keys to own data
#include <string_view>
#include <unordered_map>
#include <vector>
#include <memory> // For unique_ptr

namespace Oreshnek {
namespace Server {

// Type alias for route handler function
using RouteHandler = std::function<void(const Http::HttpRequest&, Http::HttpResponse&)>;

// Forward declaration for RouterNode
struct RouterNode;

// Represents a node in the routing tree
struct RouterNode {
    // CAMBIO: Cambiado de std::string_view a std::string para las claves
    std::unordered_map<std::string, std::unique_ptr<RouterNode>> children;
    std::unique_ptr<RouterNode> param_child; // Child for path parameters (e.g., :id)
    // CAMBIO: Cambiado de std::string_view a std::string para param_name
    std::string param_name; // Stores the name of the parameter for param_child

    // Handlers for different HTTP methods at this node
    std::unordered_map<Http::HttpMethod, RouteHandler> handlers;

    RouterNode() = default;
    RouterNode(const RouterNode&) = delete; // No copy
    RouterNode& operator=(const RouterNode&) = delete; // No copy assignment
    RouterNode(RouterNode&&) noexcept = default; // Move
    RouterNode& operator=(RouterNode&&) noexcept = default; // Move assign
};


class Router {
private:
    std::unique_ptr<RouterNode> root_;

    // Helper to add a route path segment by segment
    void add_route_recursive(RouterNode* current_node, Http::HttpMethod method,
                             std::vector<std::string_view>::const_iterator path_segment_it,
                             std::vector<std::string_view>::const_iterator path_segment_end,
                             RouteHandler handler);

    // Helper to match a route and extract path parameters
    bool match_route_recursive(const RouterNode* current_node,
                               std::vector<std::string_view>::const_iterator path_segment_it,
                               std::vector<std::string_view>::const_iterator path_segment_end,
                               Http::HttpMethod method,
                               // path_params_out puede seguir usando string_view, ya que apuntan a la ruta de la solicitud HTTP actual.
                               std::unordered_map<std::string_view, std::string_view>& path_params_out,
                               RouteHandler& matched_handler_out) const;

public:
    Router();

    // Add a route with its handler
    void add_route(Http::HttpMethod method, std::string_view path, RouteHandler handler);

    // Find a matching route and populate path parameters
    bool find_route(Http::HttpMethod method, std::string_view path,
                    std::unordered_map<std::string_view, std::string_view>& path_params_out,
                    RouteHandler& matched_handler_out) const;

private:
    // Helper to split a path into segments (e.g., "/api/users/:id" -> ["api", "users", ":id"])
    // Retorna string_views que apuntan a la cadena original `path` (que debe tener una vida útil suficiente).
    std::vector<std::string_view> split_path_to_segments(std::string_view path) const;
};

} // namespace Server
} // namespace Oreshnek

#endif // ORESHNEK_SERVER_ROUTER_H
