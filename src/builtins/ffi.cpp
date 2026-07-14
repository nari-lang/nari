#include "common.h"

#ifndef DISABLE_FFI

#include "../nari_ffi.h"

Value ScriptRuntime::builtin_ffi_membersof(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 0) {
        fprintf(stderr, "ERROR: __ffi_membersof() requires an argument (type name, string, or type identifier)\n");
        return Value::none();
    }

    std::string type_name;
    if (argvals[0].is_string()) {
        type_name = argvals[0].get_string();
    } else if (argvals[0].is_object()) {
        const ObjectObj *oobj2 = argvals[0].get_obj_ptr();
        const Value *type_v = oobj2->get_field("__type");
        if (type_v && type_v->is_string()) {
            type_name = type_v->get_string();
        } else {
            fprintf(stderr, "ERROR: __ffi_membersof() object does not have a __type property\n");
            return Value::none();
        }
    } else {
        fprintf(stderr, "ERROR: __ffi_membersof() requires a string or object argument (type name)\n");
        return Value::none();
    }

    const nari::TypeDecl *type_decl = Parser::get_registered_type(type_name);

    if (!type_decl) {
        fprintf(stderr, "ERROR: Type '%s' not found in registry\n", type_name.c_str());
        return Value::none();
    }

    std::string resolved_type_name = type_name;
    const nari::TypeDecl *resolved_decl = type_decl;
    std::vector<std::string> aliases;
    while (resolved_decl && resolved_decl->is_alias()) {
        if (std::find(aliases.begin(), aliases.end(), resolved_decl->name) != aliases.end()) {
            fprintf(stderr, "ERROR: Cyclic FFI type alias involving '%s'\n", resolved_decl->name.c_str());
            return Value::none();
        }
        aliases.push_back(resolved_decl->name);
        if (!resolved_decl->alias_target) {
            fprintf(stderr, "ERROR: Type alias '%s' has no target\n", resolved_type_name.c_str());
            return Value::none();
        }

        const nari::TypeDecl *next_decl = Parser::get_registered_type(resolved_decl->alias_target->name);
        // return primitives directly
        if (!next_decl) {
            Value result_val2 = Value::make_object();
            ObjectObj *res_oobj2 = result_val2.get_obj_ptr();
            res_oobj2->set_field("type", Value::make_string(resolved_decl->alias_target->name));
            return result_val2;
        }

        resolved_type_name = resolved_decl->alias_target->name;
        resolved_decl = next_decl;
    }

    if (!resolved_decl || resolved_decl->is_alias()) {
        fprintf(stderr, "ERROR: Could not resolve type '%s'\n", type_name.c_str());
        return Value::none();
    }

    // nari type -> ffi type
    auto map_type_to_ffi = [](const std::string &nari_type) -> std::string {
        if (nari_type == "f32" || nari_type == "float") {
            return "float";
        }
        if (nari_type == "f64" || nari_type == "double") {
            return "double";
        }
        if (nari_type == "i8") {
            return "i8";
        }
        if (nari_type == "u8") {
            return "u8";
        }
        if (nari_type == "i16") {
            return "i16";
        }
        if (nari_type == "u16") {
            return "u16";
        }
        if (nari_type == "i32" || nari_type == "int") {
            return "int";
        }
        if (nari_type == "i64" || nari_type == "long") {
            return "long";
        }
        if (nari_type == "u32" || nari_type == "uint") {
            return "uint";
        }
        if (nari_type == "u64" || nari_type == "ulong") {
            return "ulong";
        }
        if (nari_type == "bool" || nari_type == "boolean") {
            return "bool";
        }
        if (nari_type == "string" || nari_type == "pointer") {
            return "pointer";
        }

        return nari_type;
    };

    std::vector<Value> fields_array;

    for (const auto &field : resolved_decl->fields) {
        Value field_val = Value::make_object();
        ObjectObj *fobj = field_val.get_obj_ptr();
        fobj->set_field("name", Value::make_string(field.name));
        fobj->set_field("type", Value::make_string(map_type_to_ffi(field.type->name)));
        if (field.type->fixed_array_count > 0) {
            fobj->set_field("count", Value::make_int(field.type->fixed_array_count));
        }
        fields_array.push_back(field_val);
    }

    Value result_val = Value::make_object();
    ObjectObj *res_oobj = result_val.get_obj_ptr();
    res_oobj->set_field(
        resolved_decl->kind == nari::TypeDeclKind::Union ? "union" : "struct",
        Value::make_string(resolved_type_name));
    res_oobj->set_field("fields", Value::make_array(std::move(fields_array)));

    return result_val;
}

