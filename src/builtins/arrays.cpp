// Array builtins (split out of collections.cpp).
#include "common.h"

Value ScriptRuntime::builtin_push(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_array()) {
        // push mutates the array, so we need to cast away const
        auto &arr_ptr = argvals[0].get_array();
        arr_ptr.push_back(argvals[1]);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_pop(Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc > 0) && argvals[0].is_array()) {
        auto &arr_ptr = argvals[0].get_array();
        if (!arr_ptr.empty()) {
            Value last = arr_ptr.back();
            arr_ptr.pop_back();
            return last;
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_length(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        if (argvals[0].is_array()) {
            auto &arr = argvals[0].get_array();
            return Value::make_int(arr.size());
        } else if (argvals[0].is_string()) {
            return Value::make_int(argvals[0].get_string().size());
        } else if (argvals[0].is_object()) {
            return Value::make_int(argvals[0].get_obj_ptr()->field_count());
        }
    }
    return Value::make_int(0);
}

Value ScriptRuntime::builtin_slice(Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc > 0) && argvals[0].is_array()) {
        auto &arr_ptr = argvals[0].get_array();

        int start = 0;
        if (argc > 1) {
            if (!argvals[1].is_int()) {
                return Value::make_array();
            }
            start = argvals[1].get_int();
        }
        int end = arr_ptr.size();
        if (argc > 2) {
            if (!argvals[2].is_int()) {
                return Value::make_array();
            }
            end = argvals[2].get_int();
        }

        if (start < 0) {
            start = 0;
        }
        if (end > static_cast<int>(arr_ptr.size())) {
            end = arr_ptr.size();
        }
        if (start > end) {
            start = end;
        }

        std::vector<Value> result;
        for (int i = start; i < end; ++i) {
            result.push_back(arr_ptr[i]);
        }
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_concat(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_array() && argvals[1].is_array()) {
        auto &arr1 = argvals[0].get_array();
        auto &arr2 = argvals[1].get_array();

        std::vector<Value> result(arr1.begin(), arr1.end());
        result.insert(result.end(), arr2.begin(), arr2.end());
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

// Object builtins
Value ScriptRuntime::builtin_keys(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_function()) {
        std::vector<Value> result;
        for (const auto &name : argvals[0].get_function().property_keys()) {
            result.push_back(Value::make_string(name));
        }
        return Value::make_array(std::move(result));
    }
    if (argc > 0 && argvals[0].is_object()) {
        const ObjectObj *oobj = argvals[0].get_obj_ptr();

        std::vector<Value> result;
        if (oobj) {
            for (const auto &name : oobj->get_keys()) {
                result.push_back(Value::make_string(name));
            }
        }
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_values(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_function()) {
        const FunctionData &fn = argvals[0].get_function();
        std::vector<Value> result;
        for (const auto &name : fn.property_keys()) {
            if (const Value *v = fn.get_property(name)) {
                result.push_back(*v);
            }
        }
        return Value::make_array(std::move(result));
    }
    if (argc > 0 && argvals[0].is_object()) {
        const ObjectObj *oobj = argvals[0].get_obj_ptr();

        std::vector<Value> result;
        if (oobj) {
            for (const auto &name : oobj->get_keys()) {
                if (const Value *v = oobj->get_field(name)) {
                    result.push_back(*v);
                }
            }
        }
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_has_key(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2 && argvals[0].is_array()) {
        auto *array = static_cast<ArrayObj *>(argvals[0].heap_ptr());
        return Value::make_bool(array->has_property(argvals[1].to_string()));
    }
    if (argc >= 2 && argvals[0].is_function()) {
        return Value::make_bool(argvals[0].get_function().has_property(argvals[1].to_string()));
    }
    if (argc >= 2 && argvals[0].is_object()) {
        const ObjectObj *oobj = argvals[0].get_obj_ptr();

        if (!oobj) {
            return Value::make_bool(false);
        }
        if (argvals[1].is_string()) {
            auto *key = static_cast<StringObj *>(argvals[1].heap_ptr());
            if (key->immutable) {
                if (key->field_id == UINT32_MAX) {
                    key->field_id = intern_field(key->s);
                }
                return Value::make_bool(oobj->has_field_by_id(key->field_id));
            }
        }
        return Value::make_bool(oobj->has_field(argvals[1].to_string()));
    }
    // Delegate has trap: has_key(delegate, key) -> handler.has(target, key).
    if (argc >= 2 && argvals[0].is_delegate()) {
        return Value::make_bool(delegate_has(argvals[0], argvals[1]));
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_entries(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_function()) {
        const FunctionData &fn = argvals[0].get_function();
        std::vector<Value> result;
        for (const auto &name : fn.property_keys()) {
            const Value *val = fn.get_property(name);
            if (!val) {
                continue;
            }
            std::vector<Value> pair;
            pair.push_back(Value::make_string(name));
            pair.push_back(*val);
            result.push_back(Value::make_array(std::move(pair)));
        }
        return Value::make_array(std::move(result));
    }
    if (argc > 0 && argvals[0].is_object()) {
        const ObjectObj *oobj = argvals[0].get_obj_ptr();
        std::vector<Value> result;
        if (oobj) {
            for (const auto &name : oobj->get_keys()) {
                const Value *val = oobj->get_field(name);
                if (!val) {
                    continue;
                }
                std::vector<Value> pair;
                pair.push_back(Value::make_string(name));
                pair.push_back(*val);
                result.push_back(Value::make_array(std::move(pair)));
            }
        }
        return Value::make_array(std::move(result));
    }
    return Value::make_array();
}

Value ScriptRuntime::builtin_assign(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc >= 2 && (argvals[0].is_object() || argvals[0].is_function())) {
        for (size_t i = 1; i < argc; i++) {
            if (!argvals[i].is_object() && !argvals[i].is_function()) {
                continue;
            }
            const ObjectObj *src = argvals[i].is_object() ? argvals[i].get_obj_ptr() : nullptr;
            std::vector<std::string> names;
            if (argvals[i].is_function()) {
                names = argvals[i].get_function().property_keys();
            } else if (src) {
                names = src->get_keys();
            } else {
                continue;
            }
            for (const auto &name : names) {
                const Value *val = src ? src->get_field(name) : argvals[i].get_function().get_property(name);
                if (val) {
                    if (argvals[0].is_object()) {
                        argvals[0].get_obj_ptr()->set_field(name, *val);
                    } else {
                        argvals[0].get_function().set_property(name, *val);
                    }
                }
            }
        }
        return argvals[0];
    }
    return argc > 0 ? argvals[0] : Value::none();
}

Value ScriptRuntime::builtin_freeze(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_object()) {
        ObjectObj *oobj = argvals[0].get_obj_ptr();
        oobj->frozen = true;
        return argvals[0];
    }
    return argc > 0 ? argvals[0] : Value::none();
}

Value ScriptRuntime::builtin_is_frozen(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_object()) {
        return Value::make_bool(argvals[0].get_obj_ptr()->frozen);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_yield(Value *, size_t, const nari::CallExpr *) {
    if (Runtime::g_shutdown_requested.load()) {
        if (io_pool) {
#ifndef NO_THREADS
            // close all server sockets to unblock accept() calls
            std::lock_guard<std::mutex> lock(server_sockets_mutex);
            for (int fd : server_sockets) {
                NARI_CLOSE_SOCKET(fd);
            }
            server_sockets.clear();
#endif
            io_pool->shutdown();
        }
    }

    process_completed_io();
    fire_due_intervals();

    NARI_SLEEP_MILLIS(1);
    process_completed_io();
    fire_due_intervals();

    return Value::none();
}

Value ScriptRuntime::builtin_shutdown_requested(Value *, size_t, const nari::CallExpr *) {
    return Value::make_bool(Runtime::g_shutdown_requested.load());
}

Value ScriptRuntime::builtin_sort(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::none();
    }
    auto &arr = argvals[0].get_array();
    if (argc >= 2 && argvals[1].is_function()) {
        Value cmp = argvals[1];
        // root the comparator and the array under sort.
        // the callback runs nested bytecode, and without rooting, a precise collection could cause a dangling ref
        Value arr_val = argvals[0];
        GcTempRoot temp_root(*this);
        temp_root.add(&cmp);
        temp_root.add(&arr_val);
        std::stable_sort(arr.begin(), arr.end(), [&](const Value &a, const Value &b) {
            Value r = call_function_value(cmp, { a, b });
            return r.is_int() ? r.get_int() < 0 : r.as_number() < 0.0;
        });
    } else {
        std::stable_sort(arr.begin(), arr.end(), [](const Value &a, const Value &b) {
            if (a.is_int() && b.is_int()) {
                return a.get_int() < b.get_int();
            }
            if (a.is_numeric() && b.is_numeric()) {
                return a.as_number() < b.as_number();
            }
            return a.to_string() < b.to_string();
        });
    }
    return argvals[0];
}

Value ScriptRuntime::builtin_map(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_array();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    std::vector<Value> result;
    result.reserve(arr.size());
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    temp_root.add_vec(&result);
    for (size_t i = 0; i < arr.size(); i++) {
        result.push_back(call_function_value(fn, { arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val }));
    }
    return Value::make_array(std::move(result));
}

Value ScriptRuntime::builtin_filter(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_array();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    std::vector<Value> result;
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    temp_root.add_vec(&result);
    for (size_t i = 0; i < arr.size(); i++) {
        Value keep = call_function_value(fn, { arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val });
        if (keep.as_bool()) {
            result.push_back(arr[i]);
        }
    }
    return Value::make_array(std::move(result));
}

Value ScriptRuntime::builtin_reduce(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::none();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    Value acc;
    size_t start = 0;
    if (argc >= 3) {
        acc = argvals[2];
    } else {
        if (arr.empty()) {
            return Value::none();
        }
        acc = arr[0];
        start = 1;
    }
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    temp_root.add(&acc);
    for (size_t i = start; i < arr.size(); i++) {
        acc = call_function_value(fn, { acc, arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val });
    }
    return acc;
}

Value ScriptRuntime::builtin_find(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::none();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        Value r = call_function_value(fn, { arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val });
        if (r.as_bool()) {
            return arr[i];
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_find_index(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_int(-1);
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        Value r = call_function_value(fn, { arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val });
        if (r.as_bool()) {
            return Value::make_int(static_cast<int64_t>(i));
        }
    }
    return Value::make_int(-1);
}

Value ScriptRuntime::builtin_reverse(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::none();
    }
    auto &arr = argvals[0].get_array();
    std::reverse(arr.begin(), arr.end());
    return argvals[0];
}

Value ScriptRuntime::builtin_includes(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2) {
        return Value::make_bool(false);
    }
    if (argvals[0].is_array()) {
        for (const auto &el : argvals[0].get_array()) {
            if (Value::values_equal(el, argvals[1])) {
                return Value::make_bool(true);
            }
        }
        return Value::make_bool(false);
    }
    // string includes
    std::string str = argvals[0].to_string();
    std::string search = argvals[1].to_string();
    return Value::make_bool(str.find(search) != std::string::npos);
}

Value ScriptRuntime::builtin_every(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_bool(true);
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        Value r = call_function_value(fn, { arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val });
        if (!r.as_bool()) {
            return Value::make_bool(false);
        }
    }
    return Value::make_bool(true);
}

