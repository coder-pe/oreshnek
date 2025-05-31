// src/main.cpp
#include "oreshnek/Oreshnek.h" // Include the convenience header
#include "oreshnek/platform/DatabaseManager.h" // Include your DatabaseManager
#include "oreshnek/platform/SecurityUtils.h"   // Include your SecurityUtils
// #include "oreshnek/platform/Models.h" // Assuming this holds User, Video, etc. if not in DatabaseManager.h

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
    std::string decoded; // [cite: 174]
    for(size_t i = 0; i < str.length(); ++i) {
        if(str[i] == '%' && i + 2 < str.length()) {
            int hex_value; // [cite: 175]
            std::istringstream hex_stream(str.substr(i + 1, 2));
            if(hex_stream >> std::hex >> hex_value) {
                decoded += static_cast<char>(hex_value); // [cite: 176]
                i += 2;
            } else {
                decoded += str[i]; // [cite: 177]
            }
        } else if(str[i] == '+') {
            decoded += ' '; // [cite: 178]
        } else {
            decoded += str[i]; // [cite: 179]
        }
    }
    return decoded;
}

// Function to parse x-www-form-urlencoded (from your platform_video_streaming.txt)
std::unordered_map<std::string, std::string> parse_form_urlencoded(const std::string_view& data) {
    std::unordered_map<std::string, std::string> params;
    std::string temp_data(data); // Convert string_view to string for stringstream
    std::istringstream stream(temp_data); // [cite: 171]
    std::string pair;
    
    while(std::getline(stream, pair, '&')) { // [cite: 172]
        size_t eq_pos = pair.find('=');
        if(eq_pos != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq_pos)); // [cite: 173]
            std::string value = url_decode(pair.substr(eq_pos + 1));
            params[key] = value;
        }
    }
    return params;
}


