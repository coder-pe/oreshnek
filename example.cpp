#include "oreshnek.h"
#include <iostream>
#include <signal.h>

using namespace MiniRest;

// Global server instance for signal handling
Server* g_server = nullptr;

void signal_handler(int signal) {
    if (g_server) {
        std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
        g_server->stop();
    }
}

int main() {
    try {
        // Create server instance
        Server server;
        g_server = &server;
        
        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
        
        // Define API routes
        
        // GET /api/hello - Simple hello world
        server.get("/api/hello", [](const HttpRequest& req, HttpResponse& res) {
            JsonValue response;
            response["message"] = JsonValue("Hello, World!");
            response["timestamp"] = JsonValue(std::time(nullptr));
            res.json(response);
        });
        
        // GET /api/user/:id - User by ID with path parameter
        server.get("/api/user/:id", [](const HttpRequest& req, HttpResponse& res) {
            std::string user_id = req.param("id");
            
            if (user_id.empty()) {
                res.status(400);
                JsonValue error;
                error["error"] = JsonValue("Missing user ID");
                res.json(error);
                return;
            }
            
            JsonValue user;
            user["id"] = JsonValue(std::stoi(user_id));
            user["name"] = JsonValue("User " + user_id);
            user["email"] = JsonValue("user" + user_id + "@example.com");
            
            JsonValue response;
            response["user"] = user;
            response["success"] = JsonValue(true);
            
            res.json(response);
        });
        
        // POST /api/users - Create new user
        server.post("/api/users", [](const HttpRequest& req, HttpResponse& res) {
            try {
                JsonValue request_data = req.json();
                
                // Validate required fields
                if (request_data["name"].is_null() || request_data["email"].is_null()) {
                    res.status(400);
                    JsonValue error;
                    error["error"] = JsonValue("Missing required fields: name, email");
                    res.json(error);
                    return;
                }
                
                // Simulate user creation
                JsonValue new_user;
                new_user["id"] = JsonValue(12345); // Generated ID
                new_user["name"] = request_data["name"];
                new_user["email"] = request_data["email"];
                new_user["created_at"] = JsonValue(std::time(nullptr));
                
                JsonValue response;
                response["user"] = new_user;
                response["message"] = JsonValue("User created successfully");
                
                res.status(201);
                res.json(response);
                
            } catch (const std::exception& e) {
                res.status(400);
                JsonValue error;
                error["error"] = JsonValue("Invalid JSON data");
                error["details"] = JsonValue(e.what());
                res.json(error);
            }
        });
        
        // GET /api/users - List users with pagination
        server.get("/api/users", [](const HttpRequest& req, HttpResponse& res) {
            // Simulate user list
            JsonValue users;
            users.make_array();
            
            for (int i = 1; i <= 5; ++i) {
                JsonValue user;
                user["id"] = JsonValue(i);
                user["name"] = JsonValue("User " + std::to_string(i));
                user["email"] = JsonValue("user" + std::to_string(i) + "@example.com");
                users.push_back(user);
            }
            
            JsonValue response;
            response["users"] = users;
            response["total"] = JsonValue(5);
            response["page"] = JsonValue(1);
            response["per_page"] = JsonValue(10);
            
            res.json(response);
        });
        
        // PUT /api/user/:id - Update user
        server.put("/api/user/:id", [](const HttpRequest& req, HttpResponse& res) {
            std::string user_id = req.param("id");
            
            try {
                JsonValue request_data = req.json();
                
                JsonValue updated_user;
                updated_user["id"] = JsonValue(std::stoi(user_id));
                updated_user["name"] = request_data["name"].is_null() ? 
                    JsonValue("User " + user_id) : request_data["name"];
                updated_user["email"] = request_data["email"].is_null() ? 
                    JsonValue("user" + user_id + "@example.com") : request_data["email"];
                updated_user["updated_at"] = JsonValue(std::time(nullptr));
                
                JsonValue response;
                response["user"] = updated_user;
                response["message"] = JsonValue("User updated successfully");
                
                res.json(response);
                
            } catch (const std::exception& e) {
                res.status(400);
                JsonValue error;
                error["error"] = JsonValue("Invalid request data");
                res.json(error);
            }
        });
        
        // DELETE /api/user/:id - Delete user
        server.del("/api/user/:id", [](const HttpRequest& req, HttpResponse& res) {
            std::string user_id = req.param("id");
            
            JsonValue response;
            response["message"] = JsonValue("User " + user_id + " deleted successfully");
            response["deleted_id"] = JsonValue(std::stoi(user_id));
            
            res.json(response);
        });
        
        // GET /api/health - Health check endpoint
        server.get("/api/health", [](const HttpRequest& req, HttpResponse& res) {
            JsonValue health;
            health["status"] = JsonValue("healthy");
            health["uptime"] = JsonValue(std::time(nullptr));
            health["version"] = JsonValue("1.0.0");
            
            JsonValue checks;
            checks["database"] = JsonValue("connected");
            checks["cache"] = JsonValue("operational");
            health["checks"] = checks;
            
            res.json(health);
        });
        
        // GET /api/stats - Server statistics
        server.get("/api/stats", [](const HttpRequest& req, HttpResponse& res) {
            JsonValue stats;
            stats["requests_handled"] = JsonValue(42);
            stats["active_connections"] = JsonValue(5);
            stats["uptime_seconds"] = JsonValue(3600);
            stats["memory_usage_mb"] = JsonValue(128.5);
            
            res.json(stats);
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
    
    std::cout << "Server stopped" << std::endl;
    return 0;
}