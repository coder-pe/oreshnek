// oreshnek/src/platform/DatabaseManager.cpp
#include "oreshnek/platform/DatabaseManager.h"
#include "oreshnek/utils/Logger.h"

namespace Oreshnek {
namespace Platform {

DatabaseManager::Backend DatabaseManager::make_backend(const ServerConfig& config) {
    const DatabaseConfig& db = config.db;
    if (db.backend == "postgres") {
        // The PostgreSQL backend is not yet compiled into this build; fall back
        // to SQLite so the server still starts.
        ORE_LOG(WARN) << "db.backend='postgres' requested but not available in "
                         "this binary; using SQLite instead.";
    } else if (db.backend != "sqlite") {
        ORE_LOG(WARN) << "Unknown db.backend='" << db.backend << "'; using SQLite.";
    }
    return std::make_unique<SqliteBackend>(db.sqlite_path, db.sqlite_pool_size,
                                           db.sqlite_busy_timeout_ms);
}

DatabaseManager::DatabaseManager(const ServerConfig& config)
    : backend_(make_backend(config)) {
    std::visit([](auto& backend) { backend->initializeTables(); }, backend_);
}

bool DatabaseManager::createUser(const User& user) {
    return std::visit([&](auto& backend) { return backend->createUser(user); }, backend_);
}

User DatabaseManager::getUserByUsername(const std::string& username) {
    return std::visit([&](auto& backend) { return backend->getUserByUsername(username); }, backend_);
}

bool DatabaseManager::createVideo(const Video& video) {
    return std::visit([&](auto& backend) { return backend->createVideo(video); }, backend_);
}

std::vector<Video> DatabaseManager::getVideos(int limit, int offset, const std::string& category) {
    return std::visit([&](auto& backend) { return backend->getVideos(limit, offset, category); },
                      backend_);
}

bool DatabaseManager::incrementViews(int video_id) {
    return std::visit([&](auto& backend) { return backend->incrementViews(video_id); }, backend_);
}

}  // namespace Platform
}  // namespace Oreshnek
