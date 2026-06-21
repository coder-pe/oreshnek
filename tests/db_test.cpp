// tests/db_test.cpp
//
// Tests for the SQLite connection pool (WAL + busy_timeout) through the generic
// SQL gateway: a basic CRUD roundtrip plus concurrent writers/readers across the
// pool, which under ThreadSanitizer also guards the pool's own synchronization.
// The framework is domain-agnostic, so the test owns its schema and SQL.

#include "oreshnek/platform/DatabaseManager.h"

#include <filesystem>
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

void remove_db(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + "-wal", ec);
    std::filesystem::remove(path + "-shm", ec);
}
}  // namespace

int main() {
    const std::string db_path = "oreshnek_db_test.db";
    remove_db(db_path);

    {
        ServerConfig cfg;
        cfg.db.backend = "sqlite";
        cfg.db.sqlite_path = db_path;
        cfg.db.sqlite_pool_size = 4;
        cfg.db.sqlite_busy_timeout_ms = 5000;
        DatabaseManager db(cfg);

        // The application owns its schema; the framework just runs the SQL.
        check(db.exec("CREATE TABLE IF NOT EXISTS widgets ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "name TEXT UNIQUE NOT NULL, "
                      "note TEXT, "  // nullable, for the NULL roundtrip below
                      "hits INTEGER NOT NULL DEFAULT 0);").ok,
              "create table");

        // --- Insert + parameterized read + unique constraint --------------------
        SqlResult ins = db.exec("INSERT INTO widgets (name) VALUES (?);", {"alice"});
        check(ins.ok && ins.affected == 1, "insert succeeds (affected == 1)");
        check(ins.last_insert_id > 0, "insert reports last_insert_id");

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

        // --- Concurrent increments on one row (writer serialization) ------------
        const long long wid = ins.last_insert_id;  // the "alice" row
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
    }

    remove_db(db_path);

    if (g_failures == 0) {
        std::cout << "[OK] all db tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
