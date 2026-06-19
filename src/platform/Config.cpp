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
            assign_if_present(config, "db_path", cfg.db_path);
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

            assign_if_present(config, "db_pool_size", cfg.db_pool_size);
            assign_if_present(config, "db_busy_timeout_ms", cfg.db_busy_timeout_ms);

            assign_if_present(config, "cors_enabled", cfg.cors_enabled);
            assign_if_present(config, "cors_allow_origin", cfg.cors_allow_origin);

            ORE_LOG(INFO) << "Loaded configuration from " << path;
        } else {
            ORE_LOG(WARN) << "Config file '" << path
                          << "' not found; using defaults and environment overrides";
        }
    }

    // --- 2) Environment overrides (secrets out of the file) -----------------
    if (const char* v = env_or_null("ORESHNEK_JWT_SECRET")) cfg.jwt_secret = v;
    if (const char* v = env_or_null("ORESHNEK_HOST"))       cfg.host = v;
    if (const char* v = env_or_null("ORESHNEK_DB_PATH"))    cfg.db_path = v;
    if (const char* v = env_or_null("ORESHNEK_LOG_LEVEL"))  cfg.log_level = v;
    if (const char* v = env_or_null("ORESHNEK_LOG_FILE"))   cfg.log_file = v;
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
