// tests/db_test.cpp
//
// Fase 4 tests for the SQLite connection pool (WAL + busy_timeout): a basic
// CRUD roundtrip plus concurrent writers/readers across the pool, which under
// ThreadSanitizer also guards the pool's own synchronization.

#include "oreshnek/platform/DatabaseManager.h"

#include <atomic>
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
        DatabaseManager db(db_path, /*pool_size=*/4, /*busy_timeout_ms=*/5000);

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

        User missing = db.getUserByUsername("nobody");
        check(missing.id == 0, "unknown user returns id 0");

        // --- Concurrent writers across the pool ---------------------------------
        constexpr int kThreads = 6;
        constexpr int kPerThread = 25;
        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&db, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    Video v;
                    v.title = "video-" + std::to_string(t) + "-" + std::to_string(i);
                    v.filename = v.title + ".mp4";
                    v.user_id = 1;
                    v.is_public = true;
                    db.createVideo(v);
                }
            });
        }
        for (auto& th : writers) th.join();

        std::vector<Video> all = db.getVideos(/*limit=*/10000, /*offset=*/0);
        check(static_cast<int>(all.size()) == kThreads * kPerThread,
              "all concurrently-inserted videos are present");

        // --- Concurrent increments on one row (writer serialization) ------------
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
    }

    remove_db(db_path);

    if (g_failures == 0) {
        std::cout << "[OK] all db tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