Value ScriptRuntime::builtin_ffi_load_library(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc == 0) || !argvals[0].is_string()) {
        return Value::none();
    }

    std::string lib_path = argvals[0].get_string();

    auto &registry = FFIRegistry::instance();
    auto lib = registry.load_library(lib_path);

    if (!lib || !lib->is_loaded()) {
        // return error object, later on this will be standardized into an Error
        // type to be used with either try/catch or Result<T, E>.
        Value err_val = Value::make_object();
        ObjectObj *err_oobj = err_val.get_obj_ptr();
        err_oobj->set_field("error", Value::make_string("Failed to load library: " + lib_path));
        if (lib) {
            err_oobj->set_field("message", Value::make_string(lib->get_error()));
        }
        err_oobj->set_field("loaded", Value::make_bool(false));
        return err_val;
    }

    Value lib_val = Value::make_object();
    ObjectObj *lib_oobj = lib_val.get_obj_ptr();
    lib_oobj->set_field("loaded", Value::make_bool(true));
    lib_oobj->set_field("path", Value::make_string(lib_path));
    lib_oobj->set_field("__ffi_handle__", Value::make_int(reinterpret_cast<int64_t>(lib.get())));

    const auto &symbols = lib->get_symbols();
    std::vector<Value> symbols_array;
    symbols_array.reserve(symbols.size());
    for (const auto &symbol : symbols) {
        symbols_array.push_back(Value::make_string(symbol));
    }
    lib_oobj->set_field("__symbols__", Value::make_array(std::move(symbols_array)));

    return lib_val;
}

// __ffi_get_symbol(lib, "function_name")
Value ScriptRuntime::builtin_ffi_get_symbol(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2 || !argvals[0].is_object() || !argvals[1].is_string()) {
        return Value::none();
    }

    const ObjectObj *lib_oobj_gs = argvals[0].get_obj_ptr();
    std::string symbol_name = argvals[1].get_string();

    if (!lib_oobj_gs->has_field("__ffi_handle__")) {
        return Value::none();
    }

    uintptr_t handle_int = lib_oobj_gs->get_field("__ffi_handle__")->get_ptr_bits();
    auto *lib = reinterpret_cast<FFILibrary *>(handle_int);

    void *symbol = lib->get_symbol(symbol_name);
    if (!symbol) {
        return Value::none();
    }

    return Value::make_int(reinterpret_cast<int64_t>(symbol));
}

// infer argument types from a printf-style format string, used for variadic functions
// this is purely heuristic and may not be accurate for all format strings, but *should* work for common cases
// if you find an issue, please open an issue!!
static std::vector<FFIType> infer_types_from_format_string(const std::string &fmt) {
    std::vector<FFIType> types;
    size_t len = fmt.size();
    for (size_t i = 0; i < len; ++i) {
        if (fmt[i] != '%') {
            continue;
        }
        ++i;
        if (i >= len) {
            break;
        }
        if (fmt[i] == '%') { // %%
            continue;
        }

        // Optional flags: - + space 0 # '
        while (i < len && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '0' || fmt[i] == '#' || fmt[i] == '\'')) {
            ++i;
        }

        // Optional width
        if (i < len && fmt[i] == '*') {
            types.push_back(FFIType::Int32); // * consumes an int
            ++i;
        } else {
            while (i < len && std::isdigit((unsigned char)fmt[i])) {
                ++i;
            }
        }

        // optional precision
        if (i < len && fmt[i] == '.') {
            ++i;
            if (i < len && fmt[i] == '*') {
                types.push_back(FFIType::Int32); // .* consumes an int
                ++i;
            } else {
                while (i < len && std::isdigit((unsigned char)fmt[i])) {
                    ++i;
                }
            }
        }

        // length modifier
        bool is_long_long = false;
        bool is_long = false;
        if (i < len && fmt[i] == 'h') {
            ++i;
            if (i < len && fmt[i] == 'h') {
                ++i; // hh
            }
        } else if (i < len && fmt[i] == 'l') {
            is_long = true;
            ++i;
            if (i < len && fmt[i] == 'l') {
                is_long_long = true;
                ++i;
            }
        } else if (i < len && (fmt[i] == 'L' || fmt[i] == 'q')) {
            is_long_long = true;
            ++i;
        } else if (i < len && (fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't')) {
            is_long = true;
            ++i;
        }

        if (i >= len) {
            break;
        }

        switch (fmt[i]) {
            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X':
            case 'c':
                types.push_back((is_long || is_long_long) ? FFIType::Int64 : FFIType::Int32);
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
                // C always promotes float to double in variadic calls
                types.push_back(FFIType::Double);
                break;
            case 's':
            case 'p':
            case 'n':
                types.push_back(FFIType::Pointer);
                break;
            default:
                break;
        }
    }
    return types;
}

