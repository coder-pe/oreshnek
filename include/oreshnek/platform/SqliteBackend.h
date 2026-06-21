// oreshnek/include/oreshnek/platform/SqliteBackend.h
#ifndef ORESHNEK_PLATFORM_SQLITE_BACKEND_H
#define ORESHNEK_PLATFORM_SQLITE_BACKEND_H

#include "oreshnek/platform/DatabaseBackend.h"
#include "oreshnek/platform/SqlitePool.h"

#include <string>
#include <string_view>

namespace Oreshnek {
namespace Platform {

// SQLite3 backend: a WAL connection pool (concurrent readers + one writer) with
// per-operation checkout. Implements the generic DatabaseBackend primitive
// (`run_impl`); consumers use the DatabaseBase query()/exec() API.
class SqliteBackend : public DatabaseBase<SqliteBackend> {
public:
    SqliteBackend(const std::string& path, int pool_size, int busy_timeout_ms);

    // DatabaseBackend contract: prepare, bind `params` positionally (`?`), and
    // run, collecting any result rows as text.
    SqlResult run_impl(std::string_view sql, const SqlParams& params);

private:
    SqlitePool pool_;
};

static_assert(DatabaseBackend<SqliteBackend>);

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_SQLITE_BACKEND_H
