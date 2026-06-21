// tests/pg_test.cpp
//
// Tests for the PostgreSQL backend (libpq) through the generic SQL gateway.
// Skipped unless a reachable PostgreSQL is provided via ORESHNEK_PG_TEST_DSN (a
// libpq conninfo string or URL, e.g.
// "postgresql://user:pass@localhost/oreshnek_test"). When set, it runs the same
// assertions as db_test against PostgreSQL, starting from a clean slate. The
// framework is domain-agnostic, so the test owns its schema and SQL.

#include "oreshnek/platform/DatabaseManager.h"

#include <libpq-fe.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace Oreshnek::Platform;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}

// Drop the test table so the run is idempotent.
bool reset_schema(const char* dsn) {
    PGconn* c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        std::cerr << "[FAIL] pg_test cannot connect: " << PQerrorMessage(c);
        PQfinish(c);
        return false;
    }
    PQclear(PQexec(c, "DROP TABLE IF EXISTS widgets CASCADE;"));
    PQfinish(c);
    return true;
}
}  // namespace

int main() {
    const char* dsn = std::getenv("ORESHNEK_PG_TEST_DSN");
    if (dsn == nullptr || dsn[0] == '\0') {
        std::cout << "[SKIP] pg_test: set ORESHNEK_PG_TEST_DSN to run against PostgreSQL"
                  << std::endl;
        return 0;
    }
    if (!reset_schema(dsn)) return 1;

    ServerConfig cfg;
    cfg.db.backend = "postgres";
    cfg.db.pg_url = dsn;
    cfg.db.pg_pool_size = 4;
    DatabaseManager db(cfg);

    // The application owns its schema; the framework just runs the SQL. Note the
    // PostgreSQL dialect (SERIAL) — only the SQL changes, not the API.
    check(db.exec("CREATE TABLE IF NOT EXISTS widgets ("
                  "id SERIAL PRIMARY KEY, "
                  "name TEXT UNIQUE NOT NULL, "
                  "note TEXT, "  // nullable, for the NULL roundtrip below
                  "hits INTEGER NOT NULL DEFAULT 0);").ok,
          "create table");

    // --- Insert + parameterized read + unique constraint --------------------
    // PostgreSQL has no implicit last-insert-id; RETURNING brings the id back.
    SqlResult ins = db.query("INSERT INTO widgets (name) VALUES (?) RETURNING id;", {"alice"});
    check(ins.ok && ins.row_count() == 1, "insert returns the new id");
    const long long wid = ins.integer(0, 0);
    check(wid > 0, "RETURNING id is positive");

    SqlResult got = db.query("SELECT id, name, hits FROM widgets WHERE name = ?;", {"alice"});
    check(got.ok && got.row_count() == 1, "select finds the row");
    check(got.text(0, got.column("name")) == "alice", "name roundtrips");
    check(got.integer(0, got.column("hits")) == 0, "default hits == 0");

    check(!db.exec("INSERT INTO widgets (name) VALUES (?);", {"alice"}).ok,
          "duplicate name rejected (unique constraint)");
    check(db.query("SELECT id FROM widgets WHERE name = ?;", {"nobody"}).empty(),
          "unknown row yields no tuples");

    // --- NULL roundtrip -----------------------------------------------------
    db.exec("INSERT INTO widgets (name, note) VALUES (?, ?);", {"nullable", std::nullopt});
    SqlResult nul = db.query("SELECT note FROM widgets WHERE name = ?;", {"nullable"});
    check(nul.ok && nul.row_count() == 1 && nul.is_null(0, 0), "SQL NULL roundtrips");

    // --- Concurrent writers across the pool ---------------------------------
    constexpr int kThreads = 6;
    constexpr int kPerThread = 25;
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&db, t] {
            for (int i = 0; i < kPerThread; ++i) {
                db.exec("INSERT INTO widgets (name) VALUES (?);",
                        {"w-" + std::to_string(t) + "-" + std::to_string(i)});
            }
        });
    }
    for (auto& th : writers) th.join();

    SqlResult count = db.query("SELECT COUNT(*) FROM widgets;");
    check(count.ok && count.integer(0, 0) == kThreads * kPerThread + 2,
          "all concurrently-inserted rows are present");

    // --- Concurrent increments on one row -----------------------------------
    constexpr int kIncThreads = 5;
    constexpr int kIncEach = 20;
    std::vector<std::thread> bumpers;
    for (int t = 0; t < kIncThreads; ++t) {
        bumpers.emplace_back([&db, wid] {
            for (int i = 0; i < kIncEach; ++i) {
                db.exec("UPDATE widgets SET hits = hits + 1 WHERE id = ?;",
                        {std::to_string(wid)});
            }
        });
    }
    for (auto& th : bumpers) th.join();

    SqlResult hits = db.query("SELECT hits FROM widgets WHERE id = ?;", {std::to_string(wid)});
    check(hits.ok && hits.integer(0, 0) == kIncThreads * kIncEach,
          "increments are exact under concurrency (no lost updates)");

    if (g_failures == 0) {
        std::cout << "[OK] all pg tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