// __ffi_call(loaded_lib_reference, "function_name", signature_obj, [args...])
// signature must look something like { return: "int", params: ["int", "string"] }
Value ScriptRuntime::builtin_ffi_call(const Value *argvals, size_t argc, const nari::CallExpr *) {

    if (argc < 3 || !argvals[0].is_object() || !argvals[1].is_string() || !argvals[2].is_object()) {
        return Value::none();
    }

    if (!argvals[0].is_object()) {
        fprintf(stderr, "ERROR: First argument to __ffi_call is not an object (type index: %zu)\n", (size_t)argvals[0].tag());
        return Value::none();
    }

    const ObjectObj *lib_oobj2 = argvals[0].get_obj_ptr();
    std::string func_name = argvals[1].get_string();
    const ObjectObj *sig_oobj = argvals[2].get_obj_ptr();

    if (!lib_oobj2->has_field("__ffi_handle__")) {
        fprintf(stderr, "ERROR: __ffi_handle__ not found in library object\n");
        const Value *err_v = lib_oobj2->get_field("error");
        if (err_v) {
            fprintf(stderr, "Library load error: %s\n", err_v->get_string().c_str());
        }
        const Value *msg_v = lib_oobj2->get_field("message");
        if (msg_v) {
            fprintf(stderr, "Detailed message: %s\n", msg_v->get_string().c_str());
        }
        return Value::none();
    }

    const Value *handle_v = lib_oobj2->get_field("__ffi_handle__");
    if (!handle_v || !handle_v->is_int()) {
        fprintf(stderr, "ERROR: __ffi_handle__ is not an integer (type index: %zu)\n", (size_t)handle_v->tag());
        return Value::none();
    }

    uintptr_t handle_int = handle_v->get_ptr_bits();
    auto *lib = reinterpret_cast<FFILibrary *>(handle_int);

    void *func_ptr = lib->get_symbol(func_name);
    if (!func_ptr) {
        return Value::none();
    }

    FFISignature sig;

    auto parse_ffi_type = [&sig](const Value &type_val, bool is_return) -> FFIType {
        if (type_val.is_string()) {
            std::string type_str = type_val.get_string();

            // Check for pointer syntax (e.g., "MSG*", "u8*", "Rectangle*")
            if (!type_str.empty() && type_str.back() == '*') {
                return FFIType::Pointer;
            }

            if (type_str == "void") {
                return FFIType::Void;
            } else if (type_str == "i8" || type_str == "int8" || type_str == "char") {
                return FFIType::Int8;
            } else if (type_str == "u8" || type_str == "uint8" || type_str == "uchar" || type_str == "byte") {
                return FFIType::UInt8;
            } else if (type_str == "i16" || type_str == "int16" || type_str == "short") {
                return FFIType::Int16;
            } else if (type_str == "u16" || type_str == "uint16" || type_str == "ushort" || type_str == "word") {
                return FFIType::UInt16;
            } else if (type_str == "int" || type_str == "i32" || type_str == "int32") {
                return FFIType::Int32;
            } else if (type_str == "long" || type_str == "i64" || type_str == "int64") {
                return FFIType::Int64;
            } else if (type_str == "uint" || type_str == "u32") {
                return FFIType::UInt32;
            } else if (type_str == "ulong" || type_str == "u64") {
                return FFIType::UInt64;
            } else if (type_str == "float") {
                return FFIType::Float;
            } else if (type_str == "double") {
                return FFIType::Double;
            } else if (type_str == "bool") {
                return FFIType::Bool;
            } else if (type_str == "string" || type_str == "pointer") {
                return FFIType::Pointer;
            }
        } else if (type_val.is_object()) {
            const ObjectObj *type_oobj = type_val.get_obj_ptr();

            const bool is_union = type_oobj->has_field("union");
            if ((type_oobj->has_field("struct") || is_union) && type_oobj->has_field("fields")) {
                std::string struct_name = type_oobj->get_field(is_union ? "union" : "struct")->get_string();
                const Value *fields_v = type_oobj->get_field("fields");

                std::vector<FFIStructField> fields;

                if (fields_v && fields_v->is_array()) {
                    const auto &fields_array = fields_v->get_array();
                    for (const auto &field_val : fields_array) {
                        if (field_val.is_object()) {
                            const ObjectObj *field_oobj = field_val.get_obj_ptr();
                            if (field_oobj->has_field("name") && field_oobj->has_field("type")) {

                                std::string field_name = field_oobj->get_field("name")->get_string();
                                std::string field_type_str = field_oobj->get_field("type")->get_string();
                                FFIType field_type = FFIType::Void;

                                if (field_type_str == "i8" || field_type_str == "int8" || field_type_str == "char") {
                                    field_type = FFIType::Int8;
                                } else if (
                                    field_type_str == "u8" || field_type_str == "uint8" ||
                                    field_type_str == "uchar" || field_type_str == "byte") {
                                    field_type = FFIType::UInt8;
                                } else if (field_type_str == "i16" || field_type_str == "int16" ||
                                           field_type_str == "short") {
                                    field_type = FFIType::Int16;
                                } else if (
                                    field_type_str == "u16" || field_type_str == "uint16" ||
                                    field_type_str == "ushort" || field_type_str == "word") {
                                    field_type = FFIType::UInt16;
                                } else if (field_type_str == "int" || field_type_str == "i32" || field_type_str == "int32") {
                                    field_type = FFIType::Int32;
                                } else if (field_type_str == "long" || field_type_str == "i64" || field_type_str == "int64") {
                                    field_type = FFIType::Int64;
                                } else if (field_type_str == "uint" || field_type_str == "u32") {
                                    field_type = FFIType::UInt32;
                                } else if (field_type_str == "ulong" || field_type_str == "u64") {
                                    field_type = FFIType::UInt64;
                                } else if (field_type_str == "float") {
                                    field_type = FFIType::Float;
                                } else if (field_type_str == "double") {
                                    field_type = FFIType::Double;
                                } else if (field_type_str == "bool") {
                                    field_type = FFIType::Bool;
                                } else if (field_type_str == "string" || field_type_str == "pointer") {
                                    field_type = FFIType::Pointer;
                                }

                                size_t field_count = 1;
                                if (field_oobj->has_field("count")) {
                                    const Value *count_v = field_oobj->get_field("count");
                                    if (!count_v || !count_v->is_int() || count_v->get_int() <= 0) {
                                        fprintf(stderr, "ERROR: FFI struct field '%s' has an invalid fixed count\n", field_name.c_str());
                                        continue;
                                    }
                                    field_count = static_cast<size_t>(count_v->get_int());
                                }
                                fields.emplace_back(field_name, field_type, field_count);
                                if (field_type == FFIType::Void) {
                                    fields.back().aggregate_def = create_struct_def_from_type(field_type_str);
                                    if (!fields.back().aggregate_def) {
                                        fprintf(stderr, "ERROR: Unsupported FFI aggregate field type '%s'\n", field_type_str.c_str());
                                        fields.pop_back();
                                    } else {
                                        fields.back().type = FFIType::Struct;
                                    }
                                }
                            }
                        }
                    }
                }

                auto struct_def = std::make_shared<FFIStructDef>(struct_name, fields, is_union);

                if (is_return) {
                    sig.return_struct_def = struct_def;
                } else {
                    sig.param_struct_defs.push_back(struct_def);
                }

                return FFIType::Struct;
            }
        }

        return FFIType::Void;
    };

    if (sig_oobj->has_field("returns")) {
        sig.return_type = parse_ffi_type(*sig_oobj->get_field("returns"), true);
    } else {
        fprintf(stderr,
                "ERROR: FFI signature object is missing 'returns' field!\n");
        return Value::none();
    }

    if (sig_oobj->has_field("variadic")) {
        const Value *variadic_v = sig_oobj->get_field("variadic");
        if (variadic_v->is_int()) {
            if (variadic_v->get_int() < 0) {
                fprintf(stderr, "ERROR: FFI variadic fixed parameter count cannot be negative\n");
                return Value::none();
            }
            sig.is_variadic = true;
            sig.fixed_param_count = static_cast<size_t>(variadic_v->get_int());
        } else if (variadic_v->is_bool() && variadic_v->get_bool()) {
            sig.is_variadic = true;
            sig.fixed_param_count = 0;
        }
    }

    if (sig_oobj->has_field("params") &&
        sig_oobj->get_field("params")->is_array()) {
        const auto &params_array = sig_oobj->get_field("params")->get_array();
        for (const auto &param_val : params_array) {
            FFIType param_type = parse_ffi_type(param_val, false);
            sig.param_types.push_back(param_type);

            // if not struct, add nullptr to keep alignment
            if (param_type != FFIType::Struct) {
                sig.param_struct_defs.push_back(nullptr);
            }
        }

        // if variadic was set to true without a count, use param array size as fixed count
        if (sig.is_variadic && sig.fixed_param_count == 0) {
            sig.fixed_param_count = params_array.size();
        }
    }

    if (sig.is_variadic && sig.fixed_param_count > sig.param_types.size()) {
        fprintf(stderr, "ERROR: FFI variadic fixed parameter count exceeds declared parameters\n");
        return Value::none();
    }

    // function arguments (everything after the first 3 args)
    std::vector<Value> func_args;
    if (argc > 3 && argvals[3].is_array()) {
        const auto &args_array = argvals[3].get_array();
        func_args.assign(args_array.begin(), args_array.end());
    } else {
        // otherwise, take all args after the 3rd as individual args
        for (size_t i = 3; i < argc; i++) {
            func_args.push_back(argvals[i]);
        }
    }
    if (sig.is_variadic && func_args.size() < sig.fixed_param_count) {
        fprintf(stderr, "ERROR: Variadic FFI call has fewer arguments than fixed parameters\n");
        return Value::none();
    }

    if (sig.is_variadic && func_args.size() > sig.fixed_param_count) {
        // try to infer variadic types from a printf-style format string found in
        // the fixed arguments, this scans fixed params in reverse.
        std::vector<FFIType> fmt_types;
        for (size_t i = sig.fixed_param_count; i-- > 0;) {
            if (func_args[i].is_string()) {
                const std::string &s = func_args[i].get_string();
                if (s.find('%') != std::string::npos) {
                    fmt_types = infer_types_from_format_string(s);
                    break;
                }
            }
        }

        for (size_t i = sig.fixed_param_count; i < func_args.size(); i++) {
            size_t varg_idx = i - sig.fixed_param_count;
            // Prefer the type the format string says the C function expects
            if (varg_idx < fmt_types.size()) {
                sig.param_types.push_back(fmt_types[varg_idx]);
            } else {
                // Fall back to inferring from the runtime value type
                const Value &arg = func_args[i];
                if (arg.is_string()) {
                    sig.param_types.push_back(FFIType::Pointer);
                } else if (arg.is_int()) {
                    sig.param_types.push_back(FFIType::Int32);
                } else if (arg.is_float()) {
                    sig.param_types.push_back(FFIType::Double);
                } else if (arg.is_bool()) {
                    sig.param_types.push_back(FFIType::Bool);
                } else {
                    fprintf(stderr, "ERROR: Cannot infer FFI type for variadic object or array argument\n");
                    return Value::none();
                }
            }
        }
    }

    if (sig.is_variadic) {
        return FFICaller::call_function_variadic(func_ptr, sig, func_args);
    } else {
        return FFICaller::call_function(func_ptr, sig, func_args);
    }
}

