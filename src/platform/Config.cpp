// oreshnek/src/platform/Config.cpp
#include "oreshnek/platform/Config.h"
#include "oreshnek/utils/Logger.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace Oreshnek {
namespace Platform {

namespace {

// Assign config[key] into `out` if present and of a compatible type. Templated
// on the destination type so each field stays a single readable line below.
template <typename T>
void assign_if_present(const nlohmann::json& config, const char* key, T& out) {
    auto it = config.find(key);
    if (it != config.end() && !it->is_null()) {
        out = it->get<T>();
    }
}

// Read an environment variable; returns nullptr if unset/empty.
const char* env_or_null(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') return nullptr;
    return v;
}

}  // namespace

ServerConfig Config::load(const std::string& path) {
    ServerConfig cfg;  // defaults

    // --- 1) JSON file (optional) --------------------------------------------
    if (!path.empty()) {
        std::ifstream file(path);
        if (file.is_open()) {
            nlohmann::json config;
            try {
                file >> config;
            } catch (const std::exception& e) {
                throw std::runtime_error("Invalid config file '" + path + "': " + e.what());
            }
            if (!config.is_object()) {
                throw std::runtime_error("Config file '" + path + "' must contain a JSON object");
            }

            assign_if_present(config, "port", cfg.port);
            assign_if_present(config, "host", cfg.host);
            assign_if_present(config, "max_connections", cfg.max_connections);
            assign_if_present(config, "thread_pool_size", cfg.thread_pool_size);
            assign_if_present(config, "upload_dir", cfg.upload_dir);
            assign_if_present(config, "static_dir", cfg.static_dir);
            assign_if_present(config, "jwt_secret", cfg.jwt_secret);
            assign_if_present(config, "jwt_expire_hours", cfg.jwt_expire_hours);
            assign_if_present(config, "max_file_size", cfg.max_file_size);

            assign_if_present(config, "read_timeout_sec", cfg.read_timeout_sec);
            assign_if_present(config, "write_timeout_sec", cfg.write_timeout_sec);
            assign_if_present(config, "idle_timeout_sec", cfg.idle_timeout_sec);
            assign_if_present(config, "shutdown_grace_sec", cfg.shutdown_grace_sec);

            assign_if_present(config, "log_level", cfg.log_level);
            assign_if_present(config, "log_file", cfg.log_file);
            assign_if_present(config, "log_max_bytes", cfg.log_max_bytes);
            assign_if_present(config, "log_max_files", cfg.log_max_files);

            if (auto db = config.find("db"); db != config.end() && db->is_object()) {
                assign_if_present(*db, "backend", cfg.db.backend);
                if (auto s = db->find("sqlite"); s != db->end() && s->is_object()) {
                    assign_if_present(*s, "path", cfg.db.sqlite_path);
                    assign_if_present(*s, "pool_size", cfg.db.sqlite_pool_size);
                    assign_if_present(*s, "busy_timeout_ms", cfg.db.sqlite_busy_timeout_ms);
                }
                if (auto p = db->find("postgres"); p != db->end() && p->is_object()) {
                    assign_if_present(*p, "host", cfg.db.pg_host);
                    assign_if_present(*p, "port", cfg.db.pg_port);
                    assign_if_present(*p, "dbname", cfg.db.pg_dbname);
                    assign_if_present(*p, "user", cfg.db.pg_user);
                    assign_if_present(*p, "password", cfg.db.pg_password);
                    assign_if_present(*p, "sslmode", cfg.db.pg_sslmode);
                    assign_if_present(*p, "pool_size", cfg.db.pg_pool_size);
                    assign_if_present(*p, "connect_timeout_sec", cfg.db.pg_connect_timeout_sec);
                    assign_if_present(*p, "url", cfg.db.pg_url);
                }
            }

            if (auto tls = config.find("tls"); tls != config.end() && tls->is_object()) {
                assign_if_present(*tls, "enabled", cfg.tls.enabled);
                assign_if_present(*tls, "cert_file", cfg.tls.cert_file);
                assign_if_present(*tls, "key_file", cfg.tls.key_file);
                assign_if_present(*tls, "min_version", cfg.tls.min_version);
            }

            if (auto rl = config.find("rate_limit"); rl != config.end() && rl->is_object()) {
                assign_if_present(*rl, "enabled", cfg.rate_limit.enabled);
                assign_if_present(*rl, "requests_per_second", cfg.rate_limit.requests_per_second);
                assign_if_present(*rl, "burst", cfg.rate_limit.burst);
            }

            assign_if_present(config, "cors_enabled", cfg.cors_enabled);
            assign_if_present(config, "cors_allow_origin", cfg.cors_allow_origin);

            ORE_LOG(INFO) << "Loaded configuration from " << path;
        } else {
            ORE_LOG(WARN) << "Config file '" << path
                          << "' not found; using defaults and environment overrides";
        }
    }

    // --- 2) Environment overrides (secrets out of the file) -----------------
    if (const char* v = env_or_null("ORESHNEK_JWT_SECRET"))   cfg.jwt_secret = v;
    if (const char* v = env_or_null("ORESHNEK_HOST"))         cfg.host = v;
    if (const char* v = env_or_null("ORESHNEK_LOG_LEVEL"))    cfg.log_level = v;
    if (const char* v = env_or_null("ORESHNEK_LOG_FILE"))     cfg.log_file = v;
    if (const char* v = env_or_null("ORESHNEK_DB_BACKEND"))   cfg.db.backend = v;
    if (const char* v = env_or_null("ORESHNEK_DB_PATH"))      cfg.db.sqlite_path = v;
    if (const char* v = env_or_null("ORESHNEK_PG_PASSWORD"))  cfg.db.pg_password = v;
    if (const char* v = env_or_null("ORESHNEK_DATABASE_URL")) cfg.db.pg_url = v;
    if (const char* v = env_or_null("ORESHNEK_TLS_CERT"))     cfg.tls.cert_file = v;
    if (const char* v = env_or_null("ORESHNEK_TLS_KEY"))      cfg.tls.key_file = v;
    if (const char* v = env_or_null("ORESHNEK_PORT")) {
        try { cfg.port = std::stoi(v); } catch (const std::exception&) {
            ORE_LOG(WARN) << "Ignoring non-numeric ORESHNEK_PORT='" << v << "'";
        }
    }

    // Warn loudly if running with the bundled placeholder secret.
    if (cfg.jwt_secret == "your-super-secret-jwt-key-change-this") {
        ORE_LOG(WARN) << "JWT secret is the built-in default; set ORESHNEK_JWT_SECRET "
                         "or 'jwt_secret' in the config for production.";
    }

    return cfg;
}

}  // namespace Platform
}  // namespace Oreshnek
