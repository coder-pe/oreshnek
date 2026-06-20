// examples/01_hello_json.cpp
//
// Use case: a minimal JSON/HTTP service.
// Customization points shown: routing (static + path params), query parameters,
// reading a JSON request body, building JSON/text responses.
//
//   curl localhost:8080/
//   curl localhost:8080/hello/edgar
//   curl 'localhost:8080/hello/edgar?loud=1'
//   curl -d '{"x":1,"y":2}' localhost:8080/echo

#include "oreshnek/Oreshnek.h"
#include "common.h"

using namespace Oreshnek;

int main() {
    Server::Server server(4); // 4 worker threads

    // Static route.
    server.get("/", [](const HttpRequest&, HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("Hello from Oreshnek\n");
    });

    // Path parameter (:name) + optional query parameter (?loud=1).
    server.get("/hello/:name", [](const HttpRequest& req, HttpResponse& res) {
        const std::string name(req.param("name").value_or("world"));
        const bool loud = req.query("loud").value_or("") == "1";

        nlohmann::json j;
        j["greeting"] = loud ? "HELLO" : "hello";
        j["name"] = name;
        res.status(Http::HttpStatus::OK).json(j);
    });

    // Parse a JSON request body and echo it back.
    server.post("/echo", [](const HttpRequest& req, HttpResponse& res) {
        try {
            res.status(Http::HttpStatus::OK).json(req.json());
        } catch (const std::exception&) {
            nlohmann::json err;
            err["error"] = "invalid JSON body";
            res.status(Http::HttpStatus::BAD_REQUEST).json(err);
        }
    });

    return ex::serve(server, "0.0.0.0", 8080);
}