Value ScriptRuntime::builtin_ffi_utf16(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc == 0) || !argvals[0].is_string()) {
        return Value::make_int(0);
    }

    const std::string &utf8 = argvals[0].get_string();
    std::u16string utf16;
    if (!utf8_to_utf16(utf8, utf16)) {
        fprintf(stderr, "ERROR: __ffi_utf16() received invalid UTF-8 input\n");
        return Value::make_int(0);
    }

    if (utf16.size() > (SIZE_MAX / sizeof(char16_t)) - 1) {
        fprintf(stderr, "ERROR: __ffi_utf16() string too large\n");
        return Value::make_int(0);
    }
    size_t units = utf16.size() + 1; // include null terminator
    auto *raw = static_cast<char16_t *>(std::malloc(units * sizeof(char16_t)));
    if (!raw) {
        fprintf(stderr, "ERROR: __ffi_utf16() failed to allocate memory\n");
        return Value::make_int(0);
    }

    if (!utf16.empty()) {
        std::memcpy(raw, utf16.data(), utf16.size() * sizeof(char16_t));
    }
    raw[utf16.size()] = u'\0';

    return Value::make_int(reinterpret_cast<int64_t>(raw));
}

Value ScriptRuntime::builtin_ffi_alloc(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc == 0) || !argvals[0].is_int()) {
        return Value::make_int(0);
    }

    int64_t size = argvals[0].get_int();
    if (size <= 0) {
        return Value::make_int(0);
    }

    void *raw = std::calloc(1, static_cast<size_t>(size));
    if (!raw) {
        return Value::make_int(0);
    }

    return Value::make_int(reinterpret_cast<int64_t>(raw));
}

