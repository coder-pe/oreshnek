// oreshnek/src/platform/PgBackend.cpp
#include "oreshnek/platform/PgBackend.h"
#include "oreshnek/utils/Logger.h"

#include <cstdlib>
#include <sstream>

namespace Oreshnek {
namespace Platform {

namespace {

// Quote a libpq conninfo value, escaping backslash and single quote.
std::string quote(const std::string& v) {
    std::string out = "'";
    for (char c : v) {
        if (c == '\\' || c == '\'') out += '\\';
        out += c;
    }
    out += "'";
    return out;
}

// Build a libpq conninfo string from the configuration. A full URL
// (db.pg_url / ORESHNEK_DATABASE_URL) takes precedence and is used verbatim.
std::string build_conninfo(const DatabaseConfig& db) {
    if (!db.pg_url.empty()) return db.pg_url;
    std::ostringstream c;
    c << "host=" << quote(db.pg_host)
      << " port=" << db.pg_port
      << " dbname=" << quote(db.pg_dbname)
      << " user=" << quote(db.pg_user)
      << " password=" << quote(db.pg_password)
      << " sslmode=" << quote(db.pg_sslmode)
      << " connect_timeout=" << db.pg_connect_timeout_sec;
    return c.str();
}

// RAII for PGresult.
class PgResult {
public:
    explicit PgResult(PGresult* r) : r_(r) {}
    ~PgResult() { if (r_) PQclear(r_); }
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    operator PGresult*() const { return r_; }
    PGresult* get() const { return r_; }
private:
    PGresult* r_;
};

// PostgreSQL renders booleans as "t"/"f" in text result format.
bool pg_bool(const char* v) { return v != nullptr && v[0] == 't'; }

}  // namespace

PgBackend::PgBackend(const DatabaseConfig& db)
    : pool_(build_conninfo(db), db.pg_pool_size) {}

void PgBackend::initialize_tables_impl() {
    auto conn = pool_.acquire();
    static const char* kDdl[] = {
        R"(CREATE TABLE IF NOT EXISTS users (
               id SERIAL PRIMARY KEY,
               username TEXT UNIQUE NOT NULL,
               email TEXT UNIQUE NOT NULL,
               password_hash TEXT NOT NULL,
               role TEXT DEFAULT 'student',
               created_at TIMESTAMPTZ DEFAULT now(),
               is_active BOOLEAN DEFAULT TRUE
           );)",
        R"(CREATE TABLE IF NOT EXISTS videos (
               id SERIAL PRIMARY KEY,
               title TEXT NOT NULL,
               description TEXT,
               filename TEXT NOT NULL,
               thumbnail TEXT,
               user_id INTEGER REFERENCES users(id),
               category TEXT,
               tags TEXT,
               views INTEGER DEFAULT 0,
               likes INTEGER DEFAULT 0,
               duration TEXT,
               is_public BOOLEAN DEFAULT TRUE,
               created_at TIMESTAMPTZ DEFAULT now()
           );)",
        R"(CREATE TABLE IF NOT EXISTS comments (
               id SERIAL PRIMARY KEY,
               video_id INTEGER REFERENCES videos(id),
               user_id INTEGER REFERENCES users(id),
               content TEXT NOT NULL,
               parent_id INTEGER REFERENCES comments(id),
               created_at TIMESTAMPTZ DEFAULT now()
           );)",
        R"(CREATE TABLE IF NOT EXISTS sessions (
               id SERIAL PRIMARY KEY,
               user_id INTEGER REFERENCES users(id),
               token TEXT UNIQUE NOT NULL,
               expires_at TIMESTAMPTZ NOT NULL,
               created_at TIMESTAMPTZ DEFAULT now()
           );)",
    };
    for (const char* ddl : kDdl) {
        PgResult r(PQexec(conn, ddl));
        if (PQresultStatus(r) != PGRES_COMMAND_OK) {
            ORE_LOG(ERROR) << "PG initializeTables error: " << PQerrorMessage(conn);
        }
    }
}

bool PgBackend::create_user_impl(const User& user) {
    auto conn = pool_.acquire();
    const char* params[4] = {user.username.c_str(), user.email.c_str(),
                             user.password_hash.c_str(), user.role.c_str()};
    PgResult r(PQexecParams(
        conn,
        "INSERT INTO users (username, email, password_hash, role) VALUES ($1, $2, $3, $4)",
        4, nullptr, params, nullptr, nullptr, 0));
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        ORE_LOG(ERROR) << "PG createUser error: " << PQerrorMessage(conn);
        return false;
    }
    return true;
}

