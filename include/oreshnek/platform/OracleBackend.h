// oreshnek/include/oreshnek/platform/OracleBackend.h
#ifndef ORESHNEK_PLATFORM_ORACLE_BACKEND_H
#define ORESHNEK_PLATFORM_ORACLE_BACKEND_H

#include "oreshnek/platform/Config.h"  // DatabaseConfig
#include "oreshnek/platform/DatabaseBackend.h"
#include "oreshnek/platform/OraclePool.h"

#include <string>
#include <string_view>

namespace Oreshnek {
namespace Platform {

// Oracle backend over OCI (Oracle Call Interface), via the Instant Client SDK.
// Implements the generic DatabaseBackend primitive (`run_impl`) with
// exclusively bound-variable statements (OCIBindByPos) — never string
// concatenation — to prevent SQL injection. Positional `?` placeholders in the
// incoming SQL are translated to Oracle's `:1, :2, ...` bind syntax so the same
// statements run unchanged across backends. Every column is fetched as text
// (SQLT_STR), keeping SqlResult's driver-agnostic text representation; CLOB/
// BLOB columns are not supported by this text-only fetch path.
class OracleBackend : public DatabaseBase<OracleBackend> {
public:
    explicit OracleBackend(const DatabaseConfig& db);

    SqlResult run_impl(std::string_view sql, const SqlParams& params);

private:
    OraclePool pool_;
};

static_assert(DatabaseBackend<OracleBackend>);

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_ORACLE_BACKEND_H