Value ScriptRuntime::builtin_ffi_utf16_read(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc == 0) || !argvals[0].is_int()) {
        return Value::make_string("");
    }

    auto *ptr = reinterpret_cast<const char16_t *>(argvals[0].get_ptr_bits());
    if (!ptr) {
        return Value::make_string("");
    }

    int64_t max_units = -1;
    if (argc >= 2 && argvals[1].is_int()) {
        max_units = argvals[1].get_int();
    }

    size_t units = 0;
    if (max_units > 0) {
        size_t limit = static_cast<size_t>(max_units);
        while (units < limit && ptr[units] != u'\0') {
            ++units;
        }
    } else {
        while (ptr[units] != u'\0') {
            ++units;
        }
    }

    std::u16string utf16(ptr, units);
    std::string utf8;
    if (!utf16_to_utf8(utf16, utf8)) {
        fprintf(stderr, "ERROR: __ffi_utf16_read() failed to convert UTF-16 to UTF-8\n");
        return Value::make_string("");
    }

    return Value::make_string(utf8);
}

Value ScriptRuntime::builtin_ffi_free(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0 && argvals[0].is_object() && argvals[0].get_obj_ptr()->is_managed_native_struct()) {
        fprintf(stderr, "ERROR: __ffi_free() cannot free a managed FFI struct\n");
        return Value::none();
    }
    if ((argc == 0) || !argvals[0].is_int()) {
        return Value::none();
    }

    uintptr_t ptr_value = argvals[0].get_ptr_bits();
    if (ptr_value != 0) {
        std::free((void *)ptr_value);
    }

    return Value::none();
}

