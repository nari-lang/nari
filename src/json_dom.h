#pragma once

// replacement for nlohmann json, since it has a lot of things nari just flat out doesn't need

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Int,
        UInt,
        Float,
        String,
        Array,
        Object,
        Discarded,
    };

    Type type = Type::Null;
    bool tp_boolean = false;
    int64_t tp_int = 0;
    uint64_t tp_uint = 0;
    double tp_float = 0.0;
    std::string tp_string;
    std::vector<JsonValue> tp_array;
    std::map<std::string, JsonValue> tp_object;

    JsonValue() = default;
    JsonValue(std::nullptr_t) : JsonValue() {
    }
    JsonValue(bool b) : type(Type::Bool), tp_boolean(b) {
    }
    JsonValue(int i) : type(Type::Int), tp_int(i) {
    }
    JsonValue(int64_t i) : type(Type::Int), tp_int(i) {
    }
    JsonValue(uint64_t u) : type(Type::UInt), tp_uint(u) {
    }
    JsonValue(double d) : type(Type::Float), tp_float(d) {
    }
    JsonValue(const char *s) : type(Type::String), tp_string(s ? s : "") {
    }
    JsonValue(const std::string &s) : type(Type::String), tp_string(s) {
    }
    JsonValue(std::string &&s) : type(Type::String), tp_string(std::move(s)) {
    }

    static JsonValue object() {
        JsonValue v;
        v.type = Type::Object;
        return v;
    }
    static JsonValue array() {
        JsonValue v;
        v.type = Type::Array;
        return v;
    }
    static JsonValue discarded() {
        JsonValue v;
        v.type = Type::Discarded;
        return v;
    }
    static JsonValue parse(const std::string &text);

    bool is_null() const {
        return type == Type::Null;
    }
    bool is_bool() const {
        return type == Type::Bool;
    }
    bool is_int() const {
        return type == Type::Int || type == Type::UInt;
    }
    bool is_float() const {
        return type == Type::Float;
    }
    bool is_number() const {
        return is_int() || is_float();
    }
    bool is_number_integer() const {
        return is_int();
    }
    bool is_string() const {
        return type == Type::String;
    }
    bool is_array() const {
        return type == Type::Array;
    }
    bool is_object() const {
        return type == Type::Object;
    }
    bool is_discarded() const {
        return type == Type::Discarded;
    }

    bool contains(const std::string &key) const {
        return type == Type::Object && tp_object.find(key) != tp_object.end();
    }

    // Non-const access on a missing key inserts null
    JsonValue &operator[](const std::string &key) {
        if (type == Type::Null) {
            type = Type::Object;
        }
        return tp_object[key];
    }
    const JsonValue &operator[](const std::string &key) const {
        static const JsonValue null;
        if (type != Type::Object) {
            return null;
        }
        auto it = tp_object.find(key);
        return it == tp_object.end() ? null : it->second;
    }
    JsonValue &operator[](size_t index) {
        return tp_array[index];
    }
    const JsonValue &operator[](size_t index) const {
        static const JsonValue null;
        return index < tp_array.size() ? tp_array[index] : null;
    }

    void erase(const std::string &key) {
        if (type == Type::Object) {
            tp_object.erase(key);
        }
    }

    size_t size() const {
        if (type == Type::Null || type == Type::Discarded) {
            return 0;
        }
        if (type == Type::Array) {
            return tp_array.size();
        }
        if (type == Type::Object) {
            return tp_object.size();
        }
        return 1;
    }
    bool empty() const {
        if (type == Type::Null || type == Type::Discarded) {
            return true;
        }
        if (type == Type::Array) {
            return tp_array.empty();
        }
        if (type == Type::Object) {
            return tp_object.empty();
        }
        return false;
    }

    using const_iterator = std::vector<JsonValue>::const_iterator;
    const_iterator begin() const {
        return tp_array.begin();
    }
    const_iterator end() const {
        return tp_array.end();
    }

    void push_back(JsonValue v) {
        tp_array.push_back(std::move(v));
    }
    JsonValue &back() {
        return tp_array.back();
    }
    const JsonValue &back() const {
        return tp_array.back();
    }
    JsonValue &front() {
        return tp_array.front();
    }
    const JsonValue &front() const {
        return tp_array.front();
    }
    void clear() {
        type = Type::Null;
        tp_array.clear();
        tp_object.clear();
    }

    template <typename T>
    T get() const;
    template <typename T>
    T value(const std::string &key, T def) const {
        auto it = type == Type::Object ? tp_object.find(key) : tp_object.end();
        if (it == tp_object.end()) {
            return def;
        }
        return it->second.template get<T>();
    }

    static JsonValue array(std::initializer_list<JsonValue> items) {
        JsonValue v = array();
        v.tp_array.assign(items.begin(), items.end());
        return v;
    }
    static JsonValue object(std::initializer_list<std::pair<std::string, JsonValue>> items) {
        JsonValue v = object();
        for (const auto &kv : items) {
            v.tp_object[kv.first] = kv.second;
        }
        return v;
    }
    std::vector<JsonValue> &array_items() {
        return tp_array;
    }
    const std::vector<JsonValue> &array_items() const {
        return tp_array;
    }

    std::string dump() const;
    // Pretty-print with `indent` spaces per level
    std::string dump(int indent) const;
};

template <>
inline std::string JsonValue::get<std::string>() const {
    return type == Type::String ? tp_string : std::string();
}
template <>
inline int64_t JsonValue::get<int64_t>() const {
    if (type == Type::Int) {
        return tp_int;
    }
    if (type == Type::UInt) {
        return static_cast<int64_t>(tp_uint);
    }
    if (type == Type::Float) {
        return static_cast<int64_t>(tp_float);
    }
    if (type == Type::Bool) {
        return tp_boolean ? 1 : 0;
    }
    return 0;
}
template <>
inline int JsonValue::get<int>() const {
    return static_cast<int>(get<int64_t>());
}
template <>
inline bool JsonValue::get<bool>() const {
    if (type == Type::Bool) {
        return tp_boolean;
    }
    if (type == Type::Int) {
        return tp_int != 0;
    }
    if (type == Type::UInt) {
        return tp_uint != 0;
    }
    if (type == Type::Float) {
        return tp_float != 0.0;
    }
    if (type == Type::String) {
        return !tp_string.empty();
    }
    if (type == Type::Array) {
        return !tp_array.empty();
    }
    if (type == Type::Object) {
        return !tp_object.empty();
    }
    return false;
}
template <>
inline double JsonValue::get<double>() const {
    if (type == Type::Float) {
        return tp_float;
    }
    if (type == Type::Int) {
        return static_cast<double>(tp_int);
    }
    if (type == Type::UInt) {
        return static_cast<double>(tp_uint);
    }
    return 0.0;
}
template <>
inline JsonValue JsonValue::get<JsonValue>() const {
    return *this;
}

inline bool operator==(const JsonValue &val, const char *str) {
    return val.is_string() && val.tp_string == (str ? str : "");
}
inline bool operator==(const char *str, const JsonValue &val) {
    return val == str;
}
inline bool operator!=(const JsonValue &val, const char *str) {
    return !(val == str);
}
inline bool operator!=(const char *str, const JsonValue &val) {
    return !(val == str);
}
inline bool operator==(const JsonValue &val, const std::string &str) {
    return val.is_string() && val.tp_string == str;
}
inline bool operator!=(const JsonValue &val, const std::string &str) {
    return !(val == str);
}

void append_json_float(std::string &out, double d);