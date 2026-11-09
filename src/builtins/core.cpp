#include "common.h"

#include <climits>
#include <cmath>

namespace chrono = std::chrono;

// eval(source): parse and execute Nari source at runtime, returning the value
// of the expression (or of an explicit return) in the source.
Value ScriptRuntime::builtin_eval(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc != 1 || !argvals[0].is_string()) {
        return runtime_fatal("eval expects a single string argument", call);
    }
    const std::string src = argvals[0].get_string();

    static size_t eval_counter = 0;
    const std::string wrapper_name = "__eval_fn_" + std::to_string(eval_counter++);
    const std::string eval_toplevel = "__top_level__@<eval>";
    const std::string saved_filename = (call && !call->filename.empty()) ? call->filename : "<eval>";

    auto parse_src = [&](bool wrap, std::vector<Parser::ParseError> &errors) -> FuncList {
        Parser::set_source_filename("<eval>");

        Parser::ParseResult result =
            Parser::parse_program_recovering(wrap ? "func " + wrapper_name + "() {\nreturn (" + src + ");\n}\n" : src, false);

        Parser::set_source_filename(saved_filename);
        if (!result.ok()) {
            errors = std::move(result.errors);
            return {};
        }
        return std::move(result.functions);
    };

    std::vector<Parser::ParseError> errors;
    FuncList funcs = parse_src(/*wrap=*/true, errors);
    if (funcs.empty()) {
        errors.clear();
        funcs = parse_src(/*wrap=*/false, errors);
    }
    if (funcs.empty()) {
        std::string msg = "eval: failed to parse source";
        if (!errors.empty()) {
            msg += ": " + errors.front().message;
            if (errors.front().line > 0) {
                msg += " (line " + std::to_string(errors.front().line) + ")";
            }
        }
        return runtime_fatal(msg, call);
    }

    // Normalize the entry name: expression-form source parses into `wrapper_name`,
    // statement-form arrives as the <eval> top-level, which becomes the entry.
    // Renaming also keeps repeated evals of identical source from colliding.
    bool has_entry = false;
    for (auto &f : funcs) {
        if (!f) {
            continue;
        }
        if (f->name == wrapper_name) {
            has_entry = true;
        } else if (f->name == eval_toplevel) {
            f->name = wrapper_name;
            has_entry = true;
        }
    }

    if (!external_eval_source) {
        return runtime_fatal("eval is unavailable: no active execution tier", call);
    }

    // Compile into the running chunk and execute. Any `__top_level__@` bodies from
    // imports in the eval'd source run first, in source order. No entry (declarations
    // only) yields null.
    Value result = external_eval_source(funcs, has_entry ? wrapper_name : std::string());

    // Retain the parsed AST for the rest of the process: class declarations reached
    // through the parser registry point into these nodes.
    for (auto &f : funcs) {
        if (f) {
            functions[f->name] = std::move(f);
        }
    }
    return result;
}

