// oreshnek/include/oreshnek/json/JsonParser.h
#ifndef ORESHNEK_JSON_JSONPARSER_H
#define ORESHNEK_JSON_JSONPARSER_H

#include "oreshnek/json/JsonValue.h"
#include <string_view>

namespace Oreshnek {
namespace Json {

// Thin shim over nlohmann/json's parser, preserving the historical
// Json::JsonParser::parse(...) entry point. Throws nlohmann::json::parse_error
// (a std::exception) on invalid input.
class JsonParser {
public:
    static JsonValue parse(std::string_view json_string) {
        return JsonValue::parse(json_string.begin(), json_string.end());
    }
};

} // namespace Json
} // namespace Oreshnek

#endif // ORESHNEK_JSON_JSONPARSER_H
