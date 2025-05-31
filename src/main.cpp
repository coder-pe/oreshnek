// src/main.cpp
#include "oreshnek/Oreshnek.h" // Include the convenience header
#include <iostream>
#include <signal.h>
#include <ctime> // For std::time

// Global server instance for signal handling
Oreshnek::Server* g_server = nullptr;

void signal_handler(int signal) {
    if (g_server) {
        std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
        g_server->stop();
    }
}

int main() {
    try {
        // Create server instance with a specific number of worker threads
        // Using std::thread::hardware_concurrency() is a good default.
        Oreshnek::Server server(std::thread::hardware_concurrency());
        g_server = &server; // Set global pointer for signal handling

        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // Define API routes

        // GET /api/hello - Simple hello world
        server.get("/api/hello", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            Oreshnek::JsonValue response = Oreshnek::JsonValue::object();
            response["message"] = Oreshnek::JsonValue("Hello, World!");
            response["timestamp"] = Oreshnek::JsonValue(static_cast<double>(std::time(nullptr))); // Use double for JsonValue number
            res.json(response);
        });

        // GET /api/user/:id - User by ID with path parameter
        server.get("/api/user/:id", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            std::optional<std::string_view> user_id_opt = req.param("id");

            if (!user_id_opt) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST);
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["error"] = Oreshnek::JsonValue("Missing user ID");
                res.json(error_json);
                return;
            }

            std::string_view user_id = *user_id_opt;

            Oreshnek::JsonValue user_json = Oreshnek::JsonValue::object();
            user_json["id"] = Oreshnek::JsonValue(std::string(user_id)); // Copy string_view to std::string for JsonValue
            user_json["name"] = Oreshnek::JsonValue("John Doe");
            user_json["email"] = Oreshnek::JsonValue(std::string("john.doe_") + std::string(user_id) + "@example.com");

            res.json(user_json);
        });

        // POST /api/data - Echo request body
        server.post("/api/data", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            try {
                Oreshnek::JsonValue request_body_json = req.json();
                res.status(Oreshnek::Http::HttpStatus::OK);
                res.json(request_body_json); // Echo back the JSON body
            } catch (const std::exception& e) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST);
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["error"] = Oreshnek::JsonValue(std::string("Failed to parse JSON body: ") + e.what());
                res.json(error_json);
            }
        });
        
        // GET /api/query - Example with query parameters
        server.get("/api/query", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            std::optional<std::string_view> name_opt = req.query("name");
            std::optional<std::string_view> age_opt = req.query("age");

            Oreshnek::JsonValue response = Oreshnek::JsonValue::object();
            response["message"] = Oreshnek::JsonValue("Query parameters received");
            if (name_opt) {
                response["name"] = Oreshnek::JsonValue(std::string(*name_opt));
            }
            if (age_opt) {
                try {
                    response["age"] = Oreshnek::JsonValue(std::stod(std::string(*age_opt))); // Convert string_view to string first
                } catch (const std::exception& e) {
                    response["age_error"] = Oreshnek::JsonValue(std::string("Invalid age format: ") + e.what());
                }
            }
            res.json(response);
        });

        // GET /api/health - Health check endpoint
        server.get("/api/health", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            Oreshnek::JsonValue health = Oreshnek::JsonValue::object();
            health["status"] = Oreshnek::JsonValue("healthy");
            health["uptime"] = Oreshnek::JsonValue(static_cast<double>(std::time(nullptr)));
            health["version"] = Oreshnek::JsonValue("1.0.0");

            Oreshnek::JsonValue checks = Oreshnek::JsonValue::object();
            checks["database"] = Oreshnek::JsonValue("connected");
            checks["cache"] = Oreshnek::JsonValue("operational");
            health["checks"] = checks;

            res.json(health);
        });

        // GET /api/stats - Server statistics
        server.get("/api/stats", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            Oreshnek::JsonValue stats = Oreshnek::JsonValue::object();
            stats["requests_handled"] = Oreshnek::JsonValue(42.0); // Example values
            stats["active_connections"] = Oreshnek::JsonValue(5.0);
            stats["uptime_seconds"] = Oreshnek::JsonValue(3600.0);
            stats["memory_usage_mb"] = Oreshnek::JsonValue(128.5);

            res.json(stats);
        });
        
        // GET /api/redirect - Example redirect
        server.get("/api/redirect", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.status(Oreshnek::Http::HttpStatus::OK) // Or 302 for temporary redirect
               .header("Location", "/api/hello")
               .text("Redirecting to /api/hello");
        });

        // Start server
        if (!server.listen("0.0.0.0", 8080)) {
            std::cerr << "Failed to start server" << std::endl;
            return 1;
        }

        // Run server (blocking call)
        server.run();

    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
