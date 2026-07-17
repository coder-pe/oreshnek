// apps/fileapi/main.cpp
//
// fileapi: a small HTTPS file-storage API built on the Oreshnek framework.
// Receives files of any type and size (bodies above a threshold are streamed
// straight to disk, so uploads are bounded by disk, not RAM), stores them in a
// folder, and serves them back with range/resume support.
//
//   PUT    /files/:name   upload a file (raw body); returns {name, size}
//   POST   /files         upload; filename from the X-Filename header or generated
//   GET    /files         list stored files (JSON)
//   GET    /files/:name   download a file (sendfile + Range + ETag)
//   DELETE /files/:name   delete a file
//   GET    /health        liveness probe
//
// See README.md for TLS certificate generation and curl examples.

#include "oreshnek/Oreshnek.h"
#include "oreshnek/platform/Config.h"
#include "oreshnek/utils/Logger.h"
#include "oreshnek/utils/StringUtil.h"

#include <csignal>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using namespace Oreshnek;

Server::Server* g_server = nullptr;
void signal_handler(int) { if (g_server) g_server->request_stop(); }

namespace {

// Resolve `relative` inside `base_dir`, rejecting anything that escapes it via
// "..", absolute paths or symlinks (directory traversal). Returns nullopt on escape.
std::optional<std::string> resolve_within(const std::string& base_dir, const std::string& relative) {
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::absolute(base_dir), ec);
    if (ec) return std::nullopt;
    fs::path target = fs::weakly_canonical(fs::absolute(base_dir) / relative, ec);
    if (ec) return std::nullopt;
    const std::string b = base.string(), t = target.string();
    if (t.compare(0, b.size(), b) != 0) return std::nullopt;
    if (t.size() > b.size() && t[b.size()] != static_cast<char>(fs::path::preferred_separator))
        return std::nullopt;
    return t;
}

// Reduce a client-suggested name to a safe basename (no directories, no leading
// dot). Returns "" if nothing usable remains.
std::string safe_basename(std::string_view suggested) {
    fs::path p{std::string(suggested)};
    std::string name = p.filename().string();
    if (name.empty() || name == "." || name == ".." || name[0] == '.') return "";
    return name;
}

// Pick a non-colliding destination path under `dir` for basename `name`,
// appending -1, -2, ... to the stem on collision.
fs::path unique_dest(const fs::path& dir, const std::string& name) {
    fs::path candidate = dir / name;
    if (!fs::exists(candidate)) return candidate;
    const fs::path stem = fs::path(name).stem();
    const fs::path ext = fs::path(name).extension();
    for (int i = 1; i < 100000; ++i) {
        fs::path c = dir / (stem.string() + "-" + std::to_string(i) + ext.string());
        if (!fs::exists(c)) return c;
    }
    return dir / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + name);
}

std::string content_type_for(std::string_view path) {
    using Oreshnek::Utils::ends_with;
    if (ends_with(path, ".html") || ends_with(path, ".htm")) return "text/html";
    if (ends_with(path, ".css")) return "text/css";
    if (ends_with(path, ".js")) return "application/javascript";
    if (ends_with(path, ".json")) return "application/json";
    if (ends_with(path, ".txt")) return "text/plain";
    if (ends_with(path, ".png")) return "image/png";
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) return "image/jpeg";
    if (ends_with(path, ".gif")) return "image/gif";
    if (ends_with(path, ".pdf")) return "application/pdf";
    if (ends_with(path, ".mp4")) return "video/mp4";
    return "application/octet-stream";  // "files of any type"
}

