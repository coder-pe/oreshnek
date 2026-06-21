// oreshnek/include/oreshnek/platform/DatabaseManager.h
#ifndef ORESHNEK_PLATFORM_DATABASE_MANAGER_H
#define ORESHNEK_PLATFORM_DATABASE_MANAGER_H

#include "oreshnek/platform/Config.h"        // ServerConfig
#include "oreshnek/platform/PgBackend.h"
#include "oreshnek/platform/SqlResult.h"
#include "oreshnek/platform/SqliteBackend.h"

#include <memory>
#include <string_view>
#include <variant>

namespace Oreshnek {
namespace Platform {

// Domain-agnostic boundary over the concrete database backends. The active
// backend is chosen at runtime from the configuration and stored in a
// std::variant; every call is dispatched with std::visit (no virtual).
//
// The manager exposes only the generic query()/exec() gateway — it deliberately
// knows nothing about application models. Build your own repository on top:
// run DDL with exec(), read rows with query(), and map SqlResult into your
// structs. Add a backend by extending the Backend variant and make_backend().
class DatabaseManager {
public:
    explicit DatabaseManager(const ServerConfig& config);

    // Run any statement against the configured backend. Placeholders are `?`,
    // bound positionally from `params` (std::nullopt => SQL NULL).
    SqlResult query(std::string_view sql, const SqlParams& params = {});
    SqlResult exec(std::string_view sql, const SqlParams& params = {});

private:
    // The pools own a mutex/condvar (non-movable), so the alternatives live
    // behind unique_ptr: this keeps the variant movable and lets us pick the
    // backend at runtime in make_backend(). Add a backend by extending this
    // variant and make_backend(); std::visit call sites stay untouched.
    using Backend = std::variant<std::unique_ptr<SqliteBackend>,
                                 std::unique_ptr<PgBackend>>;

    static Backend make_backend(const ServerConfig& config);

    Backend backend_;
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_DATABASE_MANAGER_H
