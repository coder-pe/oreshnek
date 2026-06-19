// oreshnek/include/oreshnek/platform/DatabaseBackend.h
//
// Static-polymorphism (no virtual) database contract. A concrete backend
// implements the `*_impl` methods; the CRTP base DatabaseBase<Derived> exposes
// the public API and forwards to them, all resolved at compile time. The
// DatabaseBackend concept catches a missing/mismatched operation early.
//
// Runtime backend selection (SQLite vs PostgreSQL vs ...) happens at the
// DatabaseManager boundary via std::variant + std::visit, also without vtables.
#ifndef ORESHNEK_PLATFORM_DATABASE_BACKEND_H
#define ORESHNEK_PLATFORM_DATABASE_BACKEND_H

#include "oreshnek/platform/Models.h"

#include <concepts>
#include <string>
#include <vector>

namespace Oreshnek {
namespace Platform {

// The contract every backend must satisfy. The `*_impl` surface is the
// extension point; consumers call the DatabaseBase methods below.
template <typename T>
concept DatabaseBackend = requires(T b, const User& user, const Video& video,
                                   const std::string& s, int n) {
    { b.initialize_tables_impl() }        -> std::same_as<void>;
    { b.create_user_impl(user) }          -> std::same_as<bool>;
    { b.user_by_username_impl(s) }        -> std::same_as<User>;
    { b.create_video_impl(video) }        -> std::same_as<bool>;
    { b.videos_impl(n, n, s) }            -> std::same_as<std::vector<Video>>;
    { b.increment_views_impl(n) }         -> std::same_as<bool>;
};

// CRTP base: public, stable API that forwards to the derived backend's
// implementation. Add cross-cutting concerns (metrics, tracing) here later
// without touching the concretes.
template <typename Derived>
class DatabaseBase {
public:
    void initializeTables() { self().initialize_tables_impl(); }

    bool createUser(const User& user) { return self().create_user_impl(user); }
    User getUserByUsername(const std::string& username) {
        return self().user_by_username_impl(username);
    }
    bool createVideo(const Video& video) { return self().create_video_impl(video); }
    std::vector<Video> getVideos(int limit, int offset, const std::string& category) {
        return self().videos_impl(limit, offset, category);
    }
    bool incrementViews(int video_id) { return self().increment_views_impl(video_id); }

protected:
    Derived& self() { return static_cast<Derived&>(*this); }
    const Derived& self() const { return static_cast<const Derived&>(*this); }
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_DATABASE_BACKEND_H
