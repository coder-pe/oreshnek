// oreshnek/src/platform/PgPool.cpp
#include "oreshnek/platform/PgPool.h"
#include "oreshnek/utils/Logger.h"

#include <stdexcept>

namespace Oreshnek {
namespace Platform {

namespace {
PGconn* open_connection(const std::string& conninfo) {
    PGconn* conn = PQconnectdb(conninfo.c_str());
    if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
        std::string message = conn ? PQerrorMessage(conn) : "out of memory";
        if (conn) PQfinish(conn);
        throw std::runtime_error("PostgreSQL connection failed: " + message);
    }
    return conn;
}
}  // namespace

PgPool::PgPool(const std::string& conninfo, int size) : conninfo_(conninfo) {
    if (size < 1) size = 1;
    connections_.reserve(static_cast<size_t>(size));
    try {
        for (int i = 0; i < size; ++i) {
            PGconn* conn = open_connection(conninfo_);
            connections_.push_back(conn);
            available_.push(conn);
        }
    } catch (...) {
        for (PGconn* c : connections_) PQfinish(c);
        throw;
    }
    ORE_LOG(INFO) << "PostgreSQL pool ready: " << size << " connection(s)";
}

PgPool::~PgPool() {
    for (PGconn* c : connections_) {
        PQfinish(c);
    }
}

PgPool::Handle PgPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !available_.empty(); });
    PGconn* conn = available_.front();
    available_.pop();
    return Handle(this, conn);
}

void PgPool::release(PGconn* conn) {
    // Recover a connection that dropped (server restart, network blip) so the
    // next caller does not get a dead handle.
    if (PQstatus(conn) == CONNECTION_BAD) {
        ORE_LOG(WARN) << "PostgreSQL connection is bad; resetting";
        PQreset(conn);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        available_.push(conn);
    }
    cv_.notify_one();
}

}  // namespace Platform
}  // namespace Oreshnek
