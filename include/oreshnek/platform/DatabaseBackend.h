// oreshnek/include/oreshnek/platform/DatabaseBackend.h
//
// Static-polymorphism (no virtual) database contract. The framework is
// domain-agnostic: a backend exposes a single primitive that runs a
// parameterized statement and returns a generic SqlResult. Applications build
// their own repositories/models on top of this gateway.
//
// A concrete backend implements `run_impl`; the CRTP base DatabaseBase<Derived>
// exposes the public API and forwards to it, all resolved at compile time. The
// DatabaseBackend concept catches a missing/mismatched primitive early.
//
// Runtime backend selection (SQLite vs PostgreSQL vs ...) happens at the
// DatabaseManager boundary via std::variant + std::visit, also without vtables.
//
// The framework builds under C++17 or C++20 (ORESHNEK_CXX_STANDARD in
// CMakeLists.txt). Under C++20, DatabaseBackend is a real concept; under
// C++17 (no <concepts>/requires-expressions), it falls back to an equivalent
// SFINAE-based constexpr bool variable template with the same spelling, so
// `static_assert(DatabaseBackend<X>)` call sites need no changes either way.
#ifndef ORESHNEK_PLATFORM_DATABASE_BACKEND_H
#define ORESHNEK_PLATFORM_DATABASE_BACKEND_H

#include "oreshnek/platform/SqlResult.h"

#include <string_view>
#include <type_traits>

#if defined(__cpp_concepts) && __cpp_concepts >= 201907L
#define ORESHNEK_HAVE_CONCEPTS 1
#include <concepts>
#else
#define ORESHNEK_HAVE_CONCEPTS 0
#endif

namespace Oreshnek {
namespace Platform {

// The contract every backend must satisfy. `run_impl` is the single extension
// point; consumers call the DatabaseBase methods below.
#if ORESHNEK_HAVE_CONCEPTS

template <typename T>
concept DatabaseBackend = requires(T b, std::string_view sql, const SqlParams& params) {
    { b.run_impl(sql, params) } -> std::same_as<SqlResult>;
};

#else  // C++17: no concepts/requires-expressions available.

namespace detail {

template <typename T, typename = void>
struct DatabaseBackendTrait : std::false_type {};

template <typename T>
struct DatabaseBackendTrait<
    T, std::void_t<decltype(std::declval<T&>().run_impl(std::declval<std::string_view>(),
                                                         std::declval<const SqlParams&>()))>>
    : std::is_same<decltype(std::declval<T&>().run_impl(std::declval<std::string_view>(),
                                                         std::declval<const SqlParams&>())),
                   SqlResult> {};

}  // namespace detail

template <typename T>
inline constexpr bool DatabaseBackend = detail::DatabaseBackendTrait<T>::value;

#endif  // ORESHNEK_HAVE_CONCEPTS

// CRTP base: the stable, generic public API that forwards to the derived
// backend's implementation. Cross-cutting concerns (metrics, tracing, query
// logging) can be added here later without touching the concretes.
//
// Placeholders are written as `?` and bound positionally; the PostgreSQL backend
// translates them to `$1, $2, ...` so the same SQL runs on either backend.
template <typename Derived>
class DatabaseBase {
public:
    // Run any statement. For SELECT the result carries `columns`/`rows`; for
    // INSERT/UPDATE/DELETE/DDL it carries `affected` (and `last_insert_id` where
    // the backend supports it). query() and exec() are semantic aliases over the
    // same primitive — use whichever reads better at the call site.
    SqlResult query(std::string_view sql, const SqlParams& params = {}) {
        return self().run_impl(sql, params);
    }
    SqlResult exec(std::string_view sql, const SqlParams& params = {}) {
        return self().run_impl(sql, params);
    }

protected:
    Derived& self() { return static_cast<Derived&>(*this); }
    const Derived& self() const { return static_cast<const Derived&>(*this); }
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_DATABASE_BACKEND_H
