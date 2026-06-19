// oreshnek/include/oreshnek/server/Middleware.h
//
// Ready-made middlewares for the Server middleware chain (Server::use). Each
// factory returns a Middleware; register them in the order they should run:
//
//   server.use(Middlewares::request_logger());
//   server.use(Middlewares::cors("*"));
//   server.use(Middlewares::require_jwt(secret, {"/api/upload"}));
//
#ifndef ORESHNEK_SERVER_MIDDLEWARE_H
#define ORESHNEK_SERVER_MIDDLEWARE_H

#include "oreshnek/server/Server.h"  // Middleware type alias
#include "oreshnek/http/HttpEnums.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"
#include "oreshnek/platform/SecurityUtils.h"
#include "oreshnek/utils/Logger.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Oreshnek {
namespace Server {
namespace Middlewares {

// Adds permissive-by-default CORS headers to every response and answers CORS
// preflight (OPTIONS) requests with 204, short-circuiting the chain.
inline Middleware cors(std::string allow_origin = "*",
                       std::string allow_methods = "GET, POST, PUT, DELETE, PATCH, OPTIONS",
                       std::string allow_headers = "Content-Type, Authorization") {
    return [allow_origin = std::move(allow_origin),
            allow_methods = std::move(allow_methods),
            allow_headers = std::move(allow_headers)](
               const Http::HttpRequest& req, Http::HttpResponse& res) -> bool {
        res.header("Access-Control-Allow-Origin", allow_origin);
        res.header("Access-Control-Allow-Methods", allow_methods);
        res.header("Access-Control-Allow-Headers", allow_headers);
        if (req.method() == Http::HttpMethod::OPTIONS) {
            res.status(Http::HttpStatus::NO_CONTENT);
            res.header("Content-Length", "0");
            return false; // Preflight fully handled.
        }
        return true;
    };
}

// Logs one line per request (method + path) through the structured logger.
inline Middleware request_logger() {
    return [](const Http::HttpRequest& req, Http::HttpResponse&) -> bool {
        ORE_LOG(INFO) << "request " << Http::http_method_to_string(req.method()) << ' '
                      << req.path();
        return true;
    };
}

// Requires a valid "Authorization: Bearer <JWT>" on requests whose path starts
// with one of `protected_prefixes` (an empty list protects every route). On a
// missing/invalid token, responds 401 and stops the chain.
inline Middleware require_jwt(std::string secret, std::vector<std::string> protected_prefixes) {
    return [secret = std::move(secret), prefixes = std::move(protected_prefixes)](
               const Http::HttpRequest& req, Http::HttpResponse& res) -> bool {
        const std::string_view path = req.path();
        bool protectedRoute = prefixes.empty();
        for (const auto& prefix : prefixes) {
            if (path.substr(0, prefix.size()) == prefix) {
                protectedRoute = true;
                break;
            }
        }
        if (!protectedRoute) return true;

        std::string token;
        if (auto auth = req.header("Authorization")) {
            std::string value(*auth);
            if (value.rfind("Bearer ", 0) == 0) token = value.substr(7);
        }

        if (token.empty() || !Platform::SecurityUtils::validateJWT(token, secret)) {
            nlohmann::json err;
            err["success"] = false;
            err["message"] = "Unauthorized";
            res.status(Http::HttpStatus::UNAUTHORIZED).json(err);
            return false;
        }
        return true;
    };
}

}  // namespace Middlewares
}  // namespace Server
}  // namespace Oreshnek

#endif  // ORESHNEK_SERVER_MIDDLEWARE_H
