// Byte array builtins, shares array/string methods
#include "common.h"

namespace {

// A valid byte is an int in 0..255; floats and other types are rejected.
bool as_byte(const Value &v, uint8_t &out) {
    if (!v.is_int()) {
        return false;
    }
    const int64_t n = v.get_int();
    if (n < 0 || n > 255) {
        return false;
    }
    out = static_cast<uint8_t>(n);
    return true;
}

} // namespace

// bytes() -> empty
// bytes(count) -> count zero bytes
// bytes(string) -> the string's raw bytes
// bytes(array) -> bytes from ints in 0..255
// bytes(bytes) -> an independent copy
Value ScriptRuntime::builtin_bytes(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc == 0) {
        return Value::make_bytes();
    }
    if (argc > 1) {
        return runtime_fatal("TypeError: bytes() takes at most one argument", call);
    }
    const Value &v = argvals[0];
    if (v.is_bytes()) {
        const auto &data = v.get_bytes();
        return Value::make_bytes(data.data(), data.size());
    }
    if (v.is_int()) {
        const int64_t count = v.get_int();
        if (count < 0) {
            return runtime_fatal("TypeError: bytes(count) requires a non-negative count", call);
        }
        return Value::make_bytes(std::vector<uint8_t>(static_cast<size_t>(count), 0));
    }
    if (v.is_string()) {
        const std::string &s = v.get_string();
        return Value::make_bytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }
    if (v.is_array()) {
        const auto &arr = v.get_array();
        std::vector<uint8_t> out;
        out.reserve(arr.size());
        for (const Value &el : arr) {
            uint8_t byte = 0;
            if (!as_byte(el, byte)) {
                return runtime_fatal("TypeError: bytes(array) elements must be integers in 0..255", call);
            }
            out.push_back(byte);
        }
        return Value::make_bytes(std::move(out));
    }
    return runtime_fatal("TypeError: bytes() expects an int count, string, array, or bytes, got " + value_type_name(v), call);
}

Value ScriptRuntime::builtin_isBytes(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_bool(argvals[0].is_bytes());
    }
    return Value::make_bool(false);
}

// bytes.to_array() -> a plain array of ints (copy)
Value ScriptRuntime::builtin_bytes_toArray(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_bytes()) {
        const auto &data = argvals[0].get_bytes();
        std::vector<Value> out;
        out.reserve(data.size());
        for (uint8_t byte : data) {
            out.push_back(Value::make_int(static_cast<int64_t>(byte)));
        }
        return Value::make_array(std::move(out));
    }
    return Value::none();
}
