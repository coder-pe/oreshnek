// src/main.cpp
#include "oreshnek/Oreshnek.h" // Include the convenience header
#include "oreshnek/platform/DatabaseManager.h" // Include your DatabaseManager
#include "oreshnek/platform/SecurityUtils.h"   // Include your SecurityUtils

#include <iostream>
#include <signal.h>
#include <ctime> // For std::time
#include <filesystem> // For creating directories
#include <fstream> // For file operations
#include <random> // For file naming

// Global server instance for signal handling
Oreshnek::Server::Server* g_server = nullptr;
Oreshnek::Platform::DatabaseManager* g_db_manager = nullptr; // Global for easy access in handlers
Oreshnek::Platform::ServerConfig g_server_config; // Global for config access

void signal_handler(int signal) {
    if (g_server) {
        std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
        g_server->stop();
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

// Utility to parse multipart/form-data (simplified, needs robust implementation)
// This is a *very basic* placeholder and will likely fail for real-world multipart data.
// A proper implementation requires parsing boundaries, content-disposition, etc.
// For the purpose of getting the server to compile and run with the concept, this is here.
std::unordered_map<std::string, std::string> parse_multipart_form_data(const std::string_view& body, const std::string_view& content_type_header) {
    std::unordered_map<std::string, std::string> parsed_data;
    // Find boundary
    size_t boundary_pos = content_type_header.find("boundary=");
    if (boundary_pos == std::string_view::npos) {
        std::cerr << "Multipart: Boundary not found in Content-Type." << std::endl;
        return parsed_data;
    }
    std::string boundary = "--" + std::string(content_type_header.substr(boundary_pos + 9));
    
    // Very simplified parsing logic
    size_t current_pos = 0;
    while (current_pos < body.length()) {
        size_t part_start = body.find(boundary, current_pos);
        if (part_start == std::string_view::npos) break;
        part_start += boundary.length();
        
        size_t part_end = body.find(boundary, part_start);
        if (part_end == std::string_view::npos) break;

        std::string_view part_content = body.substr(part_start, part_end - part_start);

        // Find Content-Disposition
        size_t cd_pos = part_content.find("Content-Disposition:");
        if (cd_pos == std::string_view::npos) continue;
        size_t cd_end = part_content.find("\r\n", cd_pos);
        if (cd_end == std::string_view::npos) continue;
        std::string_view cd_header = part_content.substr(cd_pos, cd_end - cd_pos);

        // Extract name and filename
        std::string name;
        size_t name_pos = cd_header.find("name=\"");
        if (name_pos != std::string_view::npos) {
            name_pos += 6;
            size_t name_end = cd_header.find("\"", name_pos);
            if (name_end != std::string_view::npos) {
                name = std::string(cd_header.substr(name_pos, name_end - name_pos));
            }
        }
        
        std::string filename;
        size_t filename_pos = cd_header.find("filename=\"");
        if (filename_pos != std::string_view::npos) {
            filename_pos += 10;
            size_t filename_end = cd_header.find("\"", filename_pos);
            if (filename_end != std::string_view::npos) {
                filename = std::string(cd_header.substr(filename_pos, filename_end - filename_pos));
            }
        }

        size_t headers_end = part_content.find("\r\n\r\n");
        if (headers_end == std::string_view::npos) continue;
        headers_end += 4; // Skip CRLFCRLF

        std::string_view value_content = part_content.substr(headers_end);
        
        // Remove trailing CRLF
        if (!value_content.empty() && value_content.back() == '\n') value_content.remove_suffix(1);
        if (!value_content.empty() && value_content.back() == '\r') value_content.remove_suffix(1);

        if (!name.empty()) {
            if (!filename.empty()) {
                // This is a file, handle saving it
                std::string unique_filename = std::to_string(std::time(nullptr)) + "_" + filename;
                std::string file_path = g_server_config.upload_dir + unique_filename;
                std::ofstream ofs(file_path, std::ios::binary);
                if (ofs.is_open()) {
                    ofs.write(value_content.data(), value_content.length());
                    ofs.close();
                    parsed_data[name + "_filename"] = unique_filename; // Store filename for DB
                } else {
                    std::cerr << "Failed to write file: " << file_path << std::endl;
                }
            } else {
                parsed_data[name] = std::string(value_content);
            }
        }

        current_pos = part_end;
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


int main() {
    try {
        // Initialize ServerConfig
        // You can load this from a file later (e.g., JSON, TOML)
        g_server_config.port = 8080;
        g_server_config.jwt_secret = "my-super-secret-jwt-key-for-oreshnek-platform-tutorial-streaming"; //
        g_server_config.upload_dir = "./uploads/";
        g_server_config.static_dir = "./static/";
        g_server_config.db_path = "./database.db";

        // Create necessary directories
        std::filesystem::create_directories(g_server_config.upload_dir); //
        std::filesystem::create_directories(g_server_config.static_dir); //


        // Create DatabaseManager instance
        Oreshnek::Platform::DatabaseManager db_manager(g_server_config.db_path); //
        g_db_manager = &db_manager; // Set global pointer

        // Create server instance
        Oreshnek::Server::Server server(g_server_config.thread_pool_size); //
        g_server = &server; // Set global pointer for signal handling

        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

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
            std::string file_path = g_server_config.static_dir + relative_path;
            std::cerr << "DEBUG: Request for static file: " << file_path << "\n";

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
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
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
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Missing required fields";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            // Basic validation (can be enhanced significantly)
            if (username_it->second.empty() || email_it->second.empty() || password_it->second.empty()) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
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
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "User already exists";
                res.status(Oreshnek::Http::HttpStatus::CONFLICT).json(error_json);
                return;
            }

            std::string salt = "default_salt"; // Replace with dynamically generated salt in a real app
            new_user.password_hash = Oreshnek::Platform::SecurityUtils::hashPassword(password_it->second, salt);

            bool success = g_db_manager->createUser(new_user);

            Oreshnek::JsonValue response_json = Oreshnek::JsonValue::object();
            response_json["success"] = success;
            response_json["message"] = success ? "User registered successfully" : "Error creating user"; //
            res.json(response_json); //
        });

        // Login User
        server.post("/api/login", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); //

            if (req.body().empty()) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Empty body";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            auto form_data = parse_form_urlencoded(req.body());
            auto username_it = form_data.find("username");
            auto password_it = form_data.find("password");

            if (username_it == form_data.end() || password_it == form_data.end()) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Missing required fields";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            Oreshnek::Platform::User user = g_db_manager->getUserByUsername(username_it->second); //
            if (user.id == 0) { // User not found
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "User not found";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            // In a real implementation, store salt with user and retrieve it here.
            std::string salt = "default_salt"; //
            std::string password_hash = Oreshnek::Platform::SecurityUtils::hashPassword(password_it->second, salt); //

            if (password_hash != user.password_hash) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Incorrect password";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            std::string token = Oreshnek::Platform::SecurityUtils::generateJWT(user.id, user.username, g_server_config.jwt_secret); //

            Oreshnek::JsonValue success_response = Oreshnek::JsonValue::object();
            success_response["success"] = true;
            success_response["token"] = Oreshnek::JsonValue(token);
            Oreshnek::JsonValue user_json = Oreshnek::JsonValue::object();
            user_json["id"] = Oreshnek::JsonValue(user.id);
            user_json["username"] = Oreshnek::JsonValue(user.username);
            user_json["role"] = Oreshnek::JsonValue(user.role);
            success_response["user"] = user_json;
            
            res.json(success_response); //
        });

        // Upload Video
        server.post("/api/upload", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); //

            auto auth_header = req.header("Authorization"); //
            if (!auth_header) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Authentication token required";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            std::string token = std::string(*auth_header); //
            if (token.length() > 7 && token.substr(0, 7) == "Bearer ") { //
                token = token.substr(7); //
            } else {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Invalid Authorization header format";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }
            
            if (!Oreshnek::Platform::SecurityUtils::validateJWT(token, g_server_config.jwt_secret)) { //
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Invalid token";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }

            // Extract user_id from JWT payload
            nlohmann::json jwt_payload = Oreshnek::Platform::SecurityUtils::decodeJWT(token);
            if (jwt_payload.is_null() || !jwt_payload.contains("user_id")) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Invalid token payload";
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED).json(error_json);
                return;
            }
            int user_id = jwt_payload["user_id"].get<int>();

            // Simplified multipart/form-data parsing
            auto content_type_header_opt = req.header("Content-Type");
            if (!content_type_header_opt || content_type_header_opt->find("multipart/form-data") == std::string_view::npos) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Content-Type must be multipart/form-data";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            std::unordered_map<std::string, std::string> form_data = parse_multipart_form_data(req.body(), *content_type_header_opt);

            // Access fields
            std::string title = form_data["title"];
            std::string description = form_data["description"];
            std::string category = form_data["category"];
            std::string tags_str = form_data["tags"];
            std::string filename_in_uploads = form_data["video_filename"]; // filename from parse_multipart_form_data

            if (title.empty() || filename_in_uploads.empty()) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Missing title or video file";
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

            Oreshnek::JsonValue response_json = Oreshnek::JsonValue::object();
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
            Oreshnek::JsonValue response_json = Oreshnek::JsonValue::object();
            response_json["success"] = true;
            response_json["videos"] = Oreshnek::JsonValue::array();
            
            for(const auto& video : videos) { //
                Oreshnek::JsonValue video_json = Oreshnek::JsonValue::object();
                video_json["id"] = video.id;
                video_json["title"] = Oreshnek::JsonValue(video.title);
                video_json["description"] = Oreshnek::JsonValue(video.description);
                video_json["category"] = Oreshnek::JsonValue(video.category);
                Oreshnek::JsonValue tags_array = Oreshnek::JsonValue::array();
                for (const auto& tag : video.tags) {
                    tags_array.get_array().push_back(Oreshnek::JsonValue(tag));
                }
                video_json["tags"] = tags_array;
                video_json["views"] = video.views; //
                video_json["likes"] = video.likes;
                video_json["created_at"] = Oreshnek::JsonValue(video.created_at);
                video_json["duration"] = Oreshnek::JsonValue(video.duration);
                response_json["videos"].get_array().push_back(video_json); //
            }
            
            // --- DEBUG PRINT: Print the generated JSON to stderr for inspection ---
            std::cerr << "DEBUG: Sending /api/videos response: " << response_json.to_string() << std::endl;
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
            std::string video_path = g_server_config.upload_dir + std::string(*filename_opt); //

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
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Missing video ID";
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).json(error_json);
                return;
            }

            int video_id = 0;
            try {
                video_id = std::stoi(std::string(*id_opt));
            } catch (const std::exception& e) {
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
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
                Oreshnek::JsonValue error_json = Oreshnek::JsonValue::object();
                error_json["success"] = false;
                error_json["message"] = "Video not found";
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND).json(error_json);
                return;
            }

            Oreshnek::JsonValue response_json = Oreshnek::JsonValue::object();
            response_json["success"] = true;
            Oreshnek::JsonValue video_json = Oreshnek::JsonValue::object();
            video_json["id"] = found_video.id;
            video_json["title"] = Oreshnek::JsonValue(found_video.title);
            video_json["description"] = Oreshnek::JsonValue(found_video.description);
            video_json["filename"] = Oreshnek::JsonValue(found_video.filename); // Needed for video player source
            video_json["views"] = found_video.views;
            video_json["likes"] = found_video.likes;
            video_json["created_at"] = Oreshnek::JsonValue(found_video.created_at);
            video_json["duration"] = Oreshnek::JsonValue(found_video.duration);
            response_json["video"] = video_json;
            
            res.json(response_json);
        });


        // Start server
        if (!server.listen(g_server_config.host, g_server_config.port)) { // Use config for host/port
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