Value ScriptRuntime::builtin_some(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_bool(false);
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        Value r = call_function_value(fn, { arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val });
        if (r.as_bool()) {
            return Value::make_bool(true);
        }
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_for_each(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::none();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    for (size_t i = 0; i < arr.size(); i++) {
        call_function_value(fn, { arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val });
    }
    return Value::none();
}

Value ScriptRuntime::builtin_splice(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::make_array();
    }
    auto &arr = argvals[0].get_array();
    int64_t sz = static_cast<int64_t>(arr.size());

    int64_t start = (argc >= 2) ? argvals[1].get_int() : 0;
    if (start < 0) {
        start = std::max<int64_t>(0, sz + start);
    }
    if (start > sz) {
        start = sz;
    }

    int64_t del_count = sz - start;
    if (argc >= 3) {
        del_count = argvals[2].get_int();
        if (del_count < 0) {
            del_count = 0;
        }
        if (del_count > sz - start) {
            del_count = sz - start;
        }
    }

    std::vector<Value> removed(arr.begin() + start, arr.begin() + start + del_count);
    arr.erase(arr.begin() + start, arr.begin() + start + del_count);
    for (size_t i = 3; i < argc; i++) {
        arr.insert(arr.begin() + start + static_cast<int64_t>(i - 3), argvals[i]);
    }
    return Value::make_array(std::move(removed));
}

