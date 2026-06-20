// src/main.cpp
#include "oreshnek/Oreshnek.h" // Include the convenience header
#include "oreshnek/http/Multipart.h" // Multipart/form-data parser
#include "oreshnek/server/Middleware.h"        // Built-in middlewares
#include "oreshnek/platform/Config.h"          // External configuration loader
#include "oreshnek/platform/DatabaseManager.h" // Include your DatabaseManager
#include "oreshnek/platform/SecurityUtils.h"   // Include your SecurityUtils
#include "oreshnek/utils/Logger.h"             // Structured logging

#include <iostream>
#include <signal.h>
#include <cstdlib> // For std::getenv
#include <ctime> // For std::time
#include <filesystem> // For creating directories
#include <fstream> // For file operations
#include <random> // For file naming
#include <optional> // For std::optional

// Global server instance for signal handling
Oreshnek::Server::Server* g_server = nullptr;
Oreshnek::Platform::DatabaseManager* g_db_manager = nullptr; // Global for easy access in handlers
Oreshnek::Platform::ServerConfig g_server_config; // Global for config access

void signal_handler(int /*signal*/) {
    // Async-signal-safe: only flips an atomic and writes to a pipe. The actual
    // teardown (stop()) runs on the main thread once run() returns.
    if (g_server) {
        g_server->request_stop();
    }
}

