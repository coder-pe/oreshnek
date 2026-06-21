// oreshnek/src/platform/SqliteBackend.cpp
#include "oreshnek/platform/SqliteBackend.h"
#include "oreshnek/utils/Logger.h"

namespace Oreshnek {
namespace Platform {

SqliteBackend::SqliteBackend(const std::string& path, int pool_size, int busy_timeout_ms)
    : pool_(path, pool_size, busy_timeout_ms) {}

SqlResult SqliteBackend::run_impl(std::string_view sql, const SqlParams& params) {
    SqlResult result;
    auto conn = pool_.acquire();
    sqlite3* db = conn.get();

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        result.error = sqlite3_errmsg(db);
        ORE_LOG(ERROR) << "SQLite prepare error: " << result.error;
        return result;
    }

    for (std::size_t i = 0; i < params.size(); ++i) {
        const int idx = static_cast<int>(i) + 1;  // SQLite parameters are 1-based.
        if (params[i].has_value()) {
            sqlite3_bind_text(stmt, idx, params[i]->c_str(),
                              static_cast<int>(params[i]->size()), SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, idx);
        }
    }

    const int col_count = sqlite3_column_count(stmt);
    if (col_count > 0) {
        result.columns.reserve(static_cast<std::size_t>(col_count));
        for (int c = 0; c < col_count; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            result.columns.emplace_back(name ? name : "");
        }
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::vector<std::optional<std::string>> row;
        row.reserve(static_cast<std::size_t>(col_count));
        for (int c = 0; c < col_count; ++c) {
            if (sqlite3_column_type(stmt, c) == SQLITE_NULL) {
                row.emplace_back(std::nullopt);
            } else {
                const auto* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
                const int bytes = sqlite3_column_bytes(stmt, c);
                row.emplace_back(std::string(txt ? txt : "", static_cast<std::size_t>(bytes)));
            }
        }
        result.rows.push_back(std::move(row));
    }

    if (rc != SQLITE_DONE) {
        result.error = sqlite3_errmsg(db);
        ORE_LOG(ERROR) << "SQLite step error: " << result.error;
        sqlite3_finalize(stmt);
        return result;
    }

    sqlite3_finalize(stmt);
    result.ok = true;
    result.affected = sqlite3_changes(db);
    result.last_insert_id = sqlite3_last_insert_rowid(db);
    return result;
}

}  // namespace Platform
}  // namespace Oreshnek
