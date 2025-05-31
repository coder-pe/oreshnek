// oreshnek/include/Oreshnek.h
#ifndef ORESHNEK_ORESHNEK_H
#define ORESHNEK_ORESHNEK_H

// Core components
#include "oreshnek/http/HttpEnums.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"
#include "oreshnek/http/HttpParser.h"

#include "oreshnek/json/JsonValue.h"
#include "oreshnek/json/JsonParser.h"

#include "oreshnek/net/Connection.h"

#include "oreshnek/server/Router.h"
#include "oreshnek/server/Server.h"
#include "oreshnek/server/ThreadPool.h"

// Define the top-level namespace alias for convenience
namespace Oreshnek {
    // Aliases to make usage simpler, mimicking original MiniRest structure
    using Server = Server::Server;
    using HttpRequest = Http::HttpRequest;
    using HttpResponse = Http::HttpResponse;
    using JsonValue = Json::JsonValue;
    using RouteHandler = Server::RouteHandler;

    // You can add more aliases here if you want to expose other components
    // directly under the Oreshnek namespace.
}

#endif // ORESHNEK_ORESHNEK_H
