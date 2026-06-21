// oreshnek/src/platform/PgBackend.cpp
#include "oreshnek/platform/PgBackend.h"
#include "oreshnek/utils/Logger.h"

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

// Translate positional `?` placeholders to libpq's `$1, $2, ...`. A `?` inside a
// single-quoted string literal is data, not a placeholder, so it is left intact
// (with the standard '' escape handled). This lets applications write portable
// SQL once and run it on either backend.
std::string to_pg_placeholders(std::string_view sql) {
    std::string out;
    out.reserve(sql.size() + 8);
    int n = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (in_string) {
            out += c;
            if (c == '\'') {
                if (i + 1 < sql.size() && sql[i + 1] == '\'') {  // escaped quote ''
                    out += '\'';
                    ++i;
                } else {
                    in_string = false;
                }
            }
            continue;
        }
        if (c == '\'') {
            in_string = true;
            out += c;
        } else if (c == '?') {
            out += '$';
            out += std::to_string(++n);
        } else {
            out += c;
        }
    }
    return out;
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

}  // namespace

PgBackend::PgBackend(const DatabaseConfig& db)
    : pool_(build_conninfo(db), db.pg_pool_size) {}

SqlResult PgBackend::run_impl(std::string_view sql, const SqlParams& params) {
    SqlResult result;
    auto conn = pool_.acquire();
    const std::string translated = to_pg_placeholders(sql);

    // libpq takes a C-string array; nullptr means SQL NULL. The optional values
    // outlive the call, so pointing at their buffers is safe.
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) {
        values.push_back(p.has_value() ? p->c_str() : nullptr);
    }

    PgResult r(PQexecParams(conn, translated.c_str(), static_cast<int>(params.size()),
                            nullptr, params.empty() ? nullptr : values.data(),
                            nullptr, nullptr, 0));

    const ExecStatusType status = PQresultStatus(r);
    if (status == PGRES_TUPLES_OK) {
        const int cols = PQnfields(r);
        const int rows = PQntuples(r);
        result.columns.reserve(static_cast<std::size_t>(cols));
        for (int c = 0; c < cols; ++c) result.columns.emplace_back(PQfname(r, c));
        result.rows.reserve(static_cast<std::size_t>(rows));
        for (int i = 0; i < rows; ++i) {
            std::vector<std::optional<std::string>> row;
            row.reserve(static_cast<std::size_t>(cols));
            for (int c = 0; c < cols; ++c) {
                if (PQgetisnull(r, i, c)) {
                    row.emplace_back(std::nullopt);
                } else {
                    row.emplace_back(std::string(PQgetvalue(r, i, c),
                                                 static_cast<std::size_t>(PQgetlength(r, i, c))));
                }
            }
            result.rows.push_back(std::move(row));
        }
        result.ok = true;
    } else if (status == PGRES_COMMAND_OK) {
        result.ok = true;
        const char* tuples = PQcmdTuples(r);
        if (tuples && *tuples) {
            try { result.affected = std::stoll(tuples); } catch (...) {}
        }
        // PostgreSQL has no implicit last-insert-id; use `RETURNING id` and read
        // it from the result rows instead.
    } else {
        result.error = PQerrorMessage(conn);
        ORE_LOG(ERROR) << "PostgreSQL error: " << result.error;
    }
    return result;
}

}  // namespace Platform
}  // namespace Oreshnek
