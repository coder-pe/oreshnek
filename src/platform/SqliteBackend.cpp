// oreshnek/src/platform/SqliteBackend.cpp
#include "oreshnek/platform/SqliteBackend.h"
#include "oreshnek/utils/Logger.h"

#include <sstream>

namespace Oreshnek {
namespace Platform {

SqliteBackend::SqliteBackend(const std::string& path, int pool_size, int busy_timeout_ms)
    : pool_(path, pool_size, busy_timeout_ms) {}

void SqliteBackend::initialize_tables_impl() {
    auto conn = pool_.acquire();
    sqlite3* db = conn.get();
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
    )";
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
    )";
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
    )";
    const char* create_sessions = R"(
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER,
            token TEXT UNIQUE NOT NULL,
            expires_at DATETIME NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(user_id) REFERENCES users(id)
        );
    )";

    sqlite3_exec(db, create_users, 0, 0, 0);
    sqlite3_exec(db, create_videos, 0, 0, 0);
    sqlite3_exec(db, create_comments, 0, 0, 0);
    sqlite3_exec(db, create_sessions, 0, 0, 0);
}

bool SqliteBackend::create_user_impl(const User& user) {
    auto conn = pool_.acquire();
    sqlite3* db = conn.get();
    const char* sql = R"(
        INSERT INTO users (username, email, password_hash, role)
        VALUES (?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if(rc != SQLITE_OK) {
        ORE_LOG(ERROR) << "createUser prepare error: " << sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, user.username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user.email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, user.password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, user.role.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ORE_LOG(ERROR) << "createUser execution error: " << sqlite3_errmsg(db);
        return false;
    }
    return true;
}

User SqliteBackend::user_by_username_impl(const std::string& username) {
    auto conn = pool_.acquire();
    sqlite3* db = conn.get();
    const char* sql = R"(
        SELECT id, username, email, password_hash, role, created_at, is_active
        FROM users WHERE username = ?;
    )";
    sqlite3_stmt* stmt;
    User user_result = {};

    if(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        if(sqlite3_step(stmt) == SQLITE_ROW) {
            user_result.id = sqlite3_column_int(stmt, 0);
            user_result.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            user_result.email = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            user_result.password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            user_result.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            user_result.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            user_result.is_active = sqlite3_column_int(stmt, 6);
        }
    }
    sqlite3_finalize(stmt);
    return user_result;
}

bool SqliteBackend::create_video_impl(const Video& video) {
    auto conn = pool_.acquire();
    sqlite3* db = conn.get();
    const char* sql = R"(
        INSERT INTO videos (title, description, filename, thumbnail, user_id, category, tags, duration, is_public)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if(rc != SQLITE_OK) {
        ORE_LOG(ERROR) << "createVideo prepare error: " << sqlite3_errmsg(db);
        return false;
    }

    std::string tags_str;
    for(const auto& tag : video.tags) {
        if(!tags_str.empty()) tags_str += ",";
        tags_str += tag;
    }

    sqlite3_bind_text(stmt, 1, video.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, video.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, video.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, video.thumbnail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, video.user_id);
    sqlite3_bind_text(stmt, 6, video.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, tags_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, video.duration.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, video.is_public ? 1 : 0);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ORE_LOG(ERROR) << "createVideo execution error: " << sqlite3_errmsg(db);
        return false;
    }
    return true;
}

std::vector<Video> SqliteBackend::videos_impl(int limit, int offset, const std::string& category) {
    auto conn = pool_.acquire();
    sqlite3* db = conn.get();
    std::string sql = R"(
        SELECT id, title, description, filename, thumbnail, user_id, category, tags, views, likes, created_at, duration, is_public
        FROM videos WHERE is_public = 1
    )";
    if(!category.empty()) {
        sql += " AND category = ?";
    }
    sql += " ORDER BY created_at DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    std::vector<Video> videos;

    if(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL) == SQLITE_OK) {
        int param_index = 1;
        if(!category.empty()) {
            sqlite3_bind_text(stmt, param_index++, category.c_str(), -1, SQLITE_STATIC);
        }
        sqlite3_bind_int(stmt, param_index++, limit);
        sqlite3_bind_int(stmt, param_index++, offset);

        while(sqlite3_step(stmt) == SQLITE_ROW) {
            Video video;
            video.id = sqlite3_column_int(stmt, 0);
            video.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            video.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            video.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            video.thumbnail = sqlite3_column_text(stmt, 4) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) : "";
            video.user_id = sqlite3_column_int(stmt, 5);
            video.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            std::string tags_str = sqlite3_column_text(stmt, 7) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)) : "";
            if(!tags_str.empty()) {
                std::stringstream ss(tags_str);
                std::string tag;
                while(std::getline(ss, tag, ',')) {
                    video.tags.push_back(tag);
                }
            }

            video.views = sqlite3_column_int(stmt, 8);
            video.likes = sqlite3_column_int(stmt, 9);
            video.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
            video.duration = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            video.is_public = sqlite3_column_int(stmt, 12);

            videos.push_back(video);
        }
    }
    sqlite3_finalize(stmt);
    return videos;
}

bool SqliteBackend::increment_views_impl(int video_id) {
    auto conn = pool_.acquire();
    sqlite3* db = conn.get();
    const char* sql = "UPDATE videos SET views = views + 1 WHERE id = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if(rc != SQLITE_OK) {
        ORE_LOG(ERROR) << "incrementViews prepare error: " << sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_int(stmt, 1, video_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        ORE_LOG(ERROR) << "incrementViews execution error: " << sqlite3_errmsg(db);
        return false;
    }
    return true;
}

}  // namespace Platform
}  // namespace Oreshnek
