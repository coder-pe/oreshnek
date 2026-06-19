// oreshnek/src/platform/SqlitePool.cpp
#include "oreshnek/platform/SqlitePool.h"
#include "oreshnek/utils/Logger.h"

#include <stdexcept>
#include <string>

namespace Oreshnek {
namespace Platform {

namespace {
// Run a one-off PRAGMA/statement, throwing on failure with the SQLite message.
void exec_or_throw(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string message = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error(std::string("SQLite '") + sql + "' failed: " + message);
    }
}
}  // namespace

SqlitePool::SqlitePool(const std::string& path, int size, int busy_timeout_ms) {
    if (size < 1) size = 1;
    connections_.reserve(static_cast<size_t>(size));

    for (int i = 0; i < size; ++i) {
        sqlite3* db = nullptr;
        int rc = sqlite3_open_v2(
            path.c_str(), &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            std::string message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
            if (db) sqlite3_close(db);
            // Roll back already-opened connections before failing.
            for (sqlite3* opened : connections_) sqlite3_close(opened);
            throw std::runtime_error("Cannot open database '" + path + "': " + message);
        }

        sqlite3_busy_timeout(db, busy_timeout_ms);
        // WAL: concurrent readers with a single writer. NORMAL synchronous is the
        // recommended durable-enough pairing with WAL. Enforce foreign keys.
        exec_or_throw(db, "PRAGMA journal_mode=WAL;");
        exec_or_throw(db, "PRAGMA synchronous=NORMAL;");
        exec_or_throw(db, "PRAGMA foreign_keys=ON;");

        connections_.push_back(db);
        available_.push(db);
    }

    ORE_LOG(INFO) << "SQLite pool ready: " << size << " connection(s) to " << path
                  << " (WAL, busy_timeout=" << busy_timeout_ms << "ms)";
}

SqlitePool::~SqlitePool() {
    // All Handles are expected to be released before the pool is destroyed.
    for (sqlite3* db : connections_) {
        sqlite3_close(db);
    }
}

SqlitePool::Handle SqlitePool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !available_.empty(); });
    sqlite3* db = available_.front();
    available_.pop();
    return Handle(this, db);
}

void SqlitePool::release(sqlite3* db) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        available_.push(db);
    }
    cv_.notify_one();
}

}  // namespace Platform
}  // namespace Oreshnek
