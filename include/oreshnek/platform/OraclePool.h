// oreshnek/include/oreshnek/platform/OraclePool.h
#ifndef ORESHNEK_PLATFORM_ORACLE_POOL_H
#define ORESHNEK_PLATFORM_ORACLE_POOL_H

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <oci.h>

namespace Oreshnek {
namespace Platform {

// A fixed-size pool of OCI sessions against one Oracle database, sharing a
// single thread-safe OCI environment (OCI_THREADED). Each pooled session owns
// its own OCIError handle, since OCI error handles must not be shared across
// concurrently-executing connections/threads. A session is checked out via
// acquire() (blocking until one is free) and returned by the RAII Handle.
class OraclePool {
public:
    // Logs on `size` sessions (clamped to >= 1) to `connect_string` (an Easy
    // Connect string "host:port/service_name" or a TNS alias resolvable via
    // tnsnames.ora) as `user`/`password`. Throws std::runtime_error if the
    // environment or any session cannot be created.
    OraclePool(const std::string& connect_string, const std::string& user,
               const std::string& password, int size);
    ~OraclePool();

    OraclePool(const OraclePool&) = delete;
    OraclePool& operator=(const OraclePool&) = delete;

    struct Session {
        OCISvcCtx* svc = nullptr;
        OCIError* err = nullptr;
    };

    class Handle {
    public:
        Handle(OraclePool* pool, Session* session) : pool_(pool), session_(session) {}
        ~Handle() { if (pool_ && session_) pool_->release(session_); }

        Handle(Handle&& other) noexcept : pool_(other.pool_), session_(other.session_) {
            other.pool_ = nullptr;
            other.session_ = nullptr;
        }
        Handle& operator=(Handle&& other) noexcept {
            if (this != &other) {
                if (pool_ && session_) pool_->release(session_);
                pool_ = other.pool_;
                session_ = other.session_;
                other.pool_ = nullptr;
                other.session_ = nullptr;
            }
            return *this;
        }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        OCISvcCtx* svc() const { return session_->svc; }
        OCIError* err() const { return session_->err; }

    private:
        OraclePool* pool_;
        Session* session_;
    };

    // Blocks until a session is available, then checks it out.
    Handle acquire();

    // The environment handle shared by every session in the pool; needed by
    // the backend to allocate per-call statement handles.
    OCIEnv* env() const { return env_; }

    int size() const { return static_cast<int>(sessions_.size()); }

private:
    void release(Session* session);

    OCIEnv* env_ = nullptr;
    std::vector<Session> sessions_;    // Owns every session (for teardown).
    std::queue<Session*> available_;   // Pointers into `sessions_`, stable
                                        // because it is reserve()'d up front.
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_ORACLE_POOL_H
