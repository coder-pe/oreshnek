// oreshnek/include/oreshnek/platform/PgBackend.h
#ifndef ORESHNEK_PLATFORM_PG_BACKEND_H
#define ORESHNEK_PLATFORM_PG_BACKEND_H

#include "oreshnek/platform/Config.h"  // DatabaseConfig
#include "oreshnek/platform/DatabaseBackend.h"
#include "oreshnek/platform/PgPool.h"

#include <string>
#include <vector>

namespace Oreshnek {
namespace Platform {

// PostgreSQL backend over libpq. Uses a connection pool and exclusively
// parameterized queries (PQexecParams with $n placeholders) — never string
// concatenation — to prevent SQL injection.
class PgBackend : public DatabaseBase<PgBackend> {
public:
    explicit PgBackend(const DatabaseConfig& db);

    // DatabaseBackend contract.
    void initialize_tables_impl();
    bool create_user_impl(const User& user);
    User user_by_username_impl(const std::string& username);
    bool create_video_impl(const Video& video);
    std::vector<Video> videos_impl(int limit, int offset, const std::string& category);
    bool increment_views_impl(int video_id);

private:
    PgPool pool_;
};

static_assert(DatabaseBackend<PgBackend>);

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_PG_BACKEND_H