Value ScriptRuntime::builtin_print(Value *argvals, size_t argc, const nari::CallExpr *) {
    std::string line;
    size_t next_arg = 0;
    if (argc > 1 && argvals[0].is_string()) {
        const std::string &format = argvals[0].get_string();
        next_arg = 1;
        for (size_t i = 0; i < format.size(); ++i) {
            if (format[i] != '%' || i + 1 >= format.size()) {
                line += format[i];
                continue;
            }

            const char token = format[i + 1];
            if (token == '%') {
                line += '%';
                ++i;
                continue;
            }
            if (next_arg >= argc || std::string_view("sdifjoOc").find(token) == std::string_view::npos) {
                line += format[i];
                continue;
            }

            const Value &value = argvals[next_arg++];
            if (token == 'c') {
                ++i;
                continue;
            }
            if (token == 'd' || token == 'f') {
                line += Value::make_float(value.as_number()).to_string();
            } else if (token == 'i') {
                const double number = value.as_number();
                if (std::isfinite(number) && number >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
                    number < -static_cast<double>(std::numeric_limits<int64_t>::min())) {
                    line += std::to_string(static_cast<int64_t>(std::trunc(number)));
                } else {
                    line += Value::make_float(number).to_string();
                }
            } else {
                line += value.to_string();
            }
            ++i;
        }
    }
    for (size_t i = next_arg; i < argc; ++i) {
        if (!line.empty() || i > next_arg) {
            line += ' ';
        }
        line += argvals[i].to_string();
    }
    line += '\n';

    if (stdout_writer) {
        stdout_writer(line);
    } else {
        fwrite(line.data(), 1, line.size(), stdout);
        fflush(stdout);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_write_stdout(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 0) {
        return Value::none();
    }

    const std::string text = argvals[0].to_string();
    if (stdout_writer) {
        stdout_writer(text);
    } else {
        fwrite(text.data(), 1, text.size(), stdout);
        fflush(stdout);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_write_stderr(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc == 0) {
        return Value::none();
    }

    const std::string text = argvals[0].to_string();
    fwrite(text.data(), 1, text.size(), stderr);
    fflush(stderr);
    return Value::none();
}

// panic(value): raise an uncatchable panic that unwinds to the top of the program.
// this essentially aborts with an error value
Value ScriptRuntime::builtin_panic(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        flags.throw_value = argvals[0];
    } else {
        flags.throw_value = Value::none();
    }
    flags.throw_flag = true;
    return Value::none();
}

// Internal recovery boundary for transpilers and compatibility runtimes.
// Normal panic() calls remain fatal unless they occur inside this callback.
Value ScriptRuntime::builtin_nari_catch(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc != 1 || !argvals[0].is_function()) {
        return runtime_fatal("__nari_catch expects a single function argument", call);
    }

    if (external_catch_function_value) {
        return external_catch_function_value(argvals[0]);
    }

    Value result = call_function_value(argvals[0], {});
    if (Runtime::g_runtime_error_occurred.load()) {
        // Fatal panics stay uncatchable: propagate as none, the VM converts
        // the latched flag into has_error at the builtin boundary.
        return Value::none();
    }
    if (flags.throw_flag) {
        Value error = take_pending_throw();
        return make_err(error);
    }
    return make_ok(result);
}

// Invoke a function value with an explicit `this` receiver plus an argument array.
// Exposed for transpiled JS: its dynamic method-dispatch shims need to bind a
// receiver *without* writing a scratch property onto the receiver object.
Value ScriptRuntime::builtin_nari_invoke_with_this(Value *argvals, size_t argc, const nari::CallExpr *call) {
    if (argc < 2 || !argvals[0].is_function()) {
        return runtime_fatal("__nari_invoke_with_this expects (function, receiver, args_array)", call);
    }
    Value fn = argvals[0];
    Value receiver = argvals[1];
    GcTempRoot temp_root(*this);
    temp_root.add(&fn);
    temp_root.add(&receiver);
    if (argc < 3 || !argvals[2].is_array()) {
        return call_function_value(fn, nullptr, 0, &receiver);
    }
    Value args_val = argvals[2];
    temp_root.add(&args_val);
    const auto &args = args_val.get_array();
    return call_function_value(fn, args.data(), args.size(), &receiver);
}

Value ScriptRuntime::builtin_setTimeout(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        Value callback_val = argvals[0];
        int64_t delay_ms = 0;

        if (argvals[1].is_int()) {
            delay_ms = argvals[1].get_int();
        } else if (argvals[1].is_float()) {
            delay_ms = static_cast<int64_t>((argvals[1].get_float()));
        }

        auto io_op = std::make_shared<IOOperation>(IOOperation::Type::Timer);
        io_op->timer_ms = delay_ms;

        // capture the complete callback Value to keep lambdas alive
        io_op->callback = [this, callback_val]() {
            pending_timeouts--;
            if (callback_val.is_function()) {
                call_function_value(callback_val, {});
            }
        };

        if (io_pool) {
            async_root_set(io_op.get(), { callback_val });
            pending_timeouts++;
            io_pool->submit(io_op);
        }
    }
    return Value::none();
}

// Fires every interval whose deadline has passed. A callback may arm or clear timers,
// including its own, so ids are snapshotted and re-looked-up before each call instead
// of holding an iterator across the callback.
void ScriptRuntime::fire_due_intervals() {
    if (active_intervals.empty()) {
        return;
    }
    auto now = chrono::steady_clock::now();
    std::vector<int64_t> due;
    for (const auto &[id, interval] : active_intervals) {
        if (interval.next_fire <= now) {
            due.push_back(id);
        }
    }
    for (int64_t id : due) {
        auto it = active_intervals.find(id);
        if (it == active_intervals.end()) {
            continue; // cleared by an earlier callback in this same batch
        }
        Value callback = it->second.callback;
        int64_t period = it->second.interval_ms;
        if (callback.is_function()) {
            call_function_value(callback, {});
        }
        if (flags.throw_flag || Runtime::g_shutdown_requested.load() || Runtime::g_runtime_error_occurred.load()) {
            return;
        }
        it = active_intervals.find(id);
        if (it != active_intervals.end()) {
            // reschedule from completion, so a callback slower than its period cannot
            // build a backlog of instantly-due firings
            it->second.next_fire = chrono::steady_clock::now() + chrono::milliseconds(period);
        }
    }
}

