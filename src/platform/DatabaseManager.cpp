// oreshnek/src/platform/DatabaseManager.cpp
#include "oreshnek/platform/DatabaseManager.h"
#include "oreshnek/utils/Logger.h"

#include <stdexcept>
#include <type_traits>

namespace Oreshnek {
namespace Platform {

DatabaseManager::Backend DatabaseManager::make_backend(const ServerConfig& config) {
    const DatabaseConfig& db = config.db;

#if defined(ORESHNEK_WITH_SQLITE)
    if (db.backend == "sqlite") {
        ORE_LOG(INFO) << "Using SQLite backend (" << db.sqlite_path << ")";
        return std::make_unique<SqliteBackend>(db.sqlite_path, db.sqlite_pool_size,
                                                db.sqlite_busy_timeout_ms);
    }
#endif
#if defined(ORESHNEK_WITH_POSTGRES)
    if (db.backend == "postgres") {
        ORE_LOG(INFO) << "Using PostgreSQL backend";
        return std::make_unique<PgBackend>(db);
    }
#endif
#if defined(ORESHNEK_WITH_ORACLE)
    if (db.backend == "oracle") {
        ORE_LOG(INFO) << "Using Oracle backend";
        return std::make_unique<OracleBackend>(db);
    }
#endif

    throw std::runtime_error(
        "db.backend='" + db.backend + "' is not available: this build was compiled without "
        "support for it (enable ORESHNEK_WITH_SQLITE / ORESHNEK_WITH_POSTGRES / "
        "ORESHNEK_WITH_ORACLE in CMake).");
}

DatabaseManager::DatabaseManager(const ServerConfig& config)
    : backend_(make_backend(config)) {}

SqlResult DatabaseManager::query(std::string_view sql, const SqlParams& params) {
    return std::visit(
        [&](auto& backend) -> SqlResult {
            if constexpr (std::is_same_v<std::decay_t<decltype(backend)>, std::monostate>) {
                throw std::logic_error("DatabaseManager: no backend configured");
            } else {
                return backend->query(sql, params);
            }
        },
        backend_);
}

SqlResult DatabaseManager::exec(std::string_view sql, const SqlParams& params) {
    return std::visit(
        [&](auto& backend) -> SqlResult {
            if constexpr (std::is_same_v<std::decay_t<decltype(backend)>, std::monostate>) {
                throw std::logic_error("DatabaseManager: no backend configured");
            } else {
                return backend->exec(sql, params);
            }
        },
        backend_);
}

}  // namespace Platform
}  // namespace Oreshnek
