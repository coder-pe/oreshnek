// examples/common.h
//
// Tiny shared helper so each example can focus on the feature it demonstrates
// instead of repeating the (correct) startup/shutdown boilerplate.
#pragma once

#include "oreshnek/server/Server.h"

#include <csignal>
#include <iostream>
#include <string>

namespace ex {

inline Oreshnek::Server::Server* g_server = nullptr;
inline void on_signal(int /*sig*/) {
    // Async-signal-safe: just asks the event loop to drain and exit.
    if (g_server) g_server->request_stop();
}

// Wire signals, start listening and run the event loop until SIGINT/SIGTERM.
// Demonstrates the canonical lifecycle: request_stop() from the signal handler,
// run() drains in-flight work, then stop() tears the rest down on this thread.
inline int serve(Oreshnek::Server::Server& server,
                 const std::string& host = "0.0.0.0", int port = 8080) {
    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN); // writes to a closed peer return EPIPE, not a signal

    if (!server.listen(host, port)) {
        std::cerr << "failed to listen on " << host << ":" << port << std::endl;
        return 1;
    }
    std::cout << "Oreshnek example listening on " << host << ":" << port
              << " (Ctrl-C to stop)" << std::endl;
    server.run();  // blocks until request_stop()
    server.stop(); // graceful teardown on this thread
    return 0;
}

}  // namespace ex
