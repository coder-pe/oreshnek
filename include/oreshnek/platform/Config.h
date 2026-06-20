// oreshnek/include/oreshnek/platform/Config.h
#ifndef ORESHNEK_PLATFORM_CONFIG_H
#define ORESHNEK_PLATFORM_CONFIG_H

#include <cstddef>
#include <string>
#include <thread>

namespace Oreshnek {
namespace Platform {

// Database selection and per-backend settings. The active backend is chosen by
// `backend` ("sqlite" | "postgres"); only the matching sub-section is used.
struct DatabaseConfig {
    std::string backend = "sqlite"; // "sqlite" | "postgres"

    // SQLite backend.
    std::string sqlite_path = "./database.db";
    int sqlite_pool_size = 4;
    int sqlite_busy_timeout_ms = 5000;

    // PostgreSQL backend.
    std::string pg_host = "127.0.0.1";
    int pg_port = 5432;
    std::string pg_dbname = "oreshnek";
    std::string pg_user = "oreshnek";
    std::string pg_password;            // prefer ORESHNEK_PG_PASSWORD
    std::string pg_sslmode = "prefer";  // disable|allow|prefer|require|verify-full
    int pg_pool_size = 8;
    int pg_connect_timeout_sec = 5;
    // Full libpq connection URL; if set (e.g. via ORESHNEK_DATABASE_URL) it wins
    // over the individual pg_* fields above.
    std::string pg_url;
};

// TLS / HTTPS settings. When enabled, the listen socket speaks TLS (the server
// is HTTPS-only on its port).
struct TlsConfig {
    bool enabled = false;
    std::string cert_file;            // PEM certificate (chain), prefer ORESHNEK_TLS_CERT
    std::string key_file;             // PEM private key, prefer ORESHNEK_TLS_KEY
    std::string min_version = "1.2";  // "1.2" | "1.3"
};

// Per-IP token-bucket rate limiting.
struct RateLimitConfig {
    bool enabled = false;
    double requests_per_second = 50.0; // sustained refill rate
    double burst = 100.0;              // bucket capacity (peak allowance)
};

// Prometheus metrics endpoint.
struct MetricsConfig {
    bool enabled = false;
    std::string path = "/metrics";
};

// Runtime configuration, loadable from an external JSON file (see Config::load).
struct ServerConfig {
    int port = 8080;
    int max_connections = 1000;
    int thread_pool_size = std::thread::hardware_concurrency() * 2;
    std::string upload_dir = "./uploads/";
    std::string static_dir = "./static/";
    std::string jwt_secret = "your-super-secret-jwt-key-change-this";
    int jwt_expire_hours = 24;
    std::size_t max_file_size = 500 * 1024 * 1024; // 500MB
    std::string host = "0.0.0.0";

    // Connection timeouts (seconds). 0 disables the corresponding timeout.
    int read_timeout_sec = 30;     // Slow/incomplete request header+body -> 408.
    int write_timeout_sec = 30;    // Stalled response write -> drop connection.
    int idle_timeout_sec = 60;     // Idle keep-alive connection -> close.
    int shutdown_grace_sec = 10;   // Drain budget for graceful shutdown.

    // Logging.
    std::string log_level = "info";       // trace|debug|info|warn|error|off
    std::string log_file;                 // empty -> stderr (std::clog)
    std::size_t log_max_bytes = 10 * 1024 * 1024;
    int log_max_files = 5;

    // Persistence.
    DatabaseConfig db;

    // TLS / HTTPS.
    TlsConfig tls;

    // Rate limiting (per client IP, enforced in the event loop).
    RateLimitConfig rate_limit;

    // Prometheus metrics endpoint.
    MetricsConfig metrics;

    // CORS (applied by the built-in CORS middleware when enabled).
    bool cors_enabled = false;
    std::string cors_allow_origin = "*";
};

// Loads a ServerConfig from an external JSON file and environment overrides.
//
// Resolution order (later wins):
//   1. Built-in defaults (member initializers above).
//   2. JSON file at `path` (every key is optional; unknown keys are ignored).
//   3. Environment variables, so secrets stay out of the file/VCS:
//        ORESHNEK_JWT_SECRET, ORESHNEK_PORT, ORESHNEK_HOST, ORESHNEK_LOG_LEVEL,
//        ORESHNEK_LOG_FILE, ORESHNEK_DB_BACKEND, ORESHNEK_PG_PASSWORD,
//        ORESHNEK_DATABASE_URL.
//
// A missing file is not an error (defaults + env are used). A malformed file
// throws std::runtime_error so the operator notices instead of silently running
// with defaults.
class Config {
public:
    static ServerConfig load(const std::string& path);
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_CONFIG_H
