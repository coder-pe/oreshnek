// oreshnek/src/json/JsonValue.cpp
#include "oreshnek/json/JsonValue.h"
#include <algorithm> // For std::fill
#include <iomanip>   // For std::quoted
#include <cassert>
#include <sstream> // Required for std::ostringstream

namespace Oreshnek {
namespace Json {

void JsonValue::cleanup() {
    switch (type_) {
        case JsonType::STRING:
            delete str_val;
            break;
        case JsonType::ARRAY:
            delete arr_val;
            break;
        case JsonType::OBJECT:
            delete obj_val;
            break;
        default:
            break;
    }
    type_ = JsonType::NULL_VALUE; // Reset type after cleanup
}

void JsonValue::copy_from(const JsonValue& other) {
    type_ = other.type_;
    switch (type_) {
        case JsonType::BOOL:
            bool_val = other.bool_val;
            break;
        case JsonType::NUMBER:
            num_val = other.num_val;
            break;
        case JsonType::STRING:
            str_val = new std::string(*other.str_val);
            break;
        case JsonType::ARRAY:
            arr_val = new std::vector<JsonValue>(*other.arr_val);
            break;
        case JsonType::OBJECT:
            obj_val = new std::unordered_map<std::string, JsonValue>(*other.obj_val);
            break;
        case JsonType::NULL_VALUE:
            // Nothing to do for null
            break;
    }
}

void JsonValue::move_from(JsonValue&& other) {
    type_ = other.type_;
    switch (type_) {
        case JsonType::BOOL:
            bool_val = other.bool_val;
            break;
        case JsonType::NUMBER:
            num_val = other.num_val;
            break;
        case JsonType::STRING:
            str_val = other.str_val;
            break;
        case JsonType::ARRAY:
            arr_val = other.arr_val;
            break;
        case JsonType::OBJECT:
            obj_val = other.obj_val;
            break;
        case JsonType::NULL_VALUE:
            // Nothing to do for null
            break;
    }
    other.type_ = JsonType::NULL_VALUE; // Mark other as moved-from
    other.str_val = nullptr; // Clear pointers in moved-from object
    other.arr_val = nullptr;
    other.obj_val = nullptr;
}

void JsonValue::make_array() {
    cleanup(); // Clean up current state if any
    type_ = JsonType::ARRAY;
    arr_val = new std::vector<JsonValue>();
}

void JsonValue::make_object() {
    cleanup(); // Clean up current state if any
    type_ = JsonType::OBJECT;
    obj_val = new std::unordered_map<std::string, JsonValue>();
}


JsonValue& JsonValue::operator[](size_t index) {
    if (type_ == JsonType::NULL_VALUE) {
        make_array();
    }
    if (!is_array()) {
        throw std::runtime_error("JsonValue is not an array, cannot access by index.");
    }
    if (index >= arr_val->size()) {
        arr_val->resize(index + 1);
    }
    return (*arr_val)[index];
}

const JsonValue& JsonValue::operator[](size_t index) const {
    if (!is_array()) {
        throw std::runtime_error("JsonValue is not an array, cannot access by index.");
    }
    if (index >= arr_val->size()) {
        throw std::out_of_range("Array index out of bounds.");
    }
    return (*arr_val)[index];
}

JsonValue& JsonValue::operator[](const std::string& key) {
    if (type_ == JsonType::NULL_VALUE) {
        make_object();
    }
    if (!is_object()) {
        throw std::runtime_error("JsonValue is not an object, cannot access by key.");
    }
    return (*obj_val)[key];
}

const JsonValue& JsonValue::operator[](const std::string& key) const {
    if (!is_object()) {
        throw std::runtime_error("JsonValue is not an object, cannot access by key.");
    }
    auto it = obj_val->find(key);
    if (it == obj_val->end()) {
        // Return a null value for non-existent keys in const context
        // This is a common pattern, but be aware it creates a temporary.
        // A better approach might be to return a pointer or optional.
        static const JsonValue null_val; // static to avoid re-creation
        return null_val;
    }
    return it->second;
}

JsonValue JsonValue::array() {
    JsonValue val;
    val.make_array(); // Use the new make_array helper
    return val;
}

JsonValue JsonValue::object() {
    JsonValue val;
    val.make_object(); // Use the new make_object helper
    return val;
}

size_t JsonValue::size() const {
    if (is_array()) {
        return arr_val->size();
    } else if (is_object()) {
        return obj_val->size();
    }
    return 0; // Or throw if not an array/object
}

bool JsonValue::empty() const {
    if (is_array()) {
        return arr_val->empty();
    } else if (is_object()) {
        return obj_val->empty();
    }
    return true; // Null, bool, number, string are considered non-empty for this context
}

// Helper for to_string
void serialize_to_string_recursive(std::ostream& os, const JsonValue& val, int indent_level, int indent_width) {
    std::string indent_str(indent_level * indent_width, ' ');
    std::string next_indent_str((indent_level + 1) * indent_width, ' ');

    if (val.is_null()) {
        os << "null";
    } else if (val.is_bool()) {
        os << (val.get_bool() ? "true" : "false");
    } else if (val.is_number()) {
        os << val.get_number();
    } else if (val.is_string()) {
        // Use std::quoted for proper JSON string escaping
        os << std::quoted(val.get_string());
    } else if (val.is_array()) {
        os << "[";
        if (val.get_array().empty()) {
            os << "]";
            // No break here, continue to add newline/indent if indent_width > 0 even for empty array
            // This is to maintain consistent formatting for empty array vs non-empty array
        } else {
            if (indent_width > 0) os << "\n";
            for (size_t i = 0; i < val.get_array().size(); ++i) {
                if (indent_width > 0) os << next_indent_str;
                serialize_to_string_recursive(os, val.get_array()[i], indent_level + 1, indent_width);
                if (i < val.get_array().size() - 1) {
                    os << ",";
                }
                if (indent_width > 0) os << "\n";
            }
            if (indent_width > 0) os << indent_str;
        }
        os << "]";
    } else if (val.is_object()) {
        os << "{";
        if (val.get_object().empty()) {
            os << "}";
            // No break here, continue to add newline/indent if indent_width > 0 even for empty object
        } else {
            if (indent_width > 0) os << "\n";
            bool first = true;
            for (const auto& pair : val.get_object()) {
                if (!first) {
                    os << ",";
                    if (indent_width > 0) os << "\n";
                }
                if (indent_width > 0) os << next_indent_str;
                os << std::quoted(pair.first) << ": ";
                serialize_to_string_recursive(os, pair.second, indent_level + 1, indent_width);
                first = false;
            }
            if (indent_width > 0) os << "\n" << indent_str;
        }
        os << "}";
    }
}

std::string JsonValue::to_string(int indent_width) const {
    std::ostringstream oss;
    serialize_to_string_recursive(oss, *this, 0, indent_width);
    return oss.str();
}

std::ostream& operator<<(std::ostream& os, const JsonValue& val) {
    serialize_to_string_recursive(os, val, 0, 0); // No indentation by default for operator<<
    return os;
}

} // namespace Json
} // namespace Oreshnek
