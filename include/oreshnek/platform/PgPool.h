// oreshnek/include/oreshnek/platform/PgPool.h
#ifndef ORESHNEK_PLATFORM_PG_POOL_H
#define ORESHNEK_PLATFORM_PG_POOL_H

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <libpq-fe.h>

namespace Oreshnek {
namespace Platform {

// A fixed-size pool of libpq connections to one PostgreSQL database. A
// connection is checked out via acquire() (blocking until one is free) and
// returned by the RAII Handle. On return, a connection found in a bad state is
// transparently reset (PQreset) so callers always get a usable connection.
class PgPool {
public:
    // Opens `size` connections (clamped to >= 1) using the given libpq conninfo
    // string or URL. Throws std::runtime_error if any connection fails.
    PgPool(const std::string& conninfo, int size);
    ~PgPool();

    PgPool(const PgPool&) = delete;
    PgPool& operator=(const PgPool&) = delete;

    class Handle {
    public:
        Handle(PgPool* pool, PGconn* conn) : pool_(pool), conn_(conn) {}
        ~Handle() { if (pool_ && conn_) pool_->release(conn_); }

        Handle(Handle&& other) noexcept : pool_(other.pool_), conn_(other.conn_) {
            other.pool_ = nullptr;
            other.conn_ = nullptr;
        }
        Handle& operator=(Handle&& other) noexcept {
            if (this != &other) {
                if (pool_ && conn_) pool_->release(conn_);
                pool_ = other.pool_;
                conn_ = other.conn_;
                other.pool_ = nullptr;
                other.conn_ = nullptr;
            }
            return *this;
        }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        PGconn* get() const { return conn_; }
        operator PGconn*() const { return conn_; }

    private:
        PgPool* pool_;
        PGconn* conn_;
    };

    Handle acquire();
    int size() const { return static_cast<int>(connections_.size()); }

private:
    void release(PGconn* conn);

    std::string conninfo_;
    std::vector<PGconn*> connections_; // Owns every connection.
    std::queue<PGconn*> available_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_PG_POOL_H
