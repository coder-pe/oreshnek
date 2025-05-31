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
        // root_ es inicializado en el constructor, esta verificación es principalmente para robustez.
        if (!root_) {
            root_ = std::make_unique<RouterNode>();
        }
        root_->handlers[method] = std::move(handler);
        return;
    }

    // `path` aquí es un std::string_view que apunta a la std::string pasada desde Server::get.
    // Los segmentos resultantes de split_path_to_segments apuntarán a los datos subyacentes de `path`, lo cual es correcto.
    std::vector<std::string_view> segments = split_path_to_segments(path.substr(1)); // Remove leading '/'
    add_route_recursive(root_.get(), method, segments.begin(), segments.end(), std::move(handler));
}

void Router::add_route_recursive(RouterNode* current_node, Http::HttpMethod method,
                                 std::vector<std::string_view>::const_iterator path_segment_it,
                                 std::vector<std::string_view>::const_iterator path_segment_end,
                                 RouteHandler handler) {
    if (!current_node) {
        // Este error crítico indica que add_route_recursive fue llamada con un puntero nulo,
        // lo cual no debería ocurrir si la lógica de construcción de nodos es correcta.
        std::cerr << "CRITICAL ERROR: add_route_recursive called with null current_node." << std::endl;
        return;
    }


    if (path_segment_it == path_segment_end) {
        // Al final de la ruta, registra el manejador aquí.
        current_node->handlers[method] = std::move(handler);
        return;
    }

    std::string_view segment_view = *path_segment_it; // Vista temporal para el segmento actual

    if (segment_view.length() > 0 && segment_view[0] == ':') {
        // Esto es un parámetro de ruta
        if (!current_node->param_child) {
            current_node->param_child = std::make_unique<RouterNode>();
            // CAMBIO: Almacena el nombre del parámetro como std::string para poseer los datos.
            current_node->param_name = std::string(segment_view.substr(1)); 
        } else if (current_node->param_name != segment_view.substr(1)) { // La comparación con string_view está bien.
             std::cerr << "Warning: Route segment '" << segment_view << "' has conflicting parameter name '"
                       << current_node->param_name << "' vs '" << segment_view.substr(1) << "'" << std::endl;
        }
        add_route_recursive(current_node->param_child.get(), method, ++path_segment_it, path_segment_end, std::move(handler));
    } else {
        // Segmento de ruta regular
        // CAMBIO: Convierte a std::string para la clave del mapa, asegurando la propiedad de los datos.
        std::string segment_key(segment_view); 
        
        if (!current_node->children.count(segment_key)) {
            current_node->children[segment_key] = std::make_unique<RouterNode>();
        }
        add_route_recursive(current_node->children[segment_key].get(), method, ++path_segment_it, path_segment_end, std::move(handler));
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
        if (!root_) return false;
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
        auto it = current_node->handlers.find(method);
        if (it != current_node->handlers.end()) {
            matched_handler_out = it->second;
            return true;
        }
        return false;
    }

    std::string_view segment_view = *path_segment_it; // Segmento actual de la ruta de la solicitud entrante

    // Intenta hacer coincidir un segmento estático
    // CAMBIO: Necesita convertir std::string_view a std::string para buscar en el mapa con claves std::string.
    std::string segment_match_key(segment_view);
    auto static_child_it = current_node->children.find(segment_match_key);
    if (static_child_it != current_node->children.end()) {
        if (match_route_recursive(static_child_it->second.get(), ++path_segment_it, path_segment_end, method, path_params_out, matched_handler_out)) {
            return true;
        }
    }

    // Si la coincidencia estática falla, intenta coincidir con un segmento de parámetro
    if (current_node->param_child) {
        // param_name es ahora std::string, así que úsalo como clave.
        // El valor sigue siendo string_view de la ruta de la solicitud, lo cual es correcto para path_params_out.
        path_params_out[current_node->param_name] = segment_view; 
        if (match_route_recursive(current_node->param_child.get(), ++path_segment_it, path_segment_end, method, path_params_out, matched_handler_out)) {
            return true;
        }
        // Si la ruta del parámetro no coincide, elimínalo de params_out para el backtracking.
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
    if (start < path.length()) { // Add the last segment if any
        segments.push_back(path.substr(start));
    }
    return segments;
}

} // namespace Server
} // namespace Oreshnek
