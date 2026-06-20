// examples/05_production.cpp
//
// Use case: a production-shaped setup wiring together everything from the
// hardening phases.
// Customization points shown: external configuration (Config::load), structured
// logging, connection timeouts + graceful shutdown, optional TLS, per-IP rate
// limiting and Prometheus /metrics.
//
//   ./05_production [config.json]      # or ORESHNEK_CONFIG / ./oreshnek.json
// See config/oreshnek.example.json for every knob.

#include "oreshnek/Oreshnek.h"
#include "oreshnek/server/Middleware.h"
#include "oreshnek/platform/Config.h"
#include "oreshnek/utils/Logger.h"
#include "common.h"

#include <cstdlib>

using namespace Oreshnek;

int main(int argc, char** argv) {
    // 1) Load configuration: argv[1] > $ORESHNEK_CONFIG > ./oreshnek.json.
    std::string path = "oreshnek.json";
    if (argc > 1) path = argv[1];
    else if (const char* env = std::getenv("ORESHNEK_CONFIG")) path = env;
    Platform::ServerConfig cfg = Platform::Config::load(path);

    // 2) Structured logging from config (level + optional rotating file sink).
    auto& logger = Utils::Logger::instance();
    logger.set_level(Utils::level_from_string(cfg.log_level));
    if (!cfg.log_file.empty()) logger.set_file(cfg.log_file, cfg.log_max_bytes, cfg.log_max_files);

    Server::Server server(static_cast<size_t>(cfg.thread_pool_size));

    // 3) Timeouts + graceful-shutdown grace period.
    server.configure(Server::Server::Settings{
        cfg.read_timeout_sec, cfg.write_timeout_sec, cfg.idle_timeout_sec,
        cfg.shutdown_grace_sec, cfg.handler_timeout_sec});

    // 4) Optional HTTPS (fails fast on a bad cert/key).
    if (cfg.tls.enabled && !cfg.tls.cert_file.empty() && !cfg.tls.key_file.empty()) {
        server.enable_tls(cfg.tls.cert_file, cfg.tls.key_file, cfg.tls.min_version);
    }
    // 5) Optional per-IP rate limiting and Prometheus metrics.
    if (cfg.rate_limit.enabled) {
        server.enable_rate_limit(cfg.rate_limit.requests_per_second, cfg.rate_limit.burst);
    }
    if (cfg.metrics.enabled) server.enable_metrics(cfg.metrics.path);

    // 6) Cross-cutting middleware.
    server.use(Server::Middlewares::request_logger());
    if (cfg.cors_enabled) server.use(Server::Middlewares::cors(cfg.cors_allow_origin));

    // 7) Routes.
    server.get("/healthz", [](const HttpRequest&, HttpResponse& res) {
        res.status(Http::HttpStatus::OK).json({{"status", "ok"}});
    });

    ORE_LOG(INFO) << "starting on " << cfg.host << ":" << cfg.port;
    return ex::serve(server, cfg.host, cfg.port);
}
