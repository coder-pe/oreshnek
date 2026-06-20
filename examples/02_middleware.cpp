// examples/02_middleware.cpp
//
// Use case: cross-cutting concerns via the middleware chain.
// Customization points shown: Server::use() ordering and short-circuit, the
// built-in middlewares (request_logger, cors, require_jwt) and a custom one.
//
//   curl -i localhost:8080/public
//   curl -i localhost:8080/admin                 # 401
//   TOKEN=$(curl -s localhost:8080/token | sed 's/.*"token":"//;s/".*//')
//   curl -i -H "Authorization: Bearer $TOKEN" localhost:8080/admin

#include "oreshnek/Oreshnek.h"
#include "oreshnek/server/Middleware.h"
#include "oreshnek/platform/SecurityUtils.h"
#include "common.h"

using namespace Oreshnek;

namespace {
const std::string kSecret = "example-secret-change-me";
}

int main() {
    Server::Server server(4);
    namespace MW = Server::Middlewares;

    // Middlewares run in registration order, before the matched handler.
    server.use(MW::request_logger());          // logs "METHOD /path"
    server.use(MW::cors("*"));                  // CORS headers + 204 preflight

    // A custom middleware: reject requests carrying a banned header. Returning
    // false short-circuits the chain (the handler never runs).
    server.use([](const HttpRequest& req, HttpResponse& res) -> bool {
        if (req.header("X-Banned")) {
            res.status(Http::HttpStatus::FORBIDDEN).text("banned");
            return false;
        }
        return true;
    });

    // Require a valid Bearer JWT only for paths under /admin.
    server.use(MW::require_jwt(kSecret, {"/admin"}));

    // Hand out a token to try the protected route.
    server.get("/token", [](const HttpRequest&, HttpResponse& res) {
        nlohmann::json j;
        j["token"] = Platform::SecurityUtils::generateJWT(1, "demo", kSecret);
        res.status(Http::HttpStatus::OK).json(j);
    });

    server.get("/public", [](const HttpRequest&, HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("anyone can read this\n");
    });

    server.get("/admin", [](const HttpRequest&, HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("secret admin area\n");
    });

    return ex::serve(server, "0.0.0.0", 8080);
}