// Store an uploaded body under storage_dir as `suggested` (sanitized/uniquified).
// Handles both the streamed path (req.body_file(): move the spool file into
// place) and the small buffered path (req.body(): write it out). On success sets
// stored_name + size and returns true.
bool store_upload(const Http::HttpRequest& req, const std::string& storage_dir,
                  const std::string& suggested, std::string& stored_name, std::uint64_t& size) {
    std::string base = safe_basename(suggested);
    if (base.empty()) {
        base = "upload-" + std::to_string(
                               std::chrono::system_clock::now().time_since_epoch().count());
    }
    fs::path dest = unique_dest(storage_dir, base);
    std::error_code ec;

    if (req.body_file()) {
        // Streamed to disk: move the spool file into place (rename is O(1) on the
        // same filesystem; fall back to copy for cross-device).
        fs::rename(*req.body_file(), dest, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(*req.body_file(), dest, fs::copy_options::overwrite_existing, ec);
            fs::remove(*req.body_file(), ec);
        }
        if (ec) return false;
    } else {
        // Small body buffered in memory: write it out.
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        std::string_view body = req.body();
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) return false;
    }
    size = fs::file_size(dest, ec);
    stored_name = dest.filename().string();
    return !ec;
}

}  // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    std::string config_path = "fileapi.json";
    if (argc > 1) config_path = argv[1];
    else if (const char* env = std::getenv("ORESHNEK_CONFIG")) config_path = env;
    Platform::ServerConfig config = Platform::Config::load(config_path);

    auto& logger = Utils::Logger::instance();
    logger.set_level(Utils::level_from_string(config.log_level));
    if (!config.log_file.empty()) logger.set_file(config.log_file, config.log_max_bytes,
                                                  config.log_max_files);

    const std::string storage_dir = config.upload_dir;  // final destination folder
    fs::create_directories(storage_dir);

    // HTTPS is required for this app. Fail fast with the exact command to make a
    // self-signed certificate if it is missing (we never shell out to openssl).
    if (!config.tls.enabled) {
        std::cerr << "fileapi requires HTTPS. Set tls.enabled=true in " << config_path << ".\n";
        return 1;
    }
    if (config.tls.cert_file.empty() || config.tls.key_file.empty() ||
        !fs::exists(config.tls.cert_file) || !fs::exists(config.tls.key_file)) {
        std::cerr << "TLS certificate/key not found (cert='" << config.tls.cert_file
                  << "', key='" << config.tls.key_file << "').\n"
                  << "Generate a self-signed pair with:\n"
                  << "  ./tools/gen-selfsigned-cert.sh\n";
        return 1;
    }

    Server::Server server(static_cast<size_t>(config.thread_pool_size));
    server.configure(Server::Server::Settings{
        config.read_timeout_sec, config.write_timeout_sec, config.idle_timeout_sec,
        config.shutdown_grace_sec, config.handler_timeout_sec, config.max_concurrent_handlers});
    server.enable_tls(config.tls.cert_file, config.tls.key_file, config.tls.min_version);

    // Stream large uploads straight to disk (this is the whole point of the app),
    // using the configured spool dir / threshold / cap.
    server.enable_upload_streaming(config.upload.spool_dir, config.upload.max_upload_bytes,
                                   config.upload.stream_threshold_bytes);

    if (config.metrics.enabled) server.enable_metrics(config.metrics.path);

    g_server = &server;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // ---- Routes -------------------------------------------------------------

    server.get("/", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).json(
            {{"name", "fileapi"}, {"message", "HTTPS file storage on Oreshnek"}});
    });
    server.get("/health", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).json({{"status", "ok"}});
    });

    // Upload via PUT /files/:name (name suggested by the client).
    server.put("/files/:name", [&storage_dir](const Http::HttpRequest& req, Http::HttpResponse& res) {
        std::string stored;
        std::uint64_t size = 0;
        if (!store_upload(req, storage_dir, std::string(req.param("name").value_or("")), stored, size)) {
            res.status(Http::HttpStatus::INTERNAL_SERVER_ERROR).json({{"error", "store failed"}});
            return;
        }
        res.status(Http::HttpStatus::CREATED).json(
            {{"name", stored}, {"size", static_cast<long long>(size)}, {"url", "/files/" + stored}});
    });

    // Upload via POST /files (filename from X-Filename, else generated).
    server.post("/files", [&storage_dir](const Http::HttpRequest& req, Http::HttpResponse& res) {
        std::string suggested = std::string(req.header("X-Filename").value_or(""));
        std::string stored;
        std::uint64_t size = 0;
        if (!store_upload(req, storage_dir, suggested, stored, size)) {
            res.status(Http::HttpStatus::INTERNAL_SERVER_ERROR).json({{"error", "store failed"}});
            return;
        }
        res.status(Http::HttpStatus::CREATED).json(
            {{"name", stored}, {"size", static_cast<long long>(size)}, {"url", "/files/" + stored}});
    });

    // List stored files.
    server.get("/files", [&storage_dir](const Http::HttpRequest&, Http::HttpResponse& res) {
        nlohmann::json arr = nlohmann::json::array();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(storage_dir, ec)) {
            if (!e.is_regular_file()) continue;
            arr.push_back({{"name", e.path().filename().string()},
                           {"size", static_cast<long long>(e.file_size())}});
        }
        res.status(Http::HttpStatus::OK).json({{"files", arr}});
    });

    // Download a file (zero-copy sendfile + Range + ETag handled by the framework).
    server.get("/files/:name", [&storage_dir](const Http::HttpRequest& req, Http::HttpResponse& res) {
        std::string name(req.param("name").value_or(""));
        std::optional<std::string> resolved = resolve_within(storage_dir, name);
        if (!resolved || !fs::exists(*resolved) || fs::is_directory(*resolved)) {
            res.status(Http::HttpStatus::NOT_FOUND).json({{"error", "not found"}});
            return;
        }
        res.status(Http::HttpStatus::OK).file(*resolved, content_type_for(*resolved));
    });

    // Delete a file.
    server.del("/files/:name", [&storage_dir](const Http::HttpRequest& req, Http::HttpResponse& res) {
        std::string name(req.param("name").value_or(""));
        std::optional<std::string> resolved = resolve_within(storage_dir, name);
        std::error_code ec;
        if (!resolved || !fs::exists(*resolved) || fs::is_directory(*resolved)) {
            res.status(Http::HttpStatus::NOT_FOUND).json({{"error", "not found"}});
            return;
        }
        fs::remove(*resolved, ec);
        if (ec) { res.status(Http::HttpStatus::INTERNAL_SERVER_ERROR).json({{"error", "delete failed"}}); return; }
        res.status(Http::HttpStatus::OK).json({{"deleted", name}});
    });

    ORE_LOG(INFO) << "fileapi listening on https://" << config.host << ":" << config.port
                  << " (storage '" << storage_dir << "')";
    if (!server.listen(config.host, config.port)) {
        std::cerr << "Failed to start server\n";
        return 1;
    }
    server.run();
    server.stop();
    return 0;
}
