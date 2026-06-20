// examples/04_static_files.cpp
//
// Use case: serving files from disk (zero-copy sendfile, Range and HEAD handled
// automatically by the framework).
// Customization points shown: HttpResponse::file(), safe path resolution to
// prevent directory traversal. Serves files from ./public.
//
//   mkdir -p public && echo hello > public/index.txt
//   curl localhost:8080/files/index.txt
//   curl -r 0-2 localhost:8080/files/index.txt        # 206 Partial Content
//   curl -I localhost:8080/files/index.txt            # HEAD

#include "oreshnek/Oreshnek.h"
#include "common.h"

#include <filesystem>
#include <optional>

using namespace Oreshnek;

namespace {
// Resolve `relative` inside `base_dir`, rejecting anything that escapes it via
// "..", absolute paths or symlinks. Returns nullopt on escape.
std::optional<std::string> resolve_within(const std::string& base_dir, const std::string& relative) {
    namespace fs = std::filesystem;
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

std::string content_type_for(const std::string& path) {
    if (path.ends_with(".html")) return "text/html";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".js")) return "application/javascript";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".txt")) return "text/plain";
    return "application/octet-stream";
}
}  // namespace

int main() {
    const std::string root = "./public";
    std::filesystem::create_directories(root);

    Server::Server server(4);

    server.get("/files/:path", [root](const HttpRequest& req, HttpResponse& res) {
        const std::string rel(req.param("path").value_or(""));
        auto resolved = resolve_within(root, rel);
        if (!resolved) {
            res.status(Http::HttpStatus::FORBIDDEN).text("forbidden");
            return;
        }
        if (!std::filesystem::exists(*resolved) || std::filesystem::is_directory(*resolved)) {
            res.status(Http::HttpStatus::NOT_FOUND).text("not found");
            return;
        }
        // The framework adds Accept-Ranges and turns a Range request into 206
        // (or 416), and suppresses the body for HEAD, automatically.
        res.status(Http::HttpStatus::OK).file(*resolved, content_type_for(*resolved));
    });

    return ex::serve(server, "0.0.0.0", 8080);
}