// arr.fill(value, count) - resize and fill with count copies of value
// arr.fill(value)  - fill existing elements with value
Value ScriptRuntime::builtin_fill(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::make_array();
    }
    auto &arr = argvals[0].get_array();
    Value fill_val = (argc >= 2) ? argvals[1] : Value::none();
    if (argc >= 3 && argvals[2].is_int()) {
        int64_t count = argvals[2].get_int();
        if (count < 0) {
            count = 0;
        }
        if (count > 10000000) {
            count = 10000000;
        }
        arr.resize(static_cast<size_t>(count), fill_val);
        // resize only sets NEW elements; overwrite existing ones too
        for (size_t i = 0; i < arr.size(); i++) {
            arr[i] = fill_val;
        }
    } else {
        // No count: fill existing elements
        for (size_t i = 0; i < arr.size(); i++) {
            arr[i] = fill_val;
        }
    }
    return argvals[0];
}

static void flatten_into(std::vector<Value> &result, const Array &arr, int64_t depth) {
    for (const auto &v : arr) {
        if (depth > 0 && v.is_array()) {
            flatten_into(result, v.get_array(), depth - 1);
        } else {
            result.push_back(v);
        }
    }
}

Value ScriptRuntime::builtin_flat(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_array()) {
        return Value::make_array();
    }
    int64_t depth = 1;
    if (argc >= 2 && argvals[1].is_int()) {
        depth = argvals[1].get_int();
    } else if (argc >= 2 && argvals[1].is_float()) {
        depth = static_cast<int64_t>(argvals[1].get_float());
    }
    if (depth < 0) {
        depth = 0;
    }
    const auto &arr = argvals[0].get_array();
    std::vector<Value> result;
    flatten_into(result, arr, depth);
    return Value::make_array(std::move(result));
}

Value ScriptRuntime::builtin_flat_map(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_array() || !argvals[1].is_function()) {
        return Value::make_array();
    }
    const auto &arr = argvals[0].get_array();
    Value fn = argvals[1];
    Value arr_val = argvals[0];
    std::vector<Value> result;
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&arr_val);
    temp_root.add_vec(&result);
    for (size_t i = 0; i < arr.size(); i++) {
        Value mapped = call_function_value(fn, { arr[i], Value::make_int(static_cast<int64_t>(i)), arr_val });
        if (mapped.is_array()) {
            const auto &inner = mapped.get_array();
            result.insert(result.end(), inner.begin(), inner.end());
        } else {
            result.push_back(mapped);
        }
    }
    return Value::make_array(std::move(result));
}
