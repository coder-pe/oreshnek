// oreshnek/include/oreshnek/json/JsonValue.h
#ifndef ORESHNEK_JSON_JSONVALUE_H
#define ORESHNEK_JSON_JSONVALUE_H

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>    // For std::unique_ptr
#include <iostream>  // For ostream
#include <stdexcept> // For std::runtime_error

namespace Oreshnek {
namespace Json {

// JSON Value types
enum class JsonType {
    NULL_VALUE, BOOL, NUMBER, STRING, ARRAY, OBJECT
};

class JsonValue {
private:
    JsonType type_;
    union {
        bool bool_val;
        double num_val;
        std::string* str_val;
        std::vector<JsonValue>* arr_val;
        std::unordered_map<std::string, JsonValue>* obj_val;
    };

    void cleanup();
    void copy_from(const JsonValue& other);
    void move_from(JsonValue&& other);

    // Private helper methods for type conversion/initialization
    void make_array();
    void make_object();

public:
    // Constructors
    JsonValue() : type_(JsonType::NULL_VALUE) {}
    JsonValue(bool val) : type_(JsonType::BOOL), bool_val(val) {}
    JsonValue(double val) : type_(JsonType::NUMBER), num_val(val) {}
    JsonValue(int val) : type_(JsonType::NUMBER), num_val(static_cast<double>(val)) {}
    JsonValue(long val) : type_(JsonType::NUMBER), num_val(static_cast<double>(val)) {}
    JsonValue(long long val) : type_(JsonType::NUMBER), num_val(static_cast<double>(val)) {}
    JsonValue(const std::string& val) : type_(JsonType::STRING), str_val(new std::string(val)) {}
    JsonValue(std::string&& val) : type_(JsonType::STRING), str_val(new std::string(std::move(val))) {}
    JsonValue(const char* val) : type_(JsonType::STRING), str_val(new std::string(val)) {}

    // Copy constructor
    JsonValue(const JsonValue& other) : type_(JsonType::NULL_VALUE) {
        copy_from(other);
    }

    // Move constructor
    JsonValue(JsonValue&& other) noexcept : type_(JsonType::NULL_VALUE) {
        move_from(std::move(other));
    }

    // Destructor
    ~JsonValue() {
        cleanup();
    }

    // Assignment operators
    JsonValue& operator=(const JsonValue& other) {
        if (this != &other) {
            cleanup();
            copy_from(other);
        }
        return *this;
    }

    JsonValue& operator=(JsonValue&& other) noexcept {
        if (this != &other) {
            cleanup();
            move_from(std::move(other));
        }
        return *this;
    }

    // Type checking
    bool is_null() const { return type_ == JsonType::NULL_VALUE; }
    bool is_bool() const { return type_ == JsonType::BOOL; }
    bool is_number() const { return type_ == JsonType::NUMBER; }
    bool is_string() const { return type_ == JsonType::STRING; }
    bool is_array() const { return type_ == JsonType::ARRAY; }
    bool is_object() const { return type_ == JsonType::OBJECT; }

    // Getters with type checking
    bool get_bool() const {
        if (!is_bool()) throw std::runtime_error("JsonValue is not a boolean.");
        return bool_val;
    }
    double get_number() const {
        if (!is_number()) throw std::runtime_error("JsonValue is not a number.");
        return num_val;
    }
    const std::string& get_string() const {
        if (!is_string()) throw std::runtime_error("JsonValue is not a string.");
        return *str_val;
    }
    std::string& get_string() {
        if (!is_string()) throw std::runtime_error("JsonValue is not a string.");
        return *str_val;
    }
    const std::vector<JsonValue>& get_array() const {
        if (!is_array()) throw std::runtime_error("JsonValue is not an array.");
        return *arr_val;
    }
    std::vector<JsonValue>& get_array() {
        if (!is_array()) throw std::runtime_error("JsonValue is not an array.");
        return *arr_val;
    }
    const std::unordered_map<std::string, JsonValue>& get_object() const {
        if (!is_object()) throw std::runtime_error("JsonValue is not an object.");
        return *obj_val;
    }
    std::unordered_map<std::string, JsonValue>& get_object() {
        if (!is_object()) throw std::runtime_error("JsonValue is not an object.");
        return *obj_val;
    }

    // Array and object accessors (will auto-convert if null)
    JsonValue& operator[](size_t index); // For arrays
    const JsonValue& operator[](size_t index) const;
    JsonValue& operator[](const std::string& key); // For objects
    const JsonValue& operator[](const std::string& key) const;

    // Static factory methods for convenience
    static JsonValue array();
    static JsonValue object();

    // Size for arrays/objects
    size_t size() const;
    bool empty() const;

    // Convert to string (JSON serialization)
    std::string to_string(int indent = 0) const;

    // Parse from string (Forward declaration, actual implementation in JsonParser)
    // static JsonValue parse(const std::string& json_string); // No longer static method here,
                                                              // will be handled by JsonParser class

    // Friend declaration for output stream operator
    friend std::ostream& operator<<(std::ostream& os, const JsonValue& val);
    friend void serialize_to_string_recursive(std::ostream& os, const JsonValue& val, int indent_level, int indent_width);

};

// Helper for parsing JSON (declaration) - actual implementation in JsonParser.cpp
// This will be used by JsonValue::parse if we decide to keep a static parse method
// or directly by a separate JsonParser class.
// For now, removing static parse from JsonValue as suggested.

} // namespace Json
} // namespace Oreshnek

#endif // ORESHNEK_JSON_JSONVALUE_H
