// examples/03_rest_crud_db.cpp
//
// Use case: a small REST API backed by the database abstraction.
//
// The framework's database layer is a domain-agnostic SQL gateway: it exposes
// only query()/exec() with parameterized statements and returns generic rows.
// The application owns its schema and models — this example defines its own
// `User` struct, runs its own DDL, and maps SqlResult rows into it. Switching to
// PostgreSQL is a one-line config change (db.backend="postgres"); only the SQL
// dialect would differ, not this code.
//
//   curl -d '{"username":"edgar","email":"e@x.io","password":"s3cret"}' \
//        localhost:8080/api/users
//   curl -d '{"username":"edgar","password":"s3cret"}' localhost:8080/api/login
//   curl localhost:8080/api/users/edgar

#include "oreshnek/Oreshnek.h"
#include "oreshnek/platform/DatabaseManager.h"
#include "oreshnek/platform/SecurityUtils.h"
#include "common.h"

#include <string>

using namespace Oreshnek;

namespace {

// The application's own model — the framework knows nothing about it.
struct User {
    long long id = 0;  // 0 means "not found"
    std::string username;
    std::string email;
    std::string password_hash;
    std::string role;
};

// Map one result row into a User (column order matches the SELECT below).
User user_from_row(const Platform::SqlResult& r, std::size_t row) {
    User u;
    u.id = r.integer(row, 0);
    u.username = r.text(row, 1);
    u.email = r.text(row, 2);
    u.password_hash = r.text(row, 3);
    u.role = r.text(row, 4);
    return u;
}

User find_user(Platform::DatabaseManager& db, const std::string& username) {
    auto r = db.query(
        "SELECT id, username, email, password_hash, role FROM users WHERE username = ?;",
        {username});
    if (!r.ok || r.empty()) return {};
    return user_from_row(r, 0);
}

}  // namespace

int main() {
    // The backend is chosen by configuration. Defaults to SQLite at ./example.db;
    // point db.backend at "postgres" (and fill db.postgres) to switch databases.
    Platform::ServerConfig cfg;
    cfg.db.backend = "sqlite";
    cfg.db.sqlite_path = "./example.db";
    cfg.jwt_secret = "example-secret-change-me";

    Platform::DatabaseManager db(cfg);

    // The application runs its own schema migration at startup.
    db.exec(
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "username TEXT UNIQUE NOT NULL, "
        "email TEXT UNIQUE NOT NULL, "
        "password_hash TEXT NOT NULL, "
        "role TEXT NOT NULL DEFAULT 'user');");

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
        const std::string username = body["username"].get<std::string>();
        if (find_user(db, username).id != 0) {
            res.status(Http::HttpStatus::CONFLICT).json({{"error", "user exists"}});
            return;
        }
        const std::string hash =
            Platform::SecurityUtils::hashPassword(body["password"].get<std::string>());
        auto r = db.exec(
            "INSERT INTO users (username, email, password_hash, role) VALUES (?, ?, ?, ?);",
            {username, body["email"].get<std::string>(), hash, "user"});
        res.status(r.ok ? Http::HttpStatus::CREATED : Http::HttpStatus::INTERNAL_SERVER_ERROR)
           .json({{"created", r.ok}});
    });

    // Log in: verify the password and return a JWT.
    server.post("/api/login", [&db, &cfg](const HttpRequest& req, HttpResponse& res) {
        nlohmann::json body;
        try { body = req.json(); } catch (...) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "invalid JSON"}});
            return;
        }
        User u = find_user(db, body.value("username", ""));
        if (u.id == 0 ||
            !Platform::SecurityUtils::verifyPassword(body.value("password", ""), u.password_hash)) {
            res.status(Http::HttpStatus::UNAUTHORIZED).json({{"error", "invalid credentials"}});
            return;
        }
        res.status(Http::HttpStatus::OK).json(
            {{"token", Platform::SecurityUtils::generateJWT(
                           static_cast<int>(u.id), u.username, cfg.jwt_secret)}});
    });

    // Fetch a user by name (no secrets in the response).
    server.get("/api/users/:name", [&db](const HttpRequest& req, HttpResponse& res) {
        User u = find_user(db, std::string(req.param("name").value_or("")));
        if (u.id == 0) {
            res.status(Http::HttpStatus::NOT_FOUND).json({{"error", "not found"}});
            return;
        }
        res.status(Http::HttpStatus::OK).json(
            {{"id", u.id}, {"username", u.username}, {"email", u.email}, {"role", u.role}});
    });

    return ex::serve(server, "0.0.0.0", 8080);
}