int main() {
    try {
        // Initialize ServerConfig
        // You can load this from a file later (e.g., JSON, TOML) [cite: 55]
        g_server_config.port = 8080;
        g_server_config.jwt_secret = "my-super-secret-jwt-key-for-oreshnek-platform-tutorial-streaming"; // [cite: 63]
        g_server_config.upload_dir = "./uploads/";
        g_server_config.static_dir = "./static/";
        g_server_config.db_path = "./database.db";

        // Create necessary directories
        std::filesystem::create_directories(g_server_config.upload_dir); // [cite: 183]
        std::filesystem::create_directories(g_server_config.static_dir); // [cite: 183]


        // Create DatabaseManager instance
        Oreshnek::Platform::DatabaseManager db_manager(g_server_config.db_path); // [cite: 182]
        g_db_manager = &db_manager; // Set global pointer

        // Create server instance
        Oreshnek::Server::Server server(g_server_config.thread_pool_size); // [cite: 62]
        g_server = &server; // Set global pointer for signal handling

        // Setup signal handlers
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // --- Define API routes for the Video Streaming Platform ---

        // Serve static files (e.g., HTML, CSS, JS) [cite: 206]
        server.get("/static/:file_path", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            std::optional<std::string_view> file_path_opt = req.param("file_path");
            if (!file_path_opt) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).text("Missing file path");
                return;
            }
            std::string file_path = g_server_config.static_dir + std::string(*file_path_opt);

            if (!std::filesystem::exists(file_path) || std::filesystem::is_directory(file_path)) {
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND).text("File not found"); // [cite: 375]
                return;
            }

            std::string content_type = "application/octet-stream";
            if (file_path.ends_with(".css")) content_type = "text/css";
            else if (file_path.ends_with(".js")) content_type = "application/javascript";
            else if (file_path.ends_with(".png")) content_type = "image/png"; // [cite: 377]
            else if (file_path.ends_with(".jpg") || file_path.ends_with(".jpeg")) content_type = "image/jpeg"; // [cite: 378]
            else if (file_path.ends_with(".html") || file_path.ends_with(".htm")) content_type = "text/html";

            res.status(Oreshnek::Http::HttpStatus::OK).file(file_path, content_type); // Use new file() method
        });

        // Serve home page [cite: 200]
        server.get("/", [](const Oreshnek::HttpRequest& /*req*/, Oreshnek::HttpResponse& res) {
            std::string html_content = R"ORES_HTML_DELIM(<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Video Tutorial Platform</title>
    <link rel="stylesheet" href="/static/style.css">
</head>
<body>
    <header>
        <div class="container">
            <nav>
                <div class="logo">🎓 Video Tutorials</div>
                <ul class="nav-links">
                    <li><a href="/">Inicio</a></li>
                    <li><a href="#videos">Videos</a></li>
                    <li><a href="#" onclick="showAuth()">Login</a></li>
                    <li><a href="#" onclick="showUpload()">Subir Video</a></li>
                </ul>
            </nav>
        </div>
    </header>

    <section class="hero">
        <div class="container">
            <h1>Aprende Programación</h1>
            <p>La mejor plataforma de video tutoriales de programación en español</p>
            <a href="#videos" class="btn">Ver Videos</a>
        </div>
    </section>

    <section class="features">
        <div class="container">
            <div class="features-grid">
                <div class="feature-card">
                    <h3>Contenido de Calidad</h3>
                    <p>Tutoriales creados por expertos en programación con años de experiencia</p>
                </div>
                <div class="feature-card">
                    <h3>Tecnologías Modernas</h3>
                    <p>Aprende las últimas tecnologías y frameworks más demandados</p>
                </div>
                <div class="feature-card">
                    <h3>Comunidad Activa</h3>
                    <p>Interactúa con otros estudiantes y instructores en los comentarios</p>
                </div>
            </div>
        </div>
    </section>

    <div id="auth-modal" class="hidden">
        <div class="container">
            <div class="auth-section">
                <h2>Iniciar Sesión / Registrarse</h2>
                <div style="display: flex; gap: 2rem;">
                    <div style="flex: 1;">
                        <h3>Iniciar Sesión</h3>
                        <form id="login-form">
                            <div class="form-group">
                                <label>Usuario:</label>
                                <input type="text" id="login-username" required>
                            </div>
                            <div class="form-group">
                                <label>Contraseña:</label>
                                <input type="password" id="login-password" required>
                            </div>
                            <button type="submit" class="btn">Iniciar Sesión</button>
                        </form>
                    </div>
                    <div style="flex: 1;">
                        <h3>Registrarse</h3>
                        <form id="register-form">
                            <div class="form-group">
                                <label>Usuario:</label>
                                <input type="text" id="register-username" required>
                            </div>
                            <div class="form-group">
                                <label>Email:</label>
                                <input type="email" id="register-email" required>
                            </div>
                            <div class="form-group">
                                <label>Contraseña:</label>
                                <input type="password" id="register-password" required>
                            </div>
                            <div class="form-group">
                                <label>Rol:</label>
                                <select id="register-role">
                                    <option value="student">Estudiante</option>
                                    <option value="instructor">Instructor</option>
                                </select>
                            </div>
                            <button type="submit" class="btn">Registrarse</button>
                        </form>
                    </div>
                </div>
                <button onclick="hideAuth()" class="btn" style="background: #95a5a6; margin-top: 1rem;">Cerrar</button>
            </div>
        </div>
    </div>

    <div id="upload-modal" class="hidden">
        <div class="container">
            <div class="auth-section">
                <h2>Subir Video Tutorial</h2>
                <form id="upload-form" enctype="multipart/form-data">
                    <div class="form-group">
                        <label>Título:</label>
                        <input type="text" id="video-title" required>
                    </div>
                    <div class="form-group">
                        <label>Descripción:</label>
                        <textarea id="video-description" rows="4"></textarea>
                    </div>
                    <div class="form-group">
                        <label>Categoría:</label>
                        <select id="video-category">
                            <option value="cpp">C++</option>
                            <option value="python">Python</option>
                            <option value="javascript">JavaScript</option>
                            <option value="java">Java</option>
                            <option value="web">Desarrollo Web</option>
                            <option value="mobile">Desarrollo Móvil</option>
                            <option value="algorithms">Algoritmos</option>
                            <option value="databases">Bases de Datos</option>
                        </select>
                    </div>
                    <div class="form-group">
                        <label>Tags (separados por comas):</label>
                        <input type="text" id="video-tags" placeholder="tutorial, principiante, programación">
                    </div>
                    <div class="upload-area" onclick="document.getElementById('video-file').click()">
                        <p>Haz clic para seleccionar un archivo de video</p>
                        <p>Formatos soportados: MP4, WebM, AVI (máx. 500MB)</p>
                        <input type="file" id="video-file" accept="video/*" style="display:none;" onchange="updateFileName(this)">
                    </div>
                    <div id="file-name"></div>
                    <button type="submit" class="btn">Subir Video</button>
                </form>
                <button onclick="hideUpload()" class="btn" style="background: #95a5a6; margin-top: 1rem;">Cerrar</button>
            </div>
        </div>
    </div>

    <section id="videos">
        <div class="container">
            <h2 style="text-align: center; margin-bottom: 2rem;">Videos Recientes</h2>
            <div id="video-list" class="video-grid">
                </div>
        </div>
    </section>

    <footer>
        <div class="container">
            <p>&copy; 2025 Video Tutorial Platform. Hecho con C++17 y amor por la programación.</p>
        </div>
    </footer>

    <script src="/static/script.js"></script>
</body>
</html>)ORES_HTML_DELIM";
            res.status(Oreshnek::Http::HttpStatus::OK).html(html_content);
        });

        // Register User [cite: 204]
        server.post("/api/register", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); // [cite: 325]
            
            if (req.body().empty()) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Empty body";
                return;
            }

            auto form_data = parse_form_urlencoded(req.body());

            auto username_it = form_data.find("username");
            auto email_it = form_data.find("email");
            auto password_it = form_data.find("password");
            auto role_it = form_data.find("role");

            if (username_it == form_data.end() || email_it == form_data.end() || password_it == form_data.end()) { // [cite: 326]
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Missing required fields"; // [cite: 327]
                return;
            }

            // Basic validation (can be enhanced significantly) [cite: 5]
            if (username_it->second.empty() || email_it->second.empty() || password_it->second.empty()) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Fields cannot be empty";
                return;
            }
            // Email format and uniqueness validation [cite: 5]
            // Password strength validation [cite: 5]

            Oreshnek::Platform::User new_user; // [cite: 332]
            new_user.username = username_it->second;
            new_user.email = email_it->second;
            new_user.role = (role_it != form_data.end()) ? role_it->second : "student";

            // Check if user already exists [cite: 329]
            if (g_db_manager->getUserByUsername(new_user.username).id != 0) {
                res.status(Oreshnek::Http::HttpStatus::CONFLICT)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "User already exists"; // [cite: 330]
                return;
            }

            std::string salt = Oreshnek::Platform::SecurityUtils::generateSalt(); // [cite: 333]
            new_user.password_hash = Oreshnek::Platform::SecurityUtils::hashPassword(password_it->second, salt);

            bool success = g_db_manager->createUser(new_user);

            Oreshnek::JsonValue response_json = Oreshnek::JsonValue::object();
            response_json["success"] = success;
            response_json["message"] = success ? "User registered successfully" : "Error creating user"; // [cite: 334]
            res.json(response_json); // [cite: 335]
        });

        // Login User [cite: 203]
        server.post("/api/login", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); // [cite: 313]

            if (req.body().empty()) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Empty body";
                return;
            }

            auto form_data = parse_form_urlencoded(req.body());
            auto username_it = form_data.find("username");
            auto password_it = form_data.find("password");

            if (username_it == form_data.end() || password_it == form_data.end()) { // [cite: 314]
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Missing required fields"; // [cite: 315]
                return;
            }

            Oreshnek::Platform::User user = g_db_manager->getUserByUsername(username_it->second); // [cite: 317]
            if (user.id == 0) { // User not found
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "User not found"; // [cite: 318]
                return;
            }

            // In a real implementation, store salt with user and retrieve it here.
            std::string salt = "default_salt"; // [cite: 320]
            std::string password_hash = Oreshnek::Platform::SecurityUtils::hashPassword(password_it->second, salt); // [cite: 321]

            if (password_hash != user.password_hash) {
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Incorrect password"; // [cite: 322]
                return;
            }

            std::string token = Oreshnek::Platform::SecurityUtils::generateJWT(user.id, user.username, g_server_config.jwt_secret); // [cite: 323]

            Oreshnek::JsonValue success_response = Oreshnek::JsonValue::object();
            success_response["success"] = true;
            success_response["token"] = Oreshnek::JsonValue(token);
            Oreshnek::JsonValue user_json = Oreshnek::JsonValue::object();
            user_json["id"] = Oreshnek::JsonValue(user.id);
            user_json["username"] = Oreshnek::JsonValue(user.username);
            user_json["role"] = Oreshnek::JsonValue(user.role);
            success_response["user"] = user_json;
            
            res.json(success_response); // [cite: 324]
        });

        // Upload Video [cite: 202]
        server.post("/api/upload", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); // [cite: 336]

            auto auth_header = req.header("Authorization"); // [cite: 337]
            if (!auth_header) {
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Authentication token required"; // [cite: 338]
                return;
            }

            std::string token = std::string(*auth_header); // [cite: 339]
            if (token.length() > 7 && token.substr(0, 7) == "Bearer ") { // [cite: 340]
                token = token.substr(7); // [cite: 341]
            } else {
                 res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Invalid Authorization header format";
                return;
            }
            
            if (!Oreshnek::Platform::SecurityUtils::validateJWT(token, g_server_config.jwt_secret)) { // [cite: 342]
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Invalid token"; // [cite: 343]
                return;
            }

            // Extract user_id from JWT payload
            nlohmann::json jwt_payload = Oreshnek::Platform::SecurityUtils::decodeJWT(token);
            if (jwt_payload.is_null() || !jwt_payload.contains("user_id")) {
                res.status(Oreshnek::Http::HttpStatus::UNAUTHORIZED)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Invalid token payload";
                return;
            }
            int user_id = jwt_payload["user_id"].get<int>();

            // Simplified multipart/form-data parsing
            auto content_type_header_opt = req.header("Content-Type");
            if (!content_type_header_opt || content_type_header_opt->find("multipart/form-data") == std::string_view::npos) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Content-Type must be multipart/form-data";
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
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Missing title or video file";
                return;
            }

            Oreshnek::Platform::Video new_video;
            new_video.title = title;
            new_video.description = description;
            new_video.category = category;
            new_video.filename = filename_in_uploads;
            new_video.user_id = user_id;
            new_video.duration = "00:00"; // Placeholder, processing would set this [cite: 16]

            std::stringstream ss(tags_str);
            std::string tag;
            while(std::getline(ss, tag, ',')) {
                new_video.tags.push_back(tag);
            }
            
            bool success = g_db_manager->createVideo(new_video);

            Oreshnek::JsonValue response_json = Oreshnek::JsonValue::object();
            response_json["success"] = success; // [cite: 344]
            response_json["message"] = success ? "Video uploaded successfully" : "Error uploading video";
            res.json(response_json); // [cite: 345]
        });

        // Get Video List [cite: 201]
        server.get("/api/videos", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json"); // [cite: 305]

            int limit = 20;
            int offset = 0;
            std::string category = "";

            auto limit_opt = req.query("limit");
            if (limit_opt) {
                try { limit = std::stoi(std::string(*limit_opt)); } catch (...) {} // [cite: 306]
            }
            auto offset_opt = req.query("offset");
            if (offset_opt) {
                try { offset = std::stoi(std::string(*offset_opt)); } catch (...) {}
            }
            auto category_opt = req.query("category");
            if (category_opt) {
                category = std::string(*category_opt); // [cite: 307]
            }
            
            std::vector<Oreshnek::Platform::Video> videos = g_db_manager->getVideos(limit, offset, category); // [cite: 308]
            Oreshnek::JsonValue response_json = Oreshnek::JsonValue::object();
            response_json["success"] = true;
            response_json["videos"] = Oreshnek::JsonValue::array();
            
            for(const auto& video : videos) { // [cite: 309]
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
                video_json["views"] = video.views; // [cite: 310]
                video_json["likes"] = video.likes;
                video_json["created_at"] = Oreshnek::JsonValue(video.created_at);
                video_json["duration"] = Oreshnek::JsonValue(video.duration);
                response_json["videos"].get_array().push_back(video_json); // [cite: 311]
            }
            
            res.json(response_json); // [cite: 312]
        });

        // Serve a specific video file (e.g., for direct playback) [cite: 205]
        server.get("/video/:filename", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            std::optional<std::string_view> filename_opt = req.param("filename");
            if (!filename_opt) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST).text("Missing video filename");
                return;
            }
            std::string video_path = g_server_config.upload_dir + std::string(*filename_opt); // [cite: 348]

            if (!std::filesystem::exists(video_path) || std::filesystem::is_directory(video_path)) {
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND).text("Video not found"); // [cite: 349]
                return;
            }
            
            res.status(Oreshnek::Http::HttpStatus::OK).file(video_path, "video/mp4"); // [cite: 352]
            res.header("Accept-Ranges", "bytes"); // Crucial for video streaming [cite: 352]
            // Note: Range request handling needs to be implemented in Connection::write_data
        });

        // Serve video player page [cite: 207]
        server.get("/watch", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "text/html; charset=utf-8"); // [cite: 353]

            std::optional<std::string_view> id_opt = req.query("id");
            if (!id_opt) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .html("<h1>400 - Bad Request: Missing video ID</h1>"); // [cite: 354]
                return;
            }

            int video_id = 0;
            try {
                video_id = std::stoi(std::string(*id_opt)); // [cite: 356]
            } catch (const std::exception& e) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .html("<h1>400 - Bad Request: Invalid video ID format</h1>");
                return;
            }
            
            // Increment view count [cite: 357]
            g_db_manager->incrementViews(video_id);

            // Fetch video details to populate the player page
            // (You'll need a get_video_by_id in DatabaseManager)
            // For now, let's just use the ID in the HTML
            std::string video_player_html = R"ORES_HTML_DELIM(<!DOCTYPE html>
