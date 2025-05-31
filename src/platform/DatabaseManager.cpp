#include "oreshnek/platform/DatabaseManager.h"
#include <iostream>
#include <sstream>

namespace Oreshnek {
namespace Platform {

DatabaseManager::DatabaseManager(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if(rc) {
        throw std::runtime_error("Cannot open database: " + std::string(sqlite3_errmsg(db_))); // [cite: 81]
    }
    initializeTables();
}

DatabaseManager::~DatabaseManager() {
    sqlite3_close(db_); // [cite: 82]
}

void DatabaseManager::initializeTables() {
    std::lock_guard<std::mutex> lock(db_mutex_); // [cite: 83]
    const char* create_users = R"(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT UNIQUE NOT NULL,
            email TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL,
            role TEXT DEFAULT 'student',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            is_active BOOLEAN DEFAULT 1
        );
    )"; // [cite: 84]
    const char* create_videos = R"(
        CREATE TABLE IF NOT EXISTS videos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            description TEXT,
            filename TEXT NOT NULL,
            thumbnail TEXT,
            user_id INTEGER,
            category TEXT,
            tags TEXT,
            views INTEGER DEFAULT 0,
            likes INTEGER DEFAULT 0,
            duration TEXT,
            is_public BOOLEAN DEFAULT 1,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(user_id) REFERENCES users(id)
        );
    )"; // [cite: 88]
    const char* create_comments = R"(
        CREATE TABLE IF NOT EXISTS comments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            video_id INTEGER,
            user_id INTEGER,
            content TEXT NOT NULL,
            parent_id INTEGER,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(video_id) REFERENCES videos(id),
            FOREIGN KEY(user_id) REFERENCES users(id),
            FOREIGN KEY(parent_id) REFERENCES comments(id)
        );
    )"; // [cite: 91]
    const char* create_sessions = R"(
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER,
            token TEXT UNIQUE NOT NULL,
            expires_at DATETIME NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(user_id) REFERENCES users(id)
        );
    )"; // [cite: 92]

    sqlite3_exec(db_, create_users, 0, 0, 0); // [cite: 93]
    sqlite3_exec(db_, create_videos, 0, 0, 0);
    sqlite3_exec(db_, create_comments, 0, 0, 0);
    sqlite3_exec(db_, create_sessions, 0, 0, 0);
}

bool DatabaseManager::createUser(const User& user) {
    std::lock_guard<std::mutex> lock(db_mutex_); // [cite: 95]
    const char* sql = R"(
        INSERT INTO users (username, email, password_hash, role)
        VALUES (?, ?, ?, ?);
    )"; // [cite: 96]
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if(rc != SQLITE_OK) {
        std::cerr << "Prepare error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, user.username.c_str(), -1, SQLITE_TRANSIENT); // [cite: 97]
    sqlite3_bind_text(stmt, 2, user.email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, user.password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, user.role.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt); // [cite: 98]
    if (rc != SQLITE_DONE) {
        std::cerr << "Execution error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

User DatabaseManager::getUserByUsername(const std::string& username) {
    std::lock_guard<std::mutex> lock(db_mutex_); // [cite: 99]
    const char* sql = R"(
        SELECT id, username, email, password_hash, role, created_at, is_active
        FROM users WHERE username = ?;
    )"; // [cite: 100]
    sqlite3_stmt* stmt;
    User user_result = {}; // Initialize to default values

    if(sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC); // [cite: 101]
        if(sqlite3_step(stmt) == SQLITE_ROW) {
            user_result.id = sqlite3_column_int(stmt, 0); // [cite: 102]
            user_result.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            user_result.email = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            user_result.password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            user_result.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            user_result.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            user_result.is_active = sqlite3_column_int(stmt, 6); // [cite: 103]
        }
    }
    sqlite3_finalize(stmt); // [cite: 104]
    return user_result;
}

bool DatabaseManager::createVideo(const Video& video) {
    std::lock_guard<std::mutex> lock(db_mutex_); // [cite: 105]
    const char* sql = R"(
        INSERT INTO videos (title, description, filename, thumbnail, user_id, category, tags, duration, is_public)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )"; // [cite: 106]
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if(rc != SQLITE_OK) {
        std::cerr << "Prepare error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    std::string tags_str;
    for(const auto& tag : video.tags) { // [cite: 107]
        if(!tags_str.empty()) tags_str += ","; // [cite: 108]
        tags_str += tag;
    }

    sqlite3_bind_text(stmt, 1, video.title.c_str(), -1, SQLITE_TRANSIENT); // [cite: 109]
    sqlite3_bind_text(stmt, 2, video.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, video.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, video.thumbnail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, video.user_id); // [cite: 110]
    sqlite3_bind_text(stmt, 6, video.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, tags_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, video.duration.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, video.is_public ? 1 : 0); // [cite: 111]

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt); // [cite: 112]
    if (rc != SQLITE_DONE) {
        std::cerr << "Execution error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

std::vector<Video> DatabaseManager::getVideos(int limit, int offset, const std::string& category) {
    std::lock_guard<std::mutex> lock(db_mutex_); // [cite: 113]
    std::string sql = R"(
        SELECT id, title, description, filename, thumbnail, user_id, category, tags, views, likes, created_at, duration, is_public
        FROM videos WHERE is_public = 1
    )"; // [cite: 114]
    if(!category.empty()) {
        sql += " AND category = ?"; // [cite: 115]
    }
    sql += " ORDER BY created_at DESC LIMIT ? OFFSET ?"; // [cite: 116]
    sqlite3_stmt* stmt;
    std::vector<Video> videos;
    
    if(sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
        int param_index = 1; // [cite: 117]
        if(!category.empty()) {
            sqlite3_bind_text(stmt, param_index++, category.c_str(), -1, SQLITE_STATIC); // [cite: 118]
        }
        sqlite3_bind_int(stmt, param_index++, limit);
        sqlite3_bind_int(stmt, param_index++, offset); // [cite: 119]

        while(sqlite3_step(stmt) == SQLITE_ROW) {
            Video video; // [cite: 120]
            video.id = sqlite3_column_int(stmt, 0);
            video.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            video.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            video.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            video.thumbnail = sqlite3_column_text(stmt, 4) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) : ""; // [cite: 121]
            video.user_id = sqlite3_column_int(stmt, 5);
            video.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            std::string tags_str = sqlite3_column_text(stmt, 7) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)) : ""; // [cite: 122]
            if(!tags_str.empty()) { // [cite: 123]
                std::stringstream ss(tags_str);
                std::string tag; // [cite: 124]
                while(std::getline(ss, tag, ',')) {
                    video.tags.push_back(tag); // [cite: 125]
                }
            }
            
            video.views = sqlite3_column_int(stmt, 8); // [cite: 126]
            video.likes = sqlite3_column_int(stmt, 9);
            video.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
            video.duration = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            video.is_public = sqlite3_column_int(stmt, 12);
            
            videos.push_back(video); // [cite: 127]
        }
    }
    sqlite3_finalize(stmt); // [cite: 128]
    return videos;
}

bool DatabaseManager::incrementViews(int video_id) {
    std::lock_guard<std::mutex> lock(db_mutex_); // [cite: 129]
    const char* sql = "UPDATE videos SET views = views + 1 WHERE id = ?";
    sqlite3_stmt* stmt; // [cite: 130]
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL);
    if(rc != SQLITE_OK) {
        std::cerr << "Prepare error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, video_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt); // [cite: 131]
    if (rc != SQLITE_DONE) {
        std::cerr << "Execution error: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

} // namespace Platform
} // namespace Oreshnek