User PgBackend::user_by_username_impl(const std::string& username) {
    auto conn = pool_.acquire();
    const char* params[1] = {username.c_str()};
    PgResult r(PQexecParams(
        conn,
        "SELECT id, username, email, password_hash, role, created_at, is_active "
        "FROM users WHERE username = $1",
        1, nullptr, params, nullptr, nullptr, 0));
    User u{};
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
        u.id = std::atoi(PQgetvalue(r, 0, 0));
        u.username = PQgetvalue(r, 0, 1);
        u.email = PQgetvalue(r, 0, 2);
        u.password_hash = PQgetvalue(r, 0, 3);
        u.role = PQgetvalue(r, 0, 4);
        u.created_at = PQgetvalue(r, 0, 5);
        u.is_active = pg_bool(PQgetvalue(r, 0, 6));
    }
    return u;
}

bool PgBackend::create_video_impl(const Video& video) {
    auto conn = pool_.acquire();

    std::string tags_str;
    for (const auto& tag : video.tags) {
        if (!tags_str.empty()) tags_str += ",";
        tags_str += tag;
    }
    const std::string user_id = std::to_string(video.user_id);
    const std::string is_public = video.is_public ? "true" : "false";

    const char* params[9] = {video.title.c_str(),     video.description.c_str(),
                             video.filename.c_str(),   video.thumbnail.c_str(),
                             user_id.c_str(),          video.category.c_str(),
                             tags_str.c_str(),         video.duration.c_str(),
                             is_public.c_str()};
    PgResult r(PQexecParams(
        conn,
        "INSERT INTO videos (title, description, filename, thumbnail, user_id, "
        "category, tags, duration, is_public) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)",
        9, nullptr, params, nullptr, nullptr, 0));
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        ORE_LOG(ERROR) << "PG createVideo error: " << PQerrorMessage(conn);
        return false;
    }
    return true;
}

std::vector<Video> PgBackend::videos_impl(int limit, int offset, const std::string& category) {
    auto conn = pool_.acquire();

    // Build the query and the matching positional parameters.
    std::string sql =
        "SELECT id, title, description, filename, thumbnail, user_id, category, "
        "tags, views, likes, created_at, duration, is_public "
        "FROM videos WHERE is_public = TRUE";
    std::vector<std::string> values;
    if (!category.empty()) {
        values.push_back(category);
        sql += " AND category = $1 ORDER BY created_at DESC LIMIT $2 OFFSET $3";
    } else {
        sql += " ORDER BY created_at DESC LIMIT $1 OFFSET $2";
    }
    values.push_back(std::to_string(limit));
    values.push_back(std::to_string(offset));

    std::vector<const char*> params;
    params.reserve(values.size());
    for (const auto& v : values) params.push_back(v.c_str());

    PgResult r(PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()), nullptr,
                            params.data(), nullptr, nullptr, 0));
    std::vector<Video> videos;
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        ORE_LOG(ERROR) << "PG getVideos error: " << PQerrorMessage(conn);
        return videos;
    }

    const int rows = PQntuples(r);
    for (int i = 0; i < rows; ++i) {
        Video v;
        v.id = std::atoi(PQgetvalue(r, i, 0));
        v.title = PQgetvalue(r, i, 1);
        v.description = PQgetvalue(r, i, 2);
        v.filename = PQgetvalue(r, i, 3);
        v.thumbnail = PQgetvalue(r, i, 4);
        v.user_id = std::atoi(PQgetvalue(r, i, 5));
        v.category = PQgetvalue(r, i, 6);
        std::string tags_str = PQgetvalue(r, i, 7);
        if (!tags_str.empty()) {
            std::stringstream ss(tags_str);
            std::string tag;
            while (std::getline(ss, tag, ',')) v.tags.push_back(tag);
        }
        v.views = std::atoi(PQgetvalue(r, i, 8));
        v.likes = std::atoi(PQgetvalue(r, i, 9));
        v.created_at = PQgetvalue(r, i, 10);
        v.duration = PQgetvalue(r, i, 11);
        v.is_public = pg_bool(PQgetvalue(r, i, 12));
        videos.push_back(std::move(v));
    }
    return videos;
}

bool PgBackend::increment_views_impl(int video_id) {
    auto conn = pool_.acquire();
    const std::string id = std::to_string(video_id);
    const char* params[1] = {id.c_str()};
    PgResult r(PQexecParams(conn, "UPDATE videos SET views = views + 1 WHERE id = $1",
                            1, nullptr, params, nullptr, nullptr, 0));
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        ORE_LOG(ERROR) << "PG incrementViews error: " << PQerrorMessage(conn);
        return false;
    }
    return true;
}

}  // namespace Platform
}  // namespace Oreshnek