<html lang="es">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Reproducir Video - Video Tutorial Platform</title>
    <link rel="stylesheet" href="/static/style.css">
    <style>
        .video-player video { width: 100%; height: auto; } /* [cite: 358] */
    </style>
</head>
<body>
    <div class="container">
        <a href="/" class="back-link">← Volver al inicio</a>
        
        <div class="video-player">
            <video controls>
                <source src="/video/VIDEO_FILENAME_PLACEHOLDER" type="video/mp4">
                Tu navegador no soporta la reproducción de videos.
            </video>
        </div>
        
        <div class="video-info">
            <h1 class="video-title" id="video-title">Cargando...</h1>
            <div class="video-meta" id="video-meta">
                <span id="views">0</span> visualizaciones • 
                <span id="likes">0</span> likes • 
                <span id="date">Fecha</span>
            </div>
            <div class="video-description" id="video-description">
                Cargando descripción...
            </div>
        </div>
        
        <div class="comments-section">
            <h3>Comentarios</h3>
            <div class="comment-form">
                <textarea placeholder="Escribe un comentario..." rows="3"></textarea>
                <br><br>
                <button class="btn">Comentar</button>
            </div>
            <div id="comments-list">
                </div>
        </div>
    </div>

    <script>
        document.addEventListener('DOMContentLoaded', function() {
            loadVideoInfo();
        });

        async function loadVideoInfo() {
            const urlParams = new URLSearchParams(window.location.search);
            const videoId = urlParams.get('id');
            if (!videoId) {
                console.error('Video ID not found in URL.');
                return;
            }

            try {
                const response = await fetch(`/api/video_details/${videoId}`);
                const result = await response.json();
                
                if(result.success && result.video) {
                    const video = result.video;
                    document.getElementById('video-title').textContent = video.title;
                    document.getElementById('views').textContent = video.views;
                    document.getElementById('likes').textContent = video.likes;
                    document.getElementById('date').textContent = video.created_at;
                    document.getElementById('video-description').textContent = video.description;
                    document.title = video.title + ' - Video Tutorial Platform';

                    // Update video source
                    const videoElement = document.querySelector('.video-player video');
                    if (videoElement) {
                        videoElement.querySelector('source').src = `/video/${video.filename}`;
                        videoElement.load(); // Reload video element
                    }
                } else {
                    console.error('Error loading video info:', result.message);
                }
            } catch(error) {
                console.error('Error loading video info:', error);
            }
        }
    </script>
</body>
</html>)ORES_HTML_DELIM";
            res.status(Oreshnek::Http::HttpStatus::OK).html(video_player_html);
        });

        // API to get video details (for the player page to fetch dynamically)
        server.get("/api/video_details/:id", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
            res.header("Content-Type", "application/json");

            std::optional<std::string_view> id_opt = req.param("id");
            if (!id_opt) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Missing video ID";
                return;
            }

            int video_id = 0;
            try {
                video_id = std::stoi(std::string(*id_opt));
            } catch (const std::exception& e) {
                res.status(Oreshnek::Http::HttpStatus::BAD_REQUEST)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Invalid video ID format";
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
                res.status(Oreshnek::Http::HttpStatus::NOT_FOUND)
                   .json(Oreshnek::JsonValue::object()["success"] = false)["message"] = "Video not found";
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