// __ffi_alloc_struct(typename) - allocate memory for a struct type
Value ScriptRuntime::builtin_ffi_alloc_struct(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc == 0) || !argvals[0].is_string()) {
        fprintf(stderr, "ERROR: __ffi_alloc_struct() requires a type name string\n");
        return Value::none();
    }

    std::string type_name = argvals[0].get_string();
    size_t struct_size = nari::get_struct_size(type_name);

    if (struct_size == 0) {
        fprintf(stderr, "ERROR: __ffi_alloc_struct() failed to get size for type '%s'\n", type_name.c_str());
        return Value::none();
    }

    void *ptr = std::calloc(1, struct_size);
    if (!ptr) {
        fprintf(stderr, "ERROR: __ffi_alloc_struct() failed to allocate %zu bytes\n", struct_size);
        return Value::none();
    }

    return Value::make_int((int64_t)ptr);
}

// __ffi_managed_struct(typename) - create an ordinary GC object owning zeroed struct memory
Value ScriptRuntime::builtin_ffi_managed_struct(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 0 || !argvals[0].is_string()) {
        fprintf(stderr, "ERROR: __ffi_managed_struct() requires a type name string\n");
        return Value::none();
    }

    const std::string &type_name = argvals[0].get_string();
    size_t struct_size = nari::get_struct_size(type_name);
    if (struct_size == 0) {
        fprintf(stderr, "ERROR: __ffi_managed_struct() failed to get size for type '%s'\n", type_name.c_str());
        return Value::none();
    }

    Value owner = Value::make_object();
    ObjectObj *obj = owner.get_obj_ptr();
    obj->native_struct_type = type_name;
    obj->native_struct_storage.assign(struct_size, 0);
    return owner;
}