Value ScriptRuntime::builtin_setInterval(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        Value callback_val = argvals[0];
        int64_t delay_ms = 0;

        if (argvals[1].is_int()) {
            delay_ms = argvals[1].get_int();
        } else if (argvals[1].is_float()) {
            delay_ms = static_cast<int64_t>((argvals[1].get_float()));
        }

        IntervalData interval;
        interval.id = next_interval_id++;
        interval.callback = callback_val;
        interval.interval_ms = delay_ms;
        interval.next_fire = chrono::steady_clock::now() + chrono::milliseconds(delay_ms);

        active_intervals[interval.id] = interval;

        return Value::make_int(interval.id);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_clearInterval(Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc > 0) && argvals[0].is_int()) {
        int64_t id = argvals[0].get_int();
        active_intervals.erase(id);
    }
    return Value::none();
}

// Math builtins
Value ScriptRuntime::builtin_math_sqrt(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        double num = 0.0;
        if (argvals[0].is_int()) {
            num = static_cast<double>((argvals[0].get_int()));
        } else if (argvals[0].is_float()) {
            num = argvals[0].get_float();
        }
        if (num >= 0.0) {
            return Value::make_float(std::sqrt(num));
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_math_rand(Value *argvals, size_t argc, const nari::CallExpr *) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    return Value::make_float(dis(gen));
};

Value ScriptRuntime::builtin_math_sin(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::sin(argvals[0].as_number()));
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_math_cos(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::cos(argvals[0].as_number()));
    }
    return Value::make_float(1.0);
}

Value ScriptRuntime::builtin_math_tan(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::tan(argvals[0].as_number()));
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_math_log(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::log(argvals[0].as_number()));
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_math_exp(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::exp(argvals[0].as_number()));
    }
    return Value::make_float(1.0);
}

Value ScriptRuntime::builtin_math_atan(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::atan(argvals[0].as_number()));
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_math_atan2(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        return Value::make_float(std::atan2(argvals[0].as_number(), argvals[1].as_number()));
    }
    return Value::make_float(0.0);
}

