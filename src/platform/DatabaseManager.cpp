// oreshnek/src/platform/DatabaseManager.cpp
#include "oreshnek/platform/DatabaseManager.h"
#include "oreshnek/utils/Logger.h"

namespace Oreshnek {
namespace Platform {

DatabaseManager::Backend DatabaseManager::make_backend(const ServerConfig& config) {
    const DatabaseConfig& db = config.db;
    if (db.backend == "postgres") {
        ORE_LOG(INFO) << "Using PostgreSQL backend";
        return std::make_unique<PgBackend>(db);
    }
    if (db.backend != "sqlite") {
        ORE_LOG(WARN) << "Unknown db.backend='" << db.backend << "'; using SQLite.";
    }
    ORE_LOG(INFO) << "Using SQLite backend (" << db.sqlite_path << ")";
    return std::make_unique<SqliteBackend>(db.sqlite_path, db.sqlite_pool_size,
                                           db.sqlite_busy_timeout_ms);
}

DatabaseManager::DatabaseManager(const ServerConfig& config)
    : backend_(make_backend(config)) {}

SqlResult DatabaseManager::query(std::string_view sql, const SqlParams& params) {
    return std::visit([&](auto& backend) { return backend->query(sql, params); }, backend_);
}

SqlResult DatabaseManager::exec(std::string_view sql, const SqlParams& params) {
    return std::visit([&](auto& backend) { return backend->exec(sql, params); }, backend_);
}

}  // namespace Platform
}  // namespace Oreshnek
