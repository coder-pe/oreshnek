// oreshnek/include/oreshnek/json/JsonParser.h
#ifndef ORESHNEK_JSON_JSONPARSER_H
#define ORESHNEK_JSON_JSONPARSER_H

#include "oreshnek/json/JsonValue.h"
#include <string_view>
#include <stack>
#include <optional>

namespace Oreshnek {
namespace Json {

class JsonParser {
public:
    // Parses a JSON string and returns a JsonValue object.
    // Throws std::runtime_error on parsing errors.
    static JsonValue parse(std::string_view json_string);

private:
    // Internal state for parsing
    enum class ParserState {
        EXPECT_VALUE,
        EXPECT_KEY,
        EXPECT_COLON,
        EXPECT_VALUE_OR_END_OBJECT,
        EXPECT_VALUE_OR_END_ARRAY,
        EXPECT_COMMA_OR_END_OBJECT,
        EXPECT_COMMA_OR_END_ARRAY
    };

    // Helper functions for parsing different JSON types
    static JsonValue parse_value(std::string_view& data);
    static JsonValue parse_string(std::string_view& data);
    static JsonValue parse_number(std::string_view& data);
    static JsonValue parse_object(std::string_view& data);
    static JsonValue parse_array(std::string_view& data);
    static bool parse_keyword(std::string_view& data, std::string_view keyword);

    // Skip whitespace characters
    static void skip_whitespace(std::string_view& data);
};

} // namespace Json
} // namespace Oreshnek

#endif // ORESHNEK_JSON_JSONPARSER_H