// File system builtins
Value ScriptRuntime::builtin_fs_readFile(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        std::string path = argvals[0].to_string();

        auto io_op = std::make_shared<NariFileOperation>(IOOperation::Type::FileRead);
        io_op->file_path = path;

        auto handle = Value::make_handle_ptr();
        handle->state = HandleData::Running;

        // The callback captures `handle`, keeping the HandleData alive until the
        // IO completes; the GC reclaims it once nothing references it.
        io_op->callback = [this, handle, io_op]() {
            handle->end_time = chrono::steady_clock::now();
            if (io_op->success) {
                if (io_op->result.type == NariFileOperation::Result::Type::String) {
                    handle->result = ScriptRuntime::make_ok(Value::make_string(io_op->result.str_value));
                } else if (io_op->result.type == NariFileOperation::Result::Type::Bool) {
                    handle->result = ScriptRuntime::make_ok(Value::make_bool(io_op->result.bool_value));
                }
                handle->state = HandleData::Completed;
            } else {
                handle->result = ScriptRuntime::make_err(Value::make_string(io_op->error_msg));
                handle->state = HandleData::Completed;
            }
        };

        if (io_pool) {
            io_pool->submit(io_op);
        }
        return Value::make_handle(handle);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_fs_writeFile(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        std::string path = argvals[0].to_string();
        std::string content = argvals[1].to_string();

        auto io_op = std::make_shared<NariFileOperation>(IOOperation::Type::FileWrite);
        io_op->file_path = path;
        io_op->file_content = std::move(content);

        auto handle = Value::make_handle_ptr();
        handle->state = HandleData::Running;

        io_op->callback = [this, handle, io_op]() {
            handle->end_time = chrono::steady_clock::now();
            if (io_op->success) {
                handle->result = ScriptRuntime::make_ok(Value::none());
                handle->state = HandleData::Completed;
            } else {
                handle->result = ScriptRuntime::make_err(Value::make_string(io_op->error_msg));
                handle->state = HandleData::Completed;
            }
        };

        if (io_pool) {
            io_pool->submit(io_op);
        }
        return Value::make_handle(handle);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_fs_appendFile(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        std::string path = argvals[0].to_string();
        std::string content = argvals[1].to_string();

        auto io_op = std::make_shared<NariFileOperation>(IOOperation::Type::FileAppend);
        io_op->file_path = path;
        io_op->file_content = std::move(content); // avoid copying data unnecessarily

        auto handle = Value::make_handle_ptr();
        handle->state = HandleData::Running;

        io_op->callback = [this, handle, io_op]() {
            handle->end_time = chrono::steady_clock::now();
            if (io_op->success) {
                handle->result = ScriptRuntime::make_ok(Value::none());
                handle->state = HandleData::Completed;
            } else {
                handle->result = ScriptRuntime::make_err(Value::make_string(io_op->error_msg));
                handle->state = HandleData::Completed;
            }
        };

        if (io_pool) {
            io_pool->submit(io_op);
        }
        return Value::make_handle(handle);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_fs_fileExists(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        std::string path = argvals[0].to_string();

        auto io_op = std::make_shared<NariFileOperation>(IOOperation::Type::FileExists);
        io_op->file_path = path;

        auto handle = Value::make_handle_ptr();
        handle->state = HandleData::Running;

        io_op->callback = [handle, io_op]() {
            handle->end_time = chrono::steady_clock::now();
            handle->result = Value::make_bool(io_op->result_bool);
            handle->state = HandleData::Completed;
        };

        if (io_pool) {
            io_pool->submit(io_op);
        }
        return Value::make_handle(handle);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_fs_isDirectory(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        std::string path = argvals[0].to_string();
        std::error_code err;
        bool is_dir = nari::fs::is_directory(nari::fs::Path(path), err);
        return Value::make_bool(!err && is_dir);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_fs_mkdirAll(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        std::string path = argvals[0].to_string();
        std::error_code err;
        bool created = nari::fs::create_directories(nari::fs::Path(path), err);
        if (err) {
            return Value::make_bool(false);
        }
        if (!created) {
            std::error_code is_dir_err;
            return Value::make_bool(nari::fs::is_directory(nari::fs::Path(path), is_dir_err));
        }
        return Value::make_bool(true);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_fs_deleteFile(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        std::string path = argvals[0].to_string();

        auto io_op = std::make_shared<NariFileOperation>(IOOperation::Type::FileDelete);
        io_op->file_path = path;

        auto handle = Value::make_handle_ptr();
        handle->state = HandleData::Running;

        io_op->callback = [this, handle, io_op]() {
            handle->end_time = chrono::steady_clock::now();
            if (io_op->success) {
                handle->result = ScriptRuntime::make_ok(Value::make_bool(io_op->result_bool));
                handle->state = HandleData::Completed;
            } else {
                handle->result = ScriptRuntime::make_err(Value::make_string(io_op->error_msg));
                handle->state = HandleData::Completed;
            }
        };

        if (io_pool) {
            io_pool->submit(io_op);
        }
        return Value::make_handle(handle);
    }
    return Value::none();
}

Value ScriptRuntime::builtin_fs_listDir(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        std::string path = argvals[0].to_string();

        auto io_op = std::make_shared<NariFileOperation>(IOOperation::Type::ListDir);
        io_op->file_path = path;

        auto handle = Value::make_handle_ptr();
        handle->state = HandleData::Running;

        io_op->callback = [this, handle, io_op]() {
            handle->end_time = chrono::steady_clock::now();
            if (io_op->success) {
                if (io_op->result.type == NariFileOperation::Result::Type::String) {
                    handle->result = ScriptRuntime::make_ok(Value::make_string(io_op->result.str_value));
                } else if (io_op->result.type == NariFileOperation::Result::Type::Array) {
                    std::vector<Value> arr;
                    for (const auto &item : io_op->result.array_value) {
                        arr.push_back(Value::make_string(item));
                    }
                    handle->result = ScriptRuntime::make_ok(Value::make_array(std::move(arr)));
                } else if (io_op->result.type == NariFileOperation::Result::Type::Bool) {
                    handle->result = ScriptRuntime::make_ok(Value::make_bool(io_op->result.bool_value));
                }
                handle->state = HandleData::Completed;
            } else {
                handle->result = ScriptRuntime::make_err(Value::make_string(io_op->error_msg));
                handle->state = HandleData::Completed;
            }
        };

        if (io_pool) {
            io_pool->submit(io_op);
        }
        return Value::make_handle(handle);
    }
    return Value::none();
}
