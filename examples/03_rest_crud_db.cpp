// examples/03_rest_crud_db.cpp
//
// Use case: a small REST API backed by the database abstraction.
// Customization points shown: selecting a backend through ServerConfig
// (SQLite here; set db.backend="postgres" for PostgreSQL with no code change),
// password hashing + JWT login, JSON request/response.
//
//   curl -d '{"username":"edgar","email":"e@x.io","password":"s3cret"}' \
//        localhost:8080/api/users
//   curl -d '{"username":"edgar","password":"s3cret"}' localhost:8080/api/login
//   curl localhost:8080/api/users/edgar

#include "oreshnek/Oreshnek.h"
#include "oreshnek/platform/DatabaseManager.h"
#include "oreshnek/platform/SecurityUtils.h"
#include "common.h"

using namespace Oreshnek;

int main() {
    // The backend is chosen by configuration. Defaults to SQLite at ./example.db;
    // point db.backend at "postgres" (and fill db.postgres) to switch databases
    // without touching any handler below.
    Platform::ServerConfig cfg;
    cfg.db.backend = "sqlite";
    cfg.db.sqlite_path = "./example.db";
    cfg.jwt_secret = "example-secret-change-me";

    Platform::DatabaseManager db(cfg);

    Server::Server server(4);

    // Create a user (hashes the password with PBKDF2 + per-user salt).
    server.post("/api/users", [&db](const HttpRequest& req, HttpResponse& res) {
        nlohmann::json body;
        try { body = req.json(); } catch (...) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "invalid JSON"}});
            return;
        }
        if (!body.contains("username") || !body.contains("email") || !body.contains("password")) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "missing fields"}});
            return;
        }
        Platform::User u;
        u.username = body["username"].get<std::string>();
        u.email = body["email"].get<std::string>();
        u.role = "student";
        u.password_hash = Platform::SecurityUtils::hashPassword(body["password"].get<std::string>());

        if (db.getUserByUsername(u.username).id != 0) {
            res.status(Http::HttpStatus::CONFLICT).json({{"error", "user exists"}});
            return;
        }
        const bool ok = db.createUser(u);
        res.status(ok ? Http::HttpStatus::CREATED : Http::HttpStatus::INTERNAL_SERVER_ERROR)
           .json({{"created", ok}});
    });

    // Log in: verify the password and return a JWT.
    server.post("/api/login", [&db, &cfg](const HttpRequest& req, HttpResponse& res) {
        nlohmann::json body;
        try { body = req.json(); } catch (...) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "invalid JSON"}});
            return;
        }
        Platform::User u = db.getUserByUsername(body.value("username", ""));
        if (u.id == 0 ||
            !Platform::SecurityUtils::verifyPassword(body.value("password", ""), u.password_hash)) {
            res.status(Http::HttpStatus::UNAUTHORIZED).json({{"error", "invalid credentials"}});
            return;
        }
        res.status(Http::HttpStatus::OK).json(
            {{"token", Platform::SecurityUtils::generateJWT(u.id, u.username, cfg.jwt_secret)}});
    });

    // Fetch a user by name (no secrets in the response).
    server.get("/api/users/:name", [&db](const HttpRequest& req, HttpResponse& res) {
        Platform::User u = db.getUserByUsername(std::string(req.param("name").value_or("")));
        if (u.id == 0) {
            res.status(Http::HttpStatus::NOT_FOUND).json({{"error", "not found"}});
            return;
        }
        res.status(Http::HttpStatus::OK).json(
            {{"id", u.id}, {"username", u.username}, {"email", u.email}, {"role", u.role}});
    });

    return ex::serve(server, "0.0.0.0", 8080);
}