// __ffi_read_struct(ptr, typename) - read struct from memory into Nari object
Value ScriptRuntime::builtin_ffi_read_struct(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 1 && argvals[0].is_object()) {
        const ObjectObj *owner = argvals[0].get_obj_ptr();
        if (!owner->is_managed_native_struct()) {
            fprintf(stderr, "ERROR: __ffi_read_struct() object is not a managed FFI struct\n");
            return Value::none();
        }
        size_t expected_size = nari::get_struct_size(owner->native_struct_type);
        if (expected_size == 0 || expected_size != owner->native_struct_storage.size()) {
            fprintf(stderr, "ERROR: __ffi_read_struct() managed struct type or size is invalid\n");
            return Value::none();
        }
        return nari::read_struct_from_memory(nari::managed_struct_pointer(argvals[0]), owner->native_struct_type);
    }
    if (argc < 2 || !argvals[0].is_int() || !argvals[1].is_string()) {
        fprintf(stderr, "ERROR: __ffi_read_struct() requires (owner) or (pointer_int, type_name_string)\n");
        return Value::none();
    }

    uintptr_t ptr_value = argvals[0].get_ptr_bits();
    std::string type_name = argvals[1].get_string();

    if (ptr_value == 0) {
        fprintf(stderr, "ERROR: __ffi_read_struct() received null pointer!\n");
        return Value::none();
    }

    void *ptr = reinterpret_cast<void *>(ptr_value);
    return nari::read_struct_from_memory(ptr, type_name);
}

// __ffi_write_struct(ptr, typename, obj) - write nari object to struct memory
Value ScriptRuntime::builtin_ffi_write_struct(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 2 && argvals[0].is_object() && argvals[1].is_object()) {
        const ObjectObj *owner = argvals[0].get_obj_ptr();
        if (!owner->is_managed_native_struct()) {
            fprintf(stderr, "ERROR: __ffi_write_struct() first object is not a managed FFI struct\n");
            return Value::none();
        }
        size_t expected_size = nari::get_struct_size(owner->native_struct_type);
        if (expected_size == 0 || expected_size != owner->native_struct_storage.size()) {
            fprintf(stderr, "ERROR: __ffi_write_struct() managed struct type or size is invalid\n");
            return Value::none();
        }
        return Value::make_bool(nari::write_struct_to_memory(
            nari::managed_struct_pointer(argvals[0]), owner->native_struct_type, argvals[1]));
    }
    if (argc < 3 || !argvals[0].is_int() || !argvals[1].is_string() || !argvals[2].is_object()) {
        fprintf(stderr, "ERROR: __ffi_write_struct() requires (owner, object) or (pointer_int, type_name_string, object)\n");
        return Value::none();
    }

    uintptr_t ptr_value = argvals[0].get_ptr_bits();
    std::string type_name = argvals[1].get_string();
    const Value &obj = argvals[2];

    if (ptr_value == 0) {
        fprintf(stderr, "ERROR: __ffi_write_struct() received null pointer\n");
        return Value::none();
    }

    void *ptr = reinterpret_cast<void *>(ptr_value);
    return Value::make_bool(nari::write_struct_to_memory(ptr, type_name, obj));
}

// __ffi_sizeof(typename) - get size of a struct type
Value ScriptRuntime::builtin_ffi_sizeof(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc == 0) || !argvals[0].is_string()) {
        fprintf(stderr, "ERROR: __ffi_sizeof() requires a type name string\n");
        return Value::none();
    }

    std::string type_name = argvals[0].get_string();
    size_t struct_size = nari::get_struct_size(type_name);

    if (struct_size == 0) {
        fprintf(stderr, "ERROR: __ffi_sizeof() failed to get size for type '%s'\n", type_name.c_str());
        return Value::none();
    }

    return Value::make_int(static_cast<int64_t>(struct_size));
}