// Utility to read file content into a string
std::string read_file_content(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Could not open file: " << file_path << std::endl;
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Parse a multipart/form-data body using the framework parser. Text fields are
// returned by name; file parts are saved to the upload directory and recorded
// under "<name>_filename". The original filename is reduced to its basename to
// avoid directory traversal via the upload name.
std::unordered_map<std::string, std::string> parse_multipart_form_data(const std::string_view& body, const std::string_view& content_type_header) {
    std::unordered_map<std::string, std::string> parsed_data;

    std::string boundary = Oreshnek::Http::Multipart::boundary_from_content_type(content_type_header);
    if (boundary.empty()) {
        std::cerr << "Multipart: boundary not found in Content-Type." << std::endl;
        return parsed_data;
    }

    for (const auto& part : Oreshnek::Http::Multipart::parse(body, boundary)) {
        if (part.is_file()) {
            std::string filename(part.filename);
            size_t slash = filename.find_last_of("/\\");
            if (slash != std::string::npos) filename = filename.substr(slash + 1);
            if (filename.empty()) continue;

            std::string unique_filename = std::to_string(std::time(nullptr)) + "_" + filename;
            std::string file_path = g_server_config.upload_dir + unique_filename;
            std::ofstream ofs(file_path, std::ios::binary);
            if (ofs.is_open()) {
                ofs.write(part.content.data(), static_cast<std::streamsize>(part.content.size()));
                parsed_data[std::string(part.name) + "_filename"] = unique_filename;
            } else {
                std::cerr << "Failed to write uploaded file: " << file_path << std::endl;
            }
        } else {
            parsed_data[std::string(part.name)] = std::string(part.content);
        }
    }
    return parsed_data;
}


// URL Decode utility (from your platform_video_streaming.txt)
std::string url_decode(const std::string& str) {
    std::string decoded; //
    for(size_t i = 0; i < str.length(); ++i) {
        if(str[i] == '%' && i + 2 < str.length()) {
            int hex_value; //
            std::istringstream hex_stream(str.substr(i + 1, 2));
            if(hex_stream >> std::hex >> hex_value) {
                decoded += static_cast<char>(hex_value); //
                i += 2;
            } else {
                decoded += str[i]; //
            }
        } else if(str[i] == '+') {
            decoded += ' '; //
        } else {
            decoded += str[i]; //
        }
    }
    return decoded;
}

// Function to parse x-www-form-urlencoded (from your platform_video_streaming.txt)
std::unordered_map<std::string, std::string> parse_form_urlencoded(const std::string_view& data) {
    std::unordered_map<std::string, std::string> params;
    std::string temp_data(data); // Convert string_view to string for stringstream
    std::istringstream stream(temp_data); //
    std::string pair;
    
    while(std::getline(stream, pair, '&')) { //
        size_t eq_pos = pair.find('=');
        if(eq_pos != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq_pos)); //
            std::string value = url_decode(pair.substr(eq_pos + 1));
            params[key] = value;
        }
    }
    return params;
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
    // target must be base itself or live strictly under base/ (with a separator
    // at the boundary, so "/static" does not match "/staticX").
    if (target_str.compare(0, base_str.size(), base_str) != 0) {
        return std::nullopt;
    }
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
        g_server_config = Oreshnek::Platform::Config::load(config_path);

        // Initialize structured logging from the configuration.
        auto& logger = Oreshnek::Utils::Logger::instance();
        logger.set_level(Oreshnek::Utils::level_from_string(g_server_config.log_level));
        if (!g_server_config.log_file.empty()) {
            if (!logger.set_file(g_server_config.log_file, g_server_config.log_max_bytes,
                                 g_server_config.log_max_files)) {
                ORE_LOG(WARN) << "Could not open log file '" << g_server_config.log_file
                              << "'; logging to stderr.";
            }
        }
        ORE_LOG(INFO) << "Oreshnek starting on " << g_server_config.host << ":"
                      << g_server_config.port;

        // Create necessary directories
        std::filesystem::create_directories(g_server_config.upload_dir); //
        std::filesystem::create_directories(g_server_config.static_dir); //


        // Create DatabaseManager instance. The concrete backend (SQLite or
        // PostgreSQL) is selected from the configuration.
        Oreshnek::Platform::DatabaseManager db_manager(g_server_config);
        g_db_manager = &db_manager; // Set global pointer

        // Create server instance
        Oreshnek::Server::Server server(g_server_config.thread_pool_size); //
        server.configure(Oreshnek::Server::Server::Settings{
            g_server_config.read_timeout_sec,
            g_server_config.write_timeout_sec,
            g_server_config.idle_timeout_sec,
            g_server_config.shutdown_grace_sec});

        // Enable HTTPS if configured (throws on an invalid certificate/key).
        if (g_server_config.tls.enabled) {
            if (g_server_config.tls.cert_file.empty() || g_server_config.tls.key_file.empty()) {
                std::cerr << "TLS enabled but tls.cert_file/tls.key_file not set" << std::endl;
                return 1;
            }
            server.enable_tls(g_server_config.tls.cert_file, g_server_config.tls.key_file,
                              g_server_config.tls.min_version);
        }

        // Enable per-IP rate limiting if configured.
        if (g_server_config.rate_limit.enabled) {
            server.enable_rate_limit(g_server_config.rate_limit.requests_per_second,
                                     g_server_config.rate_limit.burst);
        }

        // Expose Prometheus metrics if configured.
        if (g_server_config.metrics.enabled) {
            server.enable_metrics(g_server_config.metrics.path);
        }
        g_server = &server; // Set global pointer for signal handling

        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // --- Cross-cutting middleware (run before handlers, in this order) ---
        namespace MW = Oreshnek::Server::Middlewares;
        server.use(MW::request_logger());
        if (g_server_config.cors_enabled) {
            server.use(MW::cors(g_server_config.cors_allow_origin));
        }
        // Reject uploads without a valid bearer token before reaching the handler.
        server.use(MW::require_jwt(g_server_config.jwt_secret, {"/api/upload"}));

        // --- Define API routes for the Video Streaming Platform ---

        // Serve static files (e.g., HTML, CSS, JS)
        server.get("/static/:file_path", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            std::optional<std::string_view> file_path_opt = req.param("file_path");
            if (!file_path_opt) {
                std::cerr << "DEBUG: Missing file path for static request.\n";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).text("Missing file path");
                return;
            }
            std::string relative_path = std::string(*file_path_opt);
            std::optional<std::string> resolved =
                resolve_within_dir(g_server_config.static_dir, relative_path);
            if (!resolved) {
                res.status(Oreshnek::Http::HttpStatus::FORBIDDEN).text("Forbidden");
                return;
            }
            std::string file_path = *resolved;

            if (!std::filesystem::exists(file_path)) {
                std::cerr << "DEBUG: Static file not found: " << file_path << "\n";
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND).text("File not found");
                return;
            }
            if (std::filesystem::is_directory(file_path)) {
                std::cerr << "DEBUG: Static file is a directory: " << file_path << "\n";
                res.status(Oreshnek::Http::HttpStatus::FORBIDDEN).text("Cannot serve directory"); // Or NOT_FOUND
                return;
            }

            std::string content_type = "application/octet-stream";
            if (relative_path.ends_with(".css")) content_type = "text/css";
            else if (relative_path.ends_with(".js")) content_type = "application/javascript";
            else if (relative_path.ends_with(".png")) content_type = "image/png";
            else if (relative_path.ends_with(".jpg") || relative_path.ends_with(".jpeg")) content_type = "image/jpeg";
            else if (relative_path.ends_with(".html") || relative_path.ends_with(".htm")) content_type = "text/html";

            std::cerr << "DEBUG: Serving static file: " << file_path << " with Content-Type: " << content_type << "\n";
            res.status(Oreshnek::Http::HttpStatus::OK).file(file_path, content_type); // This sets the response to be a file
        });

        // Serve home page
        server.get("/", [](const Oreshnek::HttpRequest& /*req*/, Oreshnek::HttpResponse& res) {
            std::string html_content = read_file_content(g_server_config.static_dir + "index.html");
            if (html_content.empty()) {
                res.status(Oreshnek::Http::HttpStatus::INTERNAL_SERVER_ERROR).text("Could not load index.html");
                return;
            }
            res.status(Oreshnek::Http::HttpStatus::OK).html(html_content);
        });

        // Register User
        server.post("/api/register", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); //
            
            if (req.body().empty()) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Empty body";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            auto form_data = parse_form_urlencoded(req.body());

            auto username_it = form_data.find("username");
            auto email_it = form_data.find("email");
            auto password_it = form_data.find("password");
            auto role_it = form_data.find("role");

            if (username_it == form_data.end() || email_it == form_data.end() || password_it == form_data.end()) { //
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Missing required fields";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            // Basic validation (can be enhanced significantly)
            if (username_it->second.empty() || email_it->second.empty() || password_it->second.empty()) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Fields cannot be empty";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }
            // Email format and uniqueness validation
            // Password strength validation

            Oreshnek::Platform::User new_user; //
            new_user.username = username_it->second;
            new_user.email = email_it->second;
            new_user.role = (role_it != form_data.end()) ? role_it->second : "student";

            // Check if user already exists
            if (g_db_manager->getUserByUsername(new_user.username).id != 0) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "User already exists";
                res.status(Oreshnek::Http::HttpStatus::CONFLICT).json(error_json);
                return;
            }

            // PBKDF2 hash with a per-user random salt embedded in the stored string.
            new_user.password_hash = Oreshnek::Platform::SecurityUtils::hashPassword(password_it->second);

            bool success = g_db_manager->createUser(new_user);

            nlohmann::json response_json = nlohmann::json::object();
            response_json["success"] = success;
            response_json["message"] = success ? "User registered successfully" : "Error creating user"; //
            res.json(response_json); //
        });

        // Login User
        server.post("/api/login", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); //

            if (req.body().empty()) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Empty body";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            auto form_data = parse_form_urlencoded(req.body());
            auto username_it = form_data.find("username");
            auto password_it = form_data.find("password");

            if (username_it == form_data.end() || password_it == form_data.end()) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Missing required fields";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            Oreshnek::Platform::User user = g_db_manager->getUserByUsername(username_it->second); //
            if (user.id == 0) { // User not found
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "User not found";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            if (!Oreshnek::Platform::SecurityUtils::verifyPassword(password_it->second, user.password_hash)) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Incorrect password";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            std::string token = Oreshnek::Platform::SecurityUtils::generateJWT(user.id, user.username, g_server_config.jwt_secret); //

            nlohmann::json success_response = nlohmann::json::object();
            success_response["success"] = true;
            success_response["token"] = nlohmann::json(token);
            nlohmann::json user_json = nlohmann::json::object();
            user_json["id"] = nlohmann::json(user.id);
            user_json["username"] = nlohmann::json(user.username);
            user_json["role"] = nlohmann::json(user.role);
            success_response["user"] = user_json;
            
            res.json(success_response); //
        });

        // Upload Video
        server.post("/api/upload", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); //

            auto auth_header = req.header("Authorization"); //
            if (!auth_header) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Authentication token required";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            std::string token = std::string(*auth_header); //
            if (token.length() > 7 && token.substr(0, 7) == "Bearer ") { //
                token = token.substr(7); //
            } else {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Invalid Authorization header format";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }
            
            if (!Oreshnek::Platform::SecurityUtils::validateJWT(token, g_server_config.jwt_secret)) { //
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Invalid token";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            // Extract user_id from JWT payload
            nlohmann::json jwt_payload = Oreshnek::Platform::SecurityUtils::decodeJWT(token);
            if (jwt_payload.is_null() || !jwt_payload.is_object()) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Invalid token payload";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }
            
            // Check if user_id exists in the payload
            int user_id;
            try {
                user_id = jwt_payload["user_id"].get<int>();
            } catch (const std::exception& e) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Invalid token payload - missing user_id";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            // Simplified multipart/form-data parsing
            auto content_type_header_opt = req.header("Content-Type");
            if (!content_type_header_opt || content_type_header_opt->find("multipart/form-data") == std::string_view::npos) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Content-Type must be multipart/form-data";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            // --- DEBUG PRINT: Print raw request body for upload ---
            std::cerr << "DEBUG: /api/upload raw body size: " << req.body().length() << " bytes.\n";
            if (req.body().length() > 0) {
                std::cerr << "DEBUG: /api/upload raw body (first 512 bytes): " << req.body().substr(0, std::min(req.body().length(), (size_t)512)) << "...\n";
            } else {
                std::cerr << "DEBUG: /api/upload raw body is empty.\n";
            }
            // --- END DEBUG PRINT ---

            std::unordered_map<std::string, std::string> form_data = parse_multipart_form_data(req.body(), *content_type_header_opt);

            // Access fields
            std::string title = form_data["title"];
            std::string description = form_data["description"];
            std::string category = form_data["category"];
            std::string tags_str = form_data["tags"];
            std::string filename_in_uploads = form_data["video_filename"]; // filename from parse_multipart_form_data

            if (title.empty() || filename_in_uploads.empty()) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Missing title or video file (from parsed form data).";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            Oreshnek::Platform::Video new_video;
            new_video.title = title;
            new_video.description = description;
            new_video.category = category;
            new_video.filename = filename_in_uploads;
            new_video.user_id = user_id;
            new_video.duration = "00:00"; // Placeholder, processing would set this

            std::stringstream ss(tags_str);
            std::string tag;
            while(std::getline(ss, tag, ',')) {
                new_video.tags.push_back(tag);
            }
            
            bool success = g_db_manager->createVideo(new_video);

            nlohmann::json response_json = nlohmann::json::object();
            response_json["success"] = success; //
            response_json["message"] = success ? "Video uploaded successfully" : "Error uploading video";
            res.json(response_json); //
        });

        // Get Video List
        server.get("/api/videos", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); //

            int limit = 20;
            int offset = 0;
            std::string category = "";

            auto limit_opt = req.query("limit");
            if (limit_opt) {
                try { limit = std::stoi(std::string(*limit_opt)); } catch (...) {} //
            }
            auto offset_opt = req.query("offset");
            if (offset_opt) {
                try { offset = std::stoi(std::string(*offset_opt)); } catch (...) {}
            }
            auto category_opt = req.query("category");
            if (category_opt) {
                category = std::string(*category_opt); //
            }
            
            std::vector<Oreshnek::Platform::Video> videos = g_db_manager->getVideos(limit, offset, category); //
            nlohmann::json response_json = nlohmann::json::object();
            response_json["success"] = true;
            response_json["videos"] = nlohmann::json::array();
            
            for(const auto& video : videos) { //
                nlohmann::json video_json = nlohmann::json::object();
                video_json["id"] = video.id;
                video_json["title"] = nlohmann::json(video.title);
                video_json["description"] = nlohmann::json(video.description);
                video_json["category"] = nlohmann::json(video.category);
                nlohmann::json tags_array = nlohmann::json::array();
                for (const auto& tag : video.tags) {
                    tags_array.push_back(tag);
                }
                video_json["tags"] = tags_array;
                video_json["views"] = video.views; //
                video_json["likes"] = video.likes;
                video_json["created_at"] = nlohmann::json(video.created_at);
                video_json["duration"] = nlohmann::json(video.duration);
                response_json["videos"].push_back(video_json);
            }
            
            // --- DEBUG PRINT: Print the generated JSON to stderr for inspection ---
            std::cerr << "DEBUG: Sending /api/videos response: " << response_json.dump() << std::endl;
            // --- END DEBUG PRINT ---

            res.json(response_json); //
        });

        // Serve a specific video file (e.g., for direct playback)
        server.get("/video/:filename", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            std::optional<std::string_view> filename_opt = req.param("filename");
            if (!filename_opt) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).text("Missing video filename");
                return;
            }
            std::optional<std::string> resolved_video =
                resolve_within_dir(g_server_config.upload_dir, std::string(*filename_opt));
            if (!resolved_video) {
                res.status(Oreshnek::Http::HttpStatus::FORBIDDEN).text("Forbidden");
                return;
            }
            std::string video_path = *resolved_video;

            if (!std::filesystem::exists(video_path) || std::filesystem::is_directory(video_path)) {
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND).text("Video not found"); //
                return;
            }
            
            res.status(Oreshnek::Http::HttpStatus::OK).file(video_path, "video/mp4"); //
            res.header("Accept-Ranges", "bytes"); // Crucial for video streaming
            // Note: Range request handling needs to be implemented in Connection::write_data
        });

        // Serve video player page
        server.get("/watch", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            std::string html_content = read_file_content(g_server_config.static_dir + "watch.html");
            if (html_content.empty()) {
                res.status(Oreshnek::Http::HttpStatus::INTERNAL_SERVER_ERROR).text("Could not load watch.html");
                return;
            }

            res.header("Content-Type", "text/html; charset=utf-8"); //

            std::optional<std::string_view> id_opt = req.query("id");
            if (!id_opt) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .html("<h1>400 - Bad Request: Missing video ID</h1>"); //
                return;
            }

            int video_id = 0;
            try {
                video_id = std::stoi(std::string(*id_opt)); //
            } catch (const std::exception& e) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .html("<h1>400 - Bad Request: Invalid video ID format</h1>");
                return;
            }
            
            // Increment view count
            g_db_manager->incrementViews(video_id);

            // Fetch video details to populate the player page
            // (You'll need a get_video_by_id in DatabaseManager)
            // For now, let's just use the ID in the HTML
            
            res.status(Oreshnek::Http::HttpStatus::OK).html(html_content);
        });

        // API to get video details (for the player page to fetch dynamically)
        server.get("/api/video_details/:id", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json");

            std::optional<std::string_view> id_opt = req.param("id");
            if (!id_opt) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Missing video ID";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            int video_id = 0;
            try {
                video_id = std::stoi(std::string(*id_opt));
            } catch (const std::exception& e) {
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Invalid video ID format";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }
            
            // You need to implement a get_video_by_id in your DatabaseManager
            // For now, let's use a dummy video or retrieve it from getVideos
            // For a real app: Video video = g_db_manager->getVideoById(video_id);
            Oreshnek::Platform::Video found_video;
            std::vector<Oreshnek::Platform::Video> all_videos = g_db_manager->getVideos(-1); // Get all or implement proper search
            for (const auto& vid : all_videos) {
                if (vid.id == video_id) {
                    found_video = vid;
                    break;
                }
            }

            if (found_video.id == 0) { // Video not found
                nlohmann::json error_json = nlohmann::json::object();
                error_json["success"] = false;
                error_json["message"] = "Video not found";
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND).json(error_json);
                return;
            }

            nlohmann::json response_json = nlohmann::json::object();
            response_json["success"] = true;
            nlohmann::json video_json = nlohmann::json::object();
            video_json["id"] = found_video.id;
            video_json["title"] = nlohmann::json(found_video.title);
            video_json["description"] = nlohmann::json(found_video.description);
            video_json["filename"] = nlohmann::json(found_video.filename); // Needed for video player source
            video_json["views"] = found_video.views;
            video_json["likes"] = found_video.likes;
            video_json["created_at"] = nlohmann::json(found_video.created_at);
            video_json["duration"] = nlohmann::json(found_video.duration);
            response_json["video"] = video_json;
            
            res.json(response_json);
        });


        // Start server
        if (!server.listen(g_server_config.host, g_server_config.port)) { // Use config for host/port
            std::cerr << "Failed to start server" << std::endl;
            return 1;
        }

        // Run server (blocking call); returns after request_stop().
        server.run();
        server.stop(); // Graceful teardown on the main thread.

    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
