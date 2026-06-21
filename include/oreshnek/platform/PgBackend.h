// oreshnek/include/oreshnek/platform/PgBackend.h
#ifndef ORESHNEK_PLATFORM_PG_BACKEND_H
#define ORESHNEK_PLATFORM_PG_BACKEND_H

#include "oreshnek/platform/Config.h"  // DatabaseConfig
#include "oreshnek/platform/DatabaseBackend.h"
#include "oreshnek/platform/PgPool.h"

#include <string>
#include <string_view>

namespace Oreshnek {
namespace Platform {

// PostgreSQL backend over libpq. Implements the generic DatabaseBackend
// primitive (`run_impl`) with exclusively parameterized queries (PQexecParams) —
// never string concatenation — to prevent SQL injection. Positional `?`
// placeholders in the incoming SQL are translated to libpq's `$n` form so the
// same statements run unchanged on either backend.
class PgBackend : public DatabaseBase<PgBackend> {
public:
    explicit PgBackend(const DatabaseConfig& db);

    SqlResult run_impl(std::string_view sql, const SqlParams& params);

private:
    PgPool pool_;
};

static_assert(DatabaseBackend<PgBackend>);

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_PG_BACKEND_H
