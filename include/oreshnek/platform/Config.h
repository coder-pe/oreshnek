// oreshnek/include/oreshnek/platform/Config.h
#ifndef ORESHNEK_PLATFORM_CONFIG_H
#define ORESHNEK_PLATFORM_CONFIG_H

#include "oreshnek/platform/DatabaseManager.h"  // ServerConfig
#include <string>

namespace Oreshnek {
namespace Platform {

// Loads a ServerConfig from an external JSON file and environment overrides.
//
// Resolution order (later wins):
//   1. Built-in defaults (ServerConfig member initializers).
//   2. JSON file at `path` (every key is optional; unknown keys are ignored).
//   3. Environment variables, so secrets stay out of the file/VCS:
//        ORESHNEK_JWT_SECRET, ORESHNEK_PORT, ORESHNEK_HOST, ORESHNEK_DB_PATH,
//        ORESHNEK_LOG_LEVEL, ORESHNEK_LOG_FILE.
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
