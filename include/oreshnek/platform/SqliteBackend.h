// oreshnek/include/oreshnek/platform/SqliteBackend.h
#ifndef ORESHNEK_PLATFORM_SQLITE_BACKEND_H
#define ORESHNEK_PLATFORM_SQLITE_BACKEND_H

#include "oreshnek/platform/DatabaseBackend.h"
#include "oreshnek/platform/SqlitePool.h"

#include <string>
#include <vector>

namespace Oreshnek {
namespace Platform {

// SQLite3 backend: a WAL connection pool (concurrent readers + one writer) with
// per-operation checkout. Satisfies the DatabaseBackend contract via the
// `*_impl` surface; consumers use the DatabaseBase API.
class SqliteBackend : public DatabaseBase<SqliteBackend> {
public:
    SqliteBackend(const std::string& path, int pool_size, int busy_timeout_ms);

    // DatabaseBackend contract.
    void initialize_tables_impl();
    bool create_user_impl(const User& user);
    User user_by_username_impl(const std::string& username);
    bool create_video_impl(const Video& video);
    std::vector<Video> videos_impl(int limit, int offset, const std::string& category);
    bool increment_views_impl(int video_id);

private:
    SqlitePool pool_;
};

static_assert(DatabaseBackend<SqliteBackend>);

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_SQLITE_BACKEND_H
