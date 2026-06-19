// tests/pg_test.cpp
//
// Fase 5 tests for the PostgreSQL backend (libpq). Skipped unless a reachable
// PostgreSQL is provided via ORESHNEK_PG_TEST_DSN (a libpq conninfo string or
// URL, e.g. "postgresql://user:pass@localhost/oreshnek_test"). When set, it runs
// the same assertions as db_test against PostgreSQL, starting from a clean slate.

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

// Drop the schema so the run is idempotent; DatabaseManager recreates the tables.
bool reset_schema(const char* dsn) {
    PGconn* c = PQconnectdb(dsn);
    if (PQstatus(c) != CONNECTION_OK) {
        std::cerr << "[FAIL] pg_test cannot connect: " << PQerrorMessage(c);
        PQfinish(c);
        return false;
    }
    PQclear(PQexec(c, "DROP TABLE IF EXISTS sessions, comments, videos, users CASCADE;"));
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

    // --- User roundtrip + unique constraint ---------------------------------
    User u;
    u.username = "alice";
    u.email = "alice@example.com";
    u.password_hash = "hash";
    u.role = "student";
    check(db.createUser(u), "createUser succeeds");

    User got = db.getUserByUsername("alice");
    check(got.id != 0, "getUserByUsername finds the user");
    check(got.username == "alice" && got.email == "alice@example.com",
          "user fields roundtrip");
    check(!db.createUser(u), "duplicate username/email rejected");
    check(db.getUserByUsername("nobody").id == 0, "unknown user returns id 0");

    // --- Concurrent writers across the pool ---------------------------------
    const int uid = got.id;  // FK: videos.user_id references an existing user
    constexpr int kThreads = 6;
    constexpr int kPerThread = 25;
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&db, uid, t] {
            for (int i = 0; i < kPerThread; ++i) {
                Video v;
                v.title = "video-" + std::to_string(t) + "-" + std::to_string(i);
                v.filename = v.title + ".mp4";
                v.user_id = uid;
                v.is_public = true;
                db.createVideo(v);
            }
        });
    }
    for (auto& th : writers) th.join();

    std::vector<Video> all = db.getVideos(10000, 0);
    check(static_cast<int>(all.size()) == kThreads * kPerThread,
          "all concurrently-inserted videos are present");

    // --- Concurrent increments on one row -----------------------------------
    check(!all.empty(), "have a video to increment");
    if (!all.empty()) {
        const int video_id = all.front().id;
        constexpr int kIncThreads = 5;
        constexpr int kIncEach = 20;
        std::vector<std::thread> bumpers;
        for (int t = 0; t < kIncThreads; ++t) {
            bumpers.emplace_back([&db, video_id] {
                for (int i = 0; i < kIncEach; ++i) db.incrementViews(video_id);
            });
        }
        for (auto& th : bumpers) th.join();

        int views = -1;
        for (const auto& v : db.getVideos(10000, 0)) {
            if (v.id == video_id) { views = v.views; break; }
        }
        check(views == kIncThreads * kIncEach,
              "incrementViews is exact under concurrency (no lost updates)");
    }

    if (g_failures == 0) {
        std::cout << "[OK] all pg tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