Value ScriptRuntime::builtin_ffi_create_callback(const Value *argvals, size_t argc, const CallExpr *) {
    if (argc < 2 || !argvals[0].is_object() || !argvals[1].is_function()) {
        fprintf(stderr, "ERROR: __ffi_create_callback() requires (signature_object, nari_function)\n");
        return Value::none();
    }

    const ObjectObj &sig_oobj_cb = *argvals[0].get_obj_ptr();
    const Value &nari_func = argvals[1];

    // Parse signature (same as in __ffi_call)
    FFISignature sig;
    bool invalid_callback_type = false;

    auto parse_ffi_type = [&invalid_callback_type](const Value &type_val, bool is_return) -> FFIType {
        if (type_val.is_string()) {
            std::string type_str = type_val.get_string();

            if (!type_str.empty() && type_str.back() == '*') {
                return FFIType::Pointer;
            }

            if (type_str == "void") {
                return FFIType::Void;
            } else if (type_str == "i8" || type_str == "int8" || type_str == "char") {
                return FFIType::Int8;
            } else if (type_str == "u8" || type_str == "uint8" || type_str == "uchar" || type_str == "byte") {
                return FFIType::UInt8;
            } else if (type_str == "i16" || type_str == "int16" || type_str == "short") {
                return FFIType::Int16;
            } else if (type_str == "u16" || type_str == "uint16" || type_str == "ushort" || type_str == "word") {
                return FFIType::UInt16;
            } else if (type_str == "int" || type_str == "i32" || type_str == "int32") {
                return FFIType::Int32;
            } else if (type_str == "long" || type_str == "i64" || type_str == "int64") {
                return FFIType::Int64;
            } else if (type_str == "uint" || type_str == "u32") {
                return FFIType::UInt32;
            } else if (type_str == "ulong" || type_str == "u64") {
                return FFIType::UInt64;
            } else if (type_str == "float") {
                return FFIType::Float;
            } else if (type_str == "double") {
                return FFIType::Double;
            } else if (type_str == "bool") {
                return FFIType::Bool;
            } else if (type_str == "string" || type_str == "pointer") {
                return FFIType::Pointer;
            }
        }

        if (type_val.is_object()) {
            const ObjectObj *descriptor = type_val.get_obj_ptr();
            if (descriptor->has_field("union")) {
                fprintf(stderr, "ERROR: Passing unions by value in callbacks is not supported, pass a union pointer instead!\n");
            } else {
                fprintf(stderr, "ERROR: Aggregate types are not supported in callbacks\n");
            }
            invalid_callback_type = true;
            return FFIType::Void;
        }

        fprintf(stderr, "ERROR: Unsupported FFI type for callback\n");
        invalid_callback_type = true;
        return FFIType::Void;
    };

    if (sig_oobj_cb.has_field("returns")) {
        sig.return_type = parse_ffi_type(*sig_oobj_cb.get_field("returns"), true);
    } else {
        sig.return_type = FFIType::Void;
    }

    if (sig_oobj_cb.has_field("params")) {
        const Value *params_v = sig_oobj_cb.get_field("params");
        if (params_v->is_array()) {
            const auto &params_arr = params_v->get_array();
            for (const auto &param_val : params_arr) {
                sig.param_types.push_back(parse_ffi_type(param_val, false));
            }
        }
    }

    if (invalid_callback_type) {
        return Value::none();
    }

    void *callback_ptr = FFICallbackManager::instance().create_callback(sig, nari_func, this);

    if (!callback_ptr) {
        fprintf(stderr, "ERROR: Failed to create FFI callback\n");
        return Value::none();
    }

    return Value::make_int(reinterpret_cast<int64_t>(callback_ptr));
}

// __ffi_free_callback(callback_pointer) - free a previously created callback
Value ScriptRuntime::builtin_ffi_free_callback(const Value *argvals, size_t argc, const CallExpr *) {
    if ((argc == 0) || !argvals[0].is_int()) {
        fprintf(stderr, "ERROR: __ffi_free_callback() requires a callback pointer (integer)\n");
        return Value::none();
    }

    uintptr_t ptr_value = argvals[0].get_ptr_bits();
    if (ptr_value != 0) {
        void *callback_ptr = reinterpret_cast<void *>(ptr_value);
        FFICallbackManager::instance().free_callback(callback_ptr);
    }

    return Value::none();
}
#endif // DISABLE_FFI
