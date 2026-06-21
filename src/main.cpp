// src/main.cpp
//
// oreshnek_server: a small, *general-purpose* demo of the framework. It wires up
// configuration, logging, TLS/rate-limiting/metrics/compression and a couple of
// representative routes — a generic "notes" resource over the database gateway
// plus static file serving. It is intentionally domain-agnostic; for a worked
// application (a video platform built on these same primitives) see
// examples/06_video_platform.cpp.

#include "oreshnek/Oreshnek.h"
#include "oreshnek/server/Middleware.h"        // Built-in middlewares
#include "oreshnek/platform/Config.h"          // External configuration loader
#include "oreshnek/platform/DatabaseManager.h" // Generic SQL gateway
#include "oreshnek/utils/Logger.h"             // Structured logging

#include <csignal>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

// Global server instance for signal handling.
Oreshnek::Server::Server* g_server = nullptr;

void signal_handler(int /*signal*/) {
    // Async-signal-safe: only flips an atomic and writes to a pipe. The actual
    // teardown (stop()) runs on the main thread once run() returns.
    if (g_server) g_server->request_stop();
}

// Safely resolve a user-supplied relative path inside base_dir, rejecting any
// path that escapes it via "..", absolute paths or symlinks (directory
// traversal). Returns std::nullopt if the path escapes base_dir.
std::optional<std::string> resolve_within_dir(const std::string& base_dir,
                                              const std::string& relative) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::absolute(base_dir), ec);
    if (ec) return std::nullopt;
    fs::path target = fs::weakly_canonical(fs::absolute(base_dir) / relative, ec);
    if (ec) return std::nullopt;

    const std::string base_str = base.string();
    const std::string target_str = target.string();
    if (target_str.compare(0, base_str.size(), base_str) != 0) return std::nullopt;
    if (target_str.size() > base_str.size() &&
        target_str[base_str.size()] != static_cast<char>(fs::path::preferred_separator)) {
        return std::nullopt;
    }
    return target_str;
}

