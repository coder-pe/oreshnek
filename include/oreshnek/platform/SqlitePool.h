// oreshnek/include/oreshnek/platform/SqlitePool.h
#ifndef ORESHNEK_PLATFORM_SQLITE_POOL_H
#define ORESHNEK_PLATFORM_SQLITE_POOL_H

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <sqlite3.h>

namespace Oreshnek {
namespace Platform {

// A small fixed-size pool of SQLite connections to a single database file.
//
// Each connection is opened in WAL mode (concurrent readers + one writer) with a
// busy timeout, so worker threads can run queries in parallel instead of
// serializing on a single global mutex. A connection is checked out via
// acquire() (blocking until one is free) and automatically returned when the
// RAII Handle goes out of scope.
class SqlitePool {
public:
    // Opens `size` connections (clamped to >= 1) to `path`. Throws
    // std::runtime_error if any connection cannot be opened/configured.
    SqlitePool(const std::string& path, int size, int busy_timeout_ms);
    ~SqlitePool();

    SqlitePool(const SqlitePool&) = delete;
    SqlitePool& operator=(const SqlitePool&) = delete;

    // RAII checkout: holds a connection for the calling thread and returns it to
    // the pool on destruction.
    class Handle {
    public:
        Handle(SqlitePool* pool, sqlite3* db) : pool_(pool), db_(db) {}
        ~Handle() { if (pool_ && db_) pool_->release(db_); }

        Handle(Handle&& other) noexcept : pool_(other.pool_), db_(other.db_) {
            other.pool_ = nullptr;
            other.db_ = nullptr;
        }
        Handle& operator=(Handle&& other) noexcept {
            if (this != &other) {
                if (pool_ && db_) pool_->release(db_);
                pool_ = other.pool_;
                db_ = other.db_;
                other.pool_ = nullptr;
                other.db_ = nullptr;
            }
            return *this;
        }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        sqlite3* get() const { return db_; }
        operator sqlite3*() const { return db_; }

    private:
        SqlitePool* pool_;
        sqlite3* db_;
    };

    // Blocks until a connection is available, then checks it out.
    Handle acquire();

    int size() const { return static_cast<int>(connections_.size()); }

private:
    void release(sqlite3* db);

    std::vector<sqlite3*> connections_; // Owns every connection (for teardown).
    std::queue<sqlite3*> available_;    // Currently free connections.
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_SQLITE_POOL_H
