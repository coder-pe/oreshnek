// oreshnek/include/oreshnek/json/JsonValue.h
#ifndef ORESHNEK_JSON_JSONVALUE_H
#define ORESHNEK_JSON_JSONVALUE_H

// Oreshnek uses nlohmann/json as its JSON engine. The historical
// Oreshnek::Json::JsonValue name is kept as an alias so existing call sites and
// the public API (HttpRequest::json(), HttpResponse::json(), JWT helpers) keep
// working, while gaining a mature, correct and fast implementation (notably
// proper integer handling instead of storing everything as double).
#include <nlohmann/json.hpp>

namespace Oreshnek {
namespace Json {

using JsonValue = nlohmann::json;

} // namespace Json
} // namespace Oreshnek

#endif // ORESHNEK_JSON_JSONVALUE_H
