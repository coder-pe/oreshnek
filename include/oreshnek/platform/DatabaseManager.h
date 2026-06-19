// oreshnek/include/oreshnek/platform/DatabaseManager.h
#ifndef ORESHNEK_PLATFORM_DATABASE_MANAGER_H
#define ORESHNEK_PLATFORM_DATABASE_MANAGER_H

#include "oreshnek/platform/Config.h"        // ServerConfig
#include "oreshnek/platform/Models.h"
#include "oreshnek/platform/PgBackend.h"
#include "oreshnek/platform/SqliteBackend.h"

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace Oreshnek {
namespace Platform {

// Boundary over the concrete database backends. The active backend is chosen at
// runtime from the configuration and stored in a std::variant; every operation
// is dispatched with std::visit (no virtual). Add a backend by adding its type
// to the Backend variant and a branch in make_backend().
class DatabaseManager {
public:
    explicit DatabaseManager(const ServerConfig& config);

    bool createUser(const User& user);
    User getUserByUsername(const std::string& username);
    bool createVideo(const Video& video);
    std::vector<Video> getVideos(int limit = 20, int offset = 0, const std::string& category = "");
    bool incrementViews(int video_id);

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
