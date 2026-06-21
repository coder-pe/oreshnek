// examples/06_video_platform.cpp
//
// Use case: a video-platform API built *on top of* the framework.
//
// This is the home of the domain that used to be baked into the framework
// (users, videos, comments). The framework itself is now domain-agnostic — it
// offers a generic SQL gateway (query/exec) and HTTP/routing/auth primitives.
// An application like this one owns its models, its schema and its SQL, mapping
// the framework's generic SqlResult rows into its own structs. That is the whole
// point: Oreshnek is general-purpose; "video streaming" is just one app.
//
//   curl -d '{"username":"edgar","email":"e@x.io","password":"s3cret","role":"creator"}' \
//        localhost:8080/api/register
//   TOKEN=$(curl -s -d '{"username":"edgar","password":"s3cret"}' \
//        localhost:8080/api/login | sed 's/.*"token":"\([^"]*\)".*/\1/')
//   curl -H "Authorization: Bearer $TOKEN" \
//        -d '{"title":"Hello","filename":"hello.mp4","category":"demo"}' \
//        localhost:8080/api/upload
//   curl localhost:8080/api/videos

#include "oreshnek/Oreshnek.h"
#include "oreshnek/platform/DatabaseManager.h"
#include "oreshnek/platform/SecurityUtils.h"
#include "oreshnek/server/Middleware.h"
#include "common.h"

#include <optional>
#include <string>
#include <vector>

using namespace Oreshnek;

namespace app {

// ---- Domain models (owned by the application, not the framework) -----------

struct User {
    long long id = 0;
    std::string username;
    std::string email;
    std::string password_hash;
    std::string role;
};

struct Video {
    long long id = 0;
    std::string title;
    std::string category;
    std::string filename;
    long long user_id = 0;
    long long views = 0;
    std::string created_at;
};

// ---- Repository: maps the generic SQL gateway onto the domain ----------------

class Repository {
public:
    explicit Repository(Platform::DatabaseManager& db) : db_(db) {}

    // App-owned schema migration. Uses portable SQL (`?` placeholders work on
    // both backends); only the column types would change for PostgreSQL.
    void migrate() {
        db_.exec(
            "CREATE TABLE IF NOT EXISTS users ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "username TEXT UNIQUE NOT NULL, "
            "email TEXT UNIQUE NOT NULL, "
            "password_hash TEXT NOT NULL, "
            "role TEXT NOT NULL DEFAULT 'viewer');");
        db_.exec(
            "CREATE TABLE IF NOT EXISTS videos ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "title TEXT NOT NULL, "
            "category TEXT, "
            "filename TEXT NOT NULL, "
            "user_id INTEGER NOT NULL, "
            "views INTEGER NOT NULL DEFAULT 0, "
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "FOREIGN KEY(user_id) REFERENCES users(id));");
    }

    std::optional<User> user_by_name(const std::string& username) {
        auto r = db_.query(
            "SELECT id, username, email, password_hash, role FROM users WHERE username = ?;",
            {username});
        if (!r.ok || r.empty()) return std::nullopt;
        return User{r.integer(0, 0), std::string(r.text(0, 1)), std::string(r.text(0, 2)),
                    std::string(r.text(0, 3)), std::string(r.text(0, 4))};
    }

    bool create_user(const User& u) {
        return db_
            .exec("INSERT INTO users (username, email, password_hash, role) VALUES (?, ?, ?, ?);",
                  {u.username, u.email, u.password_hash, u.role})
            .ok;
    }

    bool create_video(const Video& v) {
        return db_
            .exec("INSERT INTO videos (title, category, filename, user_id) VALUES (?, ?, ?, ?);",
                  {v.title, v.category, v.filename, std::to_string(v.user_id)})
            .ok;
    }

    std::vector<Video> videos(int limit, const std::string& category) {
        Platform::SqlParams params;
        std::string sql =
            "SELECT id, title, category, filename, user_id, views, created_at FROM videos";
        if (!category.empty()) {
            sql += " WHERE category = ?";
            params.push_back(category);
        }
        sql += " ORDER BY created_at DESC LIMIT ?;";
        params.push_back(std::to_string(limit));

        std::vector<Video> out;
        auto r = db_.query(sql, params);
        for (std::size_t i = 0; i < r.row_count(); ++i) {
            out.push_back(Video{r.integer(i, 0), std::string(r.text(i, 1)), std::string(r.text(i, 2)),
                                std::string(r.text(i, 3)), r.integer(i, 4), r.integer(i, 5),
                                std::string(r.text(i, 6))});
        }
        return out;
    }

