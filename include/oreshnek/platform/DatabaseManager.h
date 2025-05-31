#ifndef ORESHNEK_PLATFORM_DATABASE_MANAGER_H
#define ORESHNEK_PLATFORM_DATABASE_MANAGER_H

#include <string>
#include <vector>
#include <mutex>
#include <sqlite3.h>
#include <vector> // Required for std::vector
#include <string_view> // Required for std::string_view
#include <thread>
#include <iostream>

// Forward declarations to avoid circular includes for models if they were in separate files
// For now, assuming they are defined directly in this header or a common models header
// Define your User, Video, Comment structures here or include a common models.h
// Based on your provided platform_video_streaming.txt, I'll put them here for now.

namespace Oreshnek {
namespace Platform {

// Configuration struct (from your platform_video_streaming.txt)
struct ServerConfig {
    int port = 8080;
    int max_connections = 1000;
    int thread_pool_size = std::thread::hardware_concurrency() * 2;
    std::string upload_dir = "./uploads/";
    std::string static_dir = "./static/";
    std::string db_path = "./database.db";
    std::string jwt_secret = "your-super-secret-jwt-key-change-this"; // [cite: 63]
    int jwt_expire_hours = 24;
    size_t max_file_size = 500 * 1024 * 1024; // 500MB
    std::string host = "0.0.0.0"; // Add this line
};

// User struct (from your platform_video_streaming.txt) [cite: 64]
struct User {
    int id = 0; // Default to 0, indicating not found/invalid
    std::string username;
    std::string email;
    std::string password_hash;
    std::string role; // "admin", "instructor", "student" [cite: 65]
    std::string created_at;
    bool is_active = true;
};

// Video struct (from your platform_video_streaming.txt) [cite: 66]
struct Video {
    int id = 0;
    std::string title;
    std::string description;
    std::string filename;
    std::string thumbnail;
    int user_id = 0; // [cite: 67]
    std::string category;
    std::vector<std::string> tags;
    int views = 0;
    int likes = 0; // [cite: 87]
    std::string created_at;
    std::string duration;
    bool is_public = true;
};

// Comment struct (from your platform_video_streaming.txt) [cite: 68]
struct Comment {
    int id = 0;
    int video_id = 0;
    int user_id = 0;
    std::string content; // [cite: 89]
    std::string created_at;
    int parent_id = 0; // Para respuestas anidadas [cite: 69]
};


class DatabaseManager {
private:
    sqlite3* db_;
    std::mutex db_mutex_;

public:
    DatabaseManager(const std::string& db_path);
    ~DatabaseManager();

    void initializeTables(); // [cite: 83]

    // User operations [cite: 94]
    bool createUser(const User& user); // [cite: 95]
    User getUserByUsername(const std::string& username); // [cite: 99]
    // Add more user ops (getUserById, updateUser, deleteUser, etc.)

    // Video operations [cite: 104]
    bool createVideo(const Video& video); // [cite: 105]
    std::vector<Video> getVideos(int limit = 20, int offset = 0, const std::string& category = ""); // [cite: 112]
    bool incrementViews(int video_id); // [cite: 129]
    // Add getVideoById, updateVideo, deleteVideo, searchVideos, etc.

    // Comment operations
    // Add createComment, getCommentsForVideo, etc.
};

} // namespace Platform
} // namespace Oreshnek

#endif // ORESHNEK_PLATFORM_DATABASE_MANAGER_H
