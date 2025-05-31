// oreshnek/src/json/JsonParser.cpp
#include "oreshnek/json/JsonParser.h"
#include <charconv> // For std::from_chars (C++17 for numeric conversions)
#include <limits>   // For numeric_limits

namespace Oreshnek {
namespace Json {

void JsonParser::skip_whitespace(std::string_view& data) {
    size_t i = 0;
    while (i < data.length() && std::isspace(static_cast<unsigned char>(data[i]))) {
        i++;
    }
    data.remove_prefix(i);
}

JsonValue JsonParser::parse_string(std::string_view& data) {
    if (data.empty() || data[0] != '"') {
        throw std::runtime_error("Expected '\"' for string parsing.");
    }
    data.remove_prefix(1); // Consume leading '"'

    std::string s;
    s.reserve(data.length()); // Reserve approximate size to reduce reallocations

    size_t i = 0;
    while (i < data.length()) {
        char c = data[i];
        if (c == '"') {
            data.remove_prefix(i + 1); // Consume string content and closing '"'
            return JsonValue(s);
        } else if (c == '\\') {
            if (i + 1 >= data.length()) {
                throw std::runtime_error("Incomplete escape sequence in string.");
            }
            char escaped_char = data[i+1];
            switch (escaped_char) {
                case '"':  s += '"'; break;
                case '\\': s += '\\'; break;
                case '/':  s += '/'; break;
                case 'b':  s += '\b'; break;
                case 'f':  s += '\f'; break;
                case 'n':  s += '\n'; break;
                case 'r':  s += '\r'; break;
                case 't':  s += '\t'; break;
                case 'u': // Handle unicode escapes (basic ASCII for now, full unicode requires more)
                    // For full unicode, you'd need to parse 4 hex digits and convert to UTF-8
                    throw std::runtime_error("Unicode escape sequences \\u not fully supported yet.");
                default:
                    throw std::runtime_error("Invalid escape sequence in string: \\" + std::string(1, escaped_char));
            }
            i += 2; // Consume '\' and the escaped character
        } else {
            s += c;
            i++;
        }
    }
    throw std::runtime_error("Unterminated string.");
}

JsonValue JsonParser::parse_number(std::string_view& data) {
    size_t i = 0;
    // Numbers can start with -, digits, or . (if it's a fractional part only)
    if (data.empty()) throw std::runtime_error("Expected number, but input is empty.");

    // Find the end of the number
    while (i < data.length() && (std::isdigit(static_cast<unsigned char>(data[i])) ||
                                  data[i] == '-' || data[i] == '+' ||
                                  data[i] == '.' || data[i] == 'e' || data[i] == 'E')) {
        i++;
    }

    std::string_view num_str = data.substr(0, i);
    data.remove_prefix(i);

    double value;
    auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.length(), value);

    if (ec == std::errc()) {
        return JsonValue(value);
    } else if (ec == std::errc::invalid_argument) {
        throw std::runtime_error("Invalid number format: " + std::string(num_str));
    } else if (ec == std::errc::result_out_of_range) {
        throw std::runtime_error("Number out of range: " + std::string(num_str));
    } else {
        throw std::runtime_error("Failed to parse number: " + std::string(num_str));
    }
}

JsonValue JsonParser::parse_array(std::string_view& data) {
    if (data.empty() || data[0] != '[') {
        throw std::runtime_error("Expected '[' for array parsing.");
    }
    data.remove_prefix(1); // Consume '['
    skip_whitespace(data);

    JsonValue array_val = JsonValue::array();

    if (data.empty() || data[0] == ']') {
        data.remove_prefix(1); // Consume ']'
        return array_val;
    }

    while (true) {
        skip_whitespace(data);
        array_val.get_array().push_back(parse_value(data));
        skip_whitespace(data);

        if (data.empty()) {
            throw std::runtime_error("Unterminated array.");
        }
        if (data[0] == ']') {
            data.remove_prefix(1); // Consume ']'
            break;
        }
        if (data[0] == ',') {
            data.remove_prefix(1); // Consume ','
            skip_whitespace(data);
        } else {
            throw std::runtime_error("Expected ',' or ']' after array element.");
        }
    }
    return array_val;
}

JsonValue JsonParser::parse_object(std::string_view& data) {
    if (data.empty() || data[0] != '{') {
        throw std::runtime_error("Expected '{' for object parsing.");
    }
    data.remove_prefix(1); // Consume '{'
    skip_whitespace(data);

    JsonValue object_val = JsonValue::object();

    if (data.empty() || data[0] == '}') {
        data.remove_prefix(1); // Consume '}'
        return object_val;
    }

    while (true) {
        skip_whitespace(data);
        std::string key_str = parse_string(data).get_string(); // Key must be a string
        skip_whitespace(data);

        if (data.empty() || data[0] != ':') {
            throw std::runtime_error("Expected ':' after object key.");
        }
        data.remove_prefix(1); // Consume ':'
        skip_whitespace(data);

        object_val[key_str] = parse_value(data);
        skip_whitespace(data);

        if (data.empty()) {
            throw std::runtime_error("Unterminated object.");
        }
        if (data[0] == '}') {
            data.remove_prefix(1); // Consume '}'
            break;
        }
        if (data[0] == ',') {
            data.remove_prefix(1); // Consume ','
            skip_whitespace(data);
        } else {
            throw std::runtime_error("Expected ',' or '}' after object member.");
        }
    }
    return object_val;
}

bool JsonParser::parse_keyword(std::string_view& data, std::string_view keyword) {
    if (data.length() >= keyword.length() && data.substr(0, keyword.length()) == keyword) {
        data.remove_prefix(keyword.length());
        return true;
    }
    return false;
}

JsonValue JsonParser::parse_value(std::string_view& data) {
    skip_whitespace(data);

    if (data.empty()) {
        throw std::runtime_error("Unexpected end of JSON input.");
    }

    char first_char = data[0];

    if (first_char == '"') {
        return parse_string(data);
    } else if (first_char == '{') {
        return parse_object(data);
    } else if (first_char == '[') {
        return parse_array(data);
    } else if (first_char == '-' || std::isdigit(static_cast<unsigned char>(first_char))) {
        return parse_number(data);
    } else if (parse_keyword(data, "true")) {
        return JsonValue(true);
    } else if (parse_keyword(data, "false")) {
        return JsonValue(false);
    } else if (parse_keyword(data, "null")) {
        return JsonValue(); // Default constructor is NULL_VALUE
    } else {
        throw std::runtime_error("Unexpected character: " + std::string(1, first_char));
    }
}

JsonValue JsonParser::parse(std::string_view json_string) {
    std::string_view data = json_string; // Work with a mutable copy of the view
    JsonValue result = parse_value(data);
    skip_whitespace(data);
    if (!data.empty()) {
        throw std::runtime_error("Extra data after JSON document: " + std::string(data));
    }
    return result;
}

} // namespace Json
} // namespace Oreshnek