    void increment_views(long long video_id) {
        db_.exec("UPDATE videos SET views = views + 1 WHERE id = ?;", {std::to_string(video_id)});
    }

private:
    Platform::DatabaseManager& db_;
};

}  // namespace app

int main() {
    Platform::ServerConfig cfg;
    cfg.db.backend = "sqlite";
    cfg.db.sqlite_path = "./video_platform.db";
    cfg.jwt_secret = "example-secret-change-me";

    Platform::DatabaseManager db(cfg);
    app::Repository repo(db);
    repo.migrate();

    Server::Server server(4);

    // Publishing requires a valid bearer token; everything else is public. The
    // built-in JWT middleware matches by path prefix, so the protected action
    // lives under its own path (/api/upload) and the public listing stays open.
    namespace MW = Server::Middlewares;
    server.use(MW::require_jwt(cfg.jwt_secret, {"/api/upload"}));

    // Register a user.
    server.post("/api/register", [&repo](const HttpRequest& req, HttpResponse& res) {
        nlohmann::json body;
        try { body = req.json(); } catch (...) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "invalid JSON"}});
            return;
        }
        if (!body.contains("username") || !body.contains("email") || !body.contains("password")) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "missing fields"}});
            return;
        }
        app::User u;
        u.username = body["username"].get<std::string>();
        u.email = body["email"].get<std::string>();
        u.role = body.value("role", "viewer");
        u.password_hash = Platform::SecurityUtils::hashPassword(body["password"].get<std::string>());

        if (repo.user_by_name(u.username)) {
            res.status(Http::HttpStatus::CONFLICT).json({{"error", "user exists"}});
            return;
        }
        const bool ok = repo.create_user(u);
        res.status(ok ? Http::HttpStatus::CREATED : Http::HttpStatus::INTERNAL_SERVER_ERROR)
           .json({{"created", ok}});
    });

    // Log in -> JWT.
    server.post("/api/login", [&repo, &cfg](const HttpRequest& req, HttpResponse& res) {
        nlohmann::json body;
        try { body = req.json(); } catch (...) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "invalid JSON"}});
            return;
        }
        auto u = repo.user_by_name(body.value("username", ""));
        if (!u ||
            !Platform::SecurityUtils::verifyPassword(body.value("password", ""), u->password_hash)) {
            res.status(Http::HttpStatus::UNAUTHORIZED).json({{"error", "invalid credentials"}});
            return;
        }
        res.status(Http::HttpStatus::OK).json(
            {{"token", Platform::SecurityUtils::generateJWT(
                           static_cast<int>(u->id), u->username, cfg.jwt_secret)}});
    });

    // Publish a video record (token-protected by the middleware above).
    server.post("/api/upload", [&repo](const HttpRequest& req, HttpResponse& res) {
        nlohmann::json body;
        try { body = req.json(); } catch (...) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "invalid JSON"}});
            return;
        }
        if (!body.contains("title") || !body.contains("filename")) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "missing fields"}});
            return;
        }
        app::Video v;
        v.title = body["title"].get<std::string>();
        v.filename = body["filename"].get<std::string>();
        v.category = body.value("category", "");
        v.user_id = body.value("user_id", 0);
        const bool ok = repo.create_video(v);
        res.status(ok ? Http::HttpStatus::CREATED : Http::HttpStatus::INTERNAL_SERVER_ERROR)
           .json({{"created", ok}});
    });

    // List videos (optionally by category).
    server.get("/api/videos", [&repo](const HttpRequest& req, HttpResponse& res) {
        int limit = 20;
        if (auto l = req.query("limit")) {
            try { limit = std::stoi(std::string(*l)); } catch (...) {}
        }
        std::string category = std::string(req.query("category").value_or(""));

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& v : repo.videos(limit, category)) {
            arr.push_back({{"id", v.id},
                           {"title", v.title},
                           {"category", v.category},
                           {"filename", v.filename},
                           {"views", v.views},
                           {"created_at", v.created_at}});
        }
        res.status(Http::HttpStatus::OK).json({{"videos", arr}});
    });

    // Record a view and return the video metadata.
    server.get("/api/videos/:id/watch", [&repo](const HttpRequest& req, HttpResponse& res) {
        long long id = 0;
        try { id = std::stoll(std::string(req.param("id").value_or("0"))); } catch (...) {}
        repo.increment_views(id);
        res.status(Http::HttpStatus::OK).json({{"watched", id}});
    });

    return ex::serve(server, "0.0.0.0", 8080);
}