int main(int argc, char** argv) {
    // Writing to a socket whose peer has closed would otherwise raise SIGPIPE and
    // terminate the process; ignore it and rely on send()/EPIPE error handling.
    signal(SIGPIPE, SIG_IGN);
    try {
        // Load configuration: explicit path (argv[1]) > $ORESHNEK_CONFIG >
        // "./oreshnek.json". Secrets (JWT) can be supplied via env so they stay
        // out of the config file / VCS.
        std::string config_path = "oreshnek.json";
        if (argc > 1) {
            config_path = argv[1];
        } else if (const char* env_path = std::getenv("ORESHNEK_CONFIG")) {
            config_path = env_path;
        }
        Oreshnek::Platform::ServerConfig config = Oreshnek::Platform::Config::load(config_path);

        // Initialize structured logging from the configuration.
        auto& logger = Oreshnek::Utils::Logger::instance();
        logger.set_level(Oreshnek::Utils::level_from_string(config.log_level));
        if (!config.log_file.empty()) {
            if (!logger.set_file(config.log_file, config.log_max_bytes, config.log_max_files)) {
                ORE_LOG(WARN) << "Could not open log file '" << config.log_file
                              << "'; logging to stderr.";
            }
        }
        ORE_LOG(INFO) << "Oreshnek starting on " << config.host << ":" << config.port;

        std::filesystem::create_directories(config.static_dir);

        // The framework's database layer is a generic SQL gateway; the
        // application owns its schema. Here we keep a tiny "notes" table.
        Oreshnek::Platform::DatabaseManager db(config);
        db.exec(
            "CREATE TABLE IF NOT EXISTS notes ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "title TEXT NOT NULL, "
            "body TEXT NOT NULL DEFAULT '', "
            "created_at DATETIME DEFAULT CURRENT_TIMESTAMP);");

        Oreshnek::Server::Server server(config.thread_pool_size);
        server.configure(Oreshnek::Server::Server::Settings{
            config.read_timeout_sec, config.write_timeout_sec, config.idle_timeout_sec,
            config.shutdown_grace_sec, config.handler_timeout_sec});

        if (config.tls.enabled) {
            if (config.tls.cert_file.empty() || config.tls.key_file.empty()) {
                std::cerr << "TLS enabled but tls.cert_file/tls.key_file not set" << std::endl;
                return 1;
            }
            server.enable_tls(config.tls.cert_file, config.tls.key_file, config.tls.min_version);
        }
        if (config.rate_limit.enabled) {
            server.enable_rate_limit(config.rate_limit.requests_per_second, config.rate_limit.burst);
        }
        if (config.metrics.enabled) {
            server.enable_metrics(config.metrics.path);
        }
        if (config.compression.enabled) {
            server.enable_compression(config.compression.min_bytes, config.compression.brotli);
        }
        g_server = &server;

        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // --- Cross-cutting middleware (run before handlers, in this order) ---
        namespace MW = Oreshnek::Server::Middlewares;
        server.use(MW::request_logger());
        if (config.cors_enabled) {
            server.use(MW::cors(config.cors_allow_origin));
        }

        // --- Routes (generic) ---

        server.get("/", [](const Oreshnek::HttpRequest& /*req*/, Oreshnek::HttpResponse& res) {
            res.status(Oreshnek::Http::HttpStatus::OK).json(
                {{"name", "Oreshnek"}, {"message", "general-purpose C++20 web framework"}});
        });

        server.get("/health", [](const Oreshnek::HttpRequest& /*req*/, Oreshnek::HttpResponse& res) {
            res.status(Oreshnek::Http::HttpStatus::OK).json({{"status", "ok"}});
        });

        // Create a note.
        server.post("/api/notes", [&db](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            nlohmann::json body;
            try { body = req.json(); } catch (...) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json({{"error", "invalid JSON"}});
                return;
            }
            if (!body.contains("title")) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json({{"error", "missing title"}});
                return;
            }
            auto r = db.exec("INSERT INTO notes (title, body) VALUES (?, ?);",
                             {body["title"].get<std::string>(), body.value("body", "")});
            if (!r.ok) {
                res.status(Oreshnek::Http::HttpStatus::INTERNAL_SERVER_ERROR)
                   .json({{"error", "insert failed"}});
                return;
            }
            res.status(Oreshnek::Http::HttpStatus::CREATED).json({{"id", r.last_insert_id}});
        });

        // List notes.
        server.get("/api/notes", [&db](const Oreshnek::HttpRequest& /*req*/, Oreshnek::HttpResponse& res) {
            auto r = db.query("SELECT id, title, body, created_at FROM notes ORDER BY id DESC LIMIT 100;");
            nlohmann::json arr = nlohmann::json::array();
            for (std::size_t i = 0; i < r.row_count(); ++i) {
                arr.push_back({{"id", r.integer(i, 0)},
                               {"title", std::string(r.text(i, 1))},
                               {"body", std::string(r.text(i, 2))},
                               {"created_at", std::string(r.text(i, 3))}});
            }
            res.status(Oreshnek::Http::HttpStatus::OK).json({{"notes", arr}});
        });

        // Fetch one note by id.
        server.get("/api/notes/:id", [&db](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            auto r = db.query("SELECT id, title, body, created_at FROM notes WHERE id = ?;",
                              {std::string(req.param("id").value_or(""))});
            if (!r.ok || r.empty()) {
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND).json({{"error", "not found"}});
                return;
            }
            res.status(Oreshnek::Http::HttpStatus::OK).json(
                {{"id", r.integer(0, 0)},
                 {"title", std::string(r.text(0, 1))},
                 {"body", std::string(r.text(0, 2))},
                 {"created_at", std::string(r.text(0, 3))}});
        });

        // Serve static files (zero-copy sendfile + ETag/Last-Modified handled by
        // the framework). Path is canonicalized to prevent directory traversal.
        server.get("/static/:file_path", [&config](const Oreshnek::HttpRequest& req,
                                                    Oreshnek::HttpResponse& res) {
            std::optional<std::string_view> file_path_opt = req.param("file_path");
            if (!file_path_opt) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).text("Missing file path");
                return;
            }
            std::string relative_path = std::string(*file_path_opt);
            std::optional<std::string> resolved =
                resolve_within_dir(config.static_dir, relative_path);
            if (!resolved) {
                res.status(Oreshnek::Http::HttpStatus::FORBIDDEN).text("Forbidden");
                return;
            }
            if (!std::filesystem::exists(*resolved) || std::filesystem::is_directory(*resolved)) {
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND).text("File not found");
                return;
            }

            std::string content_type = "application/octet-stream";
            if (relative_path.ends_with(".css")) content_type = "text/css";
            else if (relative_path.ends_with(".js")) content_type = "application/javascript";
            else if (relative_path.ends_with(".png")) content_type = "image/png";
            else if (relative_path.ends_with(".jpg") || relative_path.ends_with(".jpeg")) content_type = "image/jpeg";
            else if (relative_path.ends_with(".html") || relative_path.ends_with(".htm")) content_type = "text/html";

            res.status(Oreshnek::Http::HttpStatus::OK).file(*resolved, content_type);
            // Let the browser cache static assets; the framework adds ETag/
            // Last-Modified so revalidation is a cheap 304.
            res.header("Cache-Control", "public, max-age=3600");
        });

        if (!server.listen(config.host, config.port)) {
            std::cerr << "Failed to start server" << std::endl;
            return 1;
        }

        server.run();   // blocks until request_stop()
        server.stop();  // graceful teardown on the main thread

    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
