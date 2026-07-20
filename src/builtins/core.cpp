#include "common.h"

#include <cmath>

namespace chrono = std::chrono;

// eval(source): parse and execute Nari source at runtime, returning the value
// of the expression (or of an explicit return) in the source.
Value ScriptRuntime::builtin_eval(const Value *argvals, size_t argc, const nari::CallExpr *call) {
#ifdef DISABLE_PARSER
    runtime_fatal("eval is unavailable in this build (parser disabled)", call);
    return Value::none();
#else
    if (argc != 1 || !argvals[0].is_string()) {
        runtime_fatal("eval expects a single string argument", call);
        return Value::none();
    }
    const std::string src = argvals[0].get_string();

    static size_t eval_counter = 0;
    const std::string wrapper_name = "__eval_fn_" + std::to_string(eval_counter++);
    const std::string eval_toplevel = "__top_level__@<eval>";
    const std::string saved_filename =
        (call && !call->filename.empty()) ? call->filename : "<eval>";

    auto parse_src = [&](bool wrap, std::vector<Parser::ParseError> &errors) -> FuncList {
        Parser::set_source_filename("<eval>");
        
        Parser::ParseResult result = Parser::parse_program_recovering(
                wrap ? "func " + wrapper_name + "() {\nreturn (" + src + ");\n}\n" : src,
                false);

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
        runtime_fatal(msg, call);
        return Value::none();
    }

    // add parsed function to running program
    std::vector<Function *> import_toplevels;
    Function *wrapper_fn = nullptr;
    for (auto &f : funcs) {
        if (!f) {
            continue;
        }
        if (f->name == wrapper_name) {
            wrapper_fn = f.get();
        } else if (f->name == eval_toplevel) {
            // rename so that repeated evals of the same content don't collide
            f->name = wrapper_name;
            wrapper_fn = f.get();
        } else if (f->name.find("__top_level__@") == 0) {
            // imported modules carry their own top-level functions
            import_toplevels.push_back(f.get());
            toplevel_order.push_back(f->name);
        }
        functions[f->name] = std::move(f);
    }
    
    for (Function *toplevel : import_toplevels) {
        execute_toplevel_function(toplevel);
    }

    if (!wrapper_fn) {
        // declarations only, nothing to run
        return Value::none();
    }
    return call_user_function(wrapper_fn, {});
#endif
}

Value ScriptRuntime::builtin_print(const Value *argvals, size_t argc, const nari::CallExpr *) {
    std::string line;
    for (size_t i = 0; i < argc; ++i) {
        if (i) {
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

// panic(value): raise an uncatchable panic that unwinds to the top of the program.
// this essentially aborts with an error value
Value ScriptRuntime::builtin_panic(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        flags.throw_value = argvals[0];
    } else {
        flags.throw_value = Value::none();
    }
    flags.throw_flag = true;
    return Value::none();
}

Value ScriptRuntime::builtin_setTimeout(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        Value callback_val = argvals[0];
        int64_t delay_ms = 0;

        if (argvals[1].is_int()) {
            delay_ms = argvals[1].get_int();
        } else if (argvals[1].is_float()) {
            delay_ms = static_cast<int64_t>(argvals[1].get_float());
        }

        auto io_op = std::make_shared<IOOperation>(IOOperation::Type::Timer);
        io_op->timer_ms = delay_ms;

        // capture the complete callback Value to keep lambdas alive
        io_op->callback = [this, callback_val]() {
            if (callback_val.is_function()) {
                call_function_value(callback_val, {});
            }
        };

        if (io_pool) {
            async_root_set(io_op.get(), { callback_val });
            io_pool->submit(io_op);
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_setInterval(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        Value callback_val = argvals[0];
        int64_t delay_ms = 0;

        if (argvals[1].is_int()) {
            delay_ms = argvals[1].get_int();
        } else if (argvals[1].is_float()) {
            delay_ms = static_cast<int64_t>(argvals[1].get_float());
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

Value ScriptRuntime::builtin_clearInterval(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if ((argc > 0) && argvals[0].is_int()) {
        int64_t id = argvals[0].get_int();
        active_intervals.erase(id);
    }
    return Value::none();
}

// Math builtins
Value ScriptRuntime::builtin_math_sqrt(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        double num = 0.0;
        if (argvals[0].is_int()) {
            num = static_cast<double>(argvals[0].get_int());
        } else if (argvals[0].is_float()) {
            num = argvals[0].get_float();
        }
        if (num >= 0.0) {
            return Value::make_float(std::sqrt(num));
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_math_rand(const Value *argvals, size_t argc, const nari::CallExpr *) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.0, 1.0);
    return Value::make_float(dis(gen));
};

Value ScriptRuntime::builtin_math_sin(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::sin(argvals[0].as_number()));
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_math_cos(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::cos(argvals[0].as_number()));
    }
    return Value::make_float(1.0);
}

Value ScriptRuntime::builtin_math_tan(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::tan(argvals[0].as_number()));
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_math_log(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::log(argvals[0].as_number()));
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_math_exp(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::exp(argvals[0].as_number()));
    }
    return Value::make_float(1.0);
}

Value ScriptRuntime::builtin_math_atan(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc > 0) {
        return Value::make_float(std::atan(argvals[0].as_number()));
    }
    return Value::make_float(0.0);
}

Value ScriptRuntime::builtin_math_atan2(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        return Value::make_float(std::atan2(argvals[0].as_number(), argvals[1].as_number()));
    }
    return Value::make_float(0.0);
}

// File system builtins
Value ScriptRuntime::builtin_fs_readFile(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_fs_writeFile(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_fs_appendFile(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_fs_fileExists(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_fs_isDirectory(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1) {
        std::string path = argvals[0].to_string();
        std::error_code err;
        bool is_dir = nari::fs::is_directory(nari::fs::Path(path), err);
        return Value::make_bool(!err && is_dir);
    }
    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_fs_mkdirAll(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_fs_deleteFile(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_fs_listDir(const Value *argvals, size_t argc, const nari::CallExpr *) {
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

#ifndef DISABLE_HTTP
// Network builtins
Value ScriptRuntime::builtin_net_createServer(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2) {
        int port = static_cast<int>(argvals[0].as_number());
        Value callback_val = argvals[1];

        auto listen_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpListen);
        listen_op->port = port;
        // Capture the complete callback Value to keep lambdas alive
        listen_op->callback = [this, callback_val, listen_op, port]() {
            if (!listen_op->success) {
                fprintf(stderr, "Failed to create server: %s\n", listen_op->error_msg.c_str());
                return;
            }

            int server_fd = listen_op->socket_fd;

#ifndef NO_THREADS
            // Register server socket for shutdown
            {
                std::lock_guard<std::mutex> lock(server_sockets_mutex);
                server_sockets.push_back(server_fd);
            }

            auto accept_loop = [this, server_fd, callback_val]() {
                while (!Runtime::g_shutdown_requested.load()) {
                    auto accept_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpAccept);
                    accept_op->socket_fd = server_fd;

                    accept_op->callback = [this, callback_val, accept_op]() {
                        if (!accept_op->success) {
                            return;
                        }

                        auto conn_obj = Value::make_object();
                        ObjectObj *conn_oobj = conn_obj.get_obj_ptr();
                        conn_oobj->set_field("fd", Value::make_int(accept_op->client_fd));
                        conn_oobj->set_field("ip", Value::make_string(accept_op->client_ip));
                        conn_oobj->set_field("port", Value::make_int(accept_op->client_port));
                        conn_oobj->set_field("read", Value::make_function("__net_conn_read"));
                        conn_oobj->set_field("write", Value::make_function("__net_conn_write"));
                        conn_oobj->set_field("close", Value::make_function("__net_conn_close"));

                        if (callback_val.is_function()) {
                            // conn_obj is a C++ local live across the handler call
                            // (nested bytecode = safe-points); root it for that call.
                            GcTempRoot _gr(*this);
                            _gr.add(&conn_obj);
                            call_function_value(callback_val, { conn_obj });
                        }
                    };

                    if (io_pool) {
                        io_pool->submit(accept_op);
                    }

                    while (!accept_op->completed && !Runtime::g_shutdown_requested.load()) {
                        NARI_SLEEP_MILLIS(10);
                    }

                    if (Runtime::g_shutdown_requested.load()) {
                        break;
                    }
                }

                // dtor handles closing
            };

            std::lock_guard<std::mutex> lock(accept_threads_mutex);
            accept_threads.emplace_back(accept_loop);
#else
            // No threading support - can't run server
            printf("Attempted to start server on port %d, but threading is disabled. Server will not run!\n", port);
#endif
        };

        if (io_pool) {
            // The server handler is held by the long-lived accept thread; root it
            // for the server's lifetime so the GC won't sweep it.
            persistent_root_add(callback_val);
            io_pool->submit(listen_op);
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_net_conn_read(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_object()) {
        const ObjectObj *conn = argvals[0].get_obj_ptr();
        const Value *fd_v = conn->get_field("fd");
        if (fd_v && fd_v->is_int()) {
            int fd = static_cast<int>(fd_v->get_int());
            Value callback_val = argvals[1];

            auto read_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpRead);
            read_op->socket_fd = fd;
            // capture the complete callback Value to keep lambdas alive
            read_op->callback = [this, callback_val, read_op]() {
                if (callback_val.is_function()) {
                    if (read_op->success) {
                        call_function_value(
                            callback_val,
                            { Value::none(), Value::make_string(read_op->result_string) });
                    } else {
                        call_function_value(
                            callback_val,
                            { Value::make_string(read_op->error_msg), Value::none() });
                    }
                }
            };

            if (io_pool) {
                async_root_set(read_op.get(), { callback_val });
                io_pool->submit(read_op);
            }
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_net_conn_write(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_object()) {
        const ObjectObj *conn = argvals[0].get_obj_ptr();

        const Value *fd2_v = conn->get_field("fd");
        if (fd2_v && fd2_v->is_int()) {
            int fd = static_cast<int>(fd2_v->get_int());
            std::string data = argvals[1].to_string();

            auto handle = Value::make_handle_ptr();
            handle->state = HandleData::Running;
            auto write_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpWrite);
            write_op->socket_fd = fd;
            write_op->data = data;
            write_op->callback = [this, handle, write_op]() {
                if (write_op->success) {
                    handle->result = ScriptRuntime::make_ok(Value::none());
                    handle->state = HandleData::Completed;
                } else {
                    handle->result = ScriptRuntime::make_err(Value::make_string(write_op->error_msg));
                    handle->state = HandleData::Completed;
                }
            };

            if (io_pool) {
                io_pool->submit(write_op);
            }
            return Value::make_handle(handle);
        }
    }
    return Value::none();
}

Value ScriptRuntime::builtin_net_conn_close(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1 && argvals[0].is_object()) {
        const ObjectObj *conn = argvals[0].get_obj_ptr();

        const Value *fd3_v = conn->get_field("fd");
        if (fd3_v && fd3_v->is_int()) {
            int fd = static_cast<int>(fd3_v->get_int());

            auto close_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpClose);
            close_op->socket_fd = fd;
            close_op->callback = []() {};

            if (io_pool) {
                io_pool->submit(close_op);
            }
        }
    }
    return Value::none();
}

// Build the raw conn object exposed to scripts.
// Shared by listen/accept/connect.
static Value build_conn_object(int fd, const std::string &remote_ip, int remote_port) {
    auto conn_obj = Value::make_object();
    ObjectObj *o = conn_obj.get_obj_ptr();
    o->set_field("fd", Value::make_int(fd));
    o->set_field("ip", Value::make_string(remote_ip));
    o->set_field("port", Value::make_int(remote_port));
    return conn_obj;
}

// TCP client: net.connect(host, port) -> handle resolving to conn { fd, ip, port, read, write, close }
Value ScriptRuntime::builtin_net_connect(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2) {
        return Value::none();
    }
    std::string host = argvals[0].to_string();
    int port = static_cast<int>(argvals[1].as_number());

    auto connect_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpConnect);
    connect_op->host = host;
    connect_op->port = port;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    connect_op->callback = [this, handle, connect_op, host, port]() {
        handle->end_time = chrono::steady_clock::now();
        if (connect_op->success) {
            handle->result = ScriptRuntime::make_ok(build_conn_object(connect_op->socket_fd, host, port));
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(connect_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    if (io_pool) {
        io_pool->submit(connect_op);
    }
    return Value::make_handle(handle);
}

// TCP listen: net.listen(port) -> handle resolving to server { fd, port, accept, close }
// port == 0 -> kernel-assigned ephemeral port (read back into server.port).
Value ScriptRuntime::builtin_net_listen(const Value *argvals, size_t argc, const nari::CallExpr *) {
    int port = 0;
    if (argc >= 1) {
        port = static_cast<int>(argvals[0].as_number());
    }

    auto listen_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpListen);
    listen_op->port = port;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    listen_op->callback = [this, handle, listen_op]() {
        handle->end_time = chrono::steady_clock::now();
        if (listen_op->success) {
            auto server_obj = Value::make_object();
            ObjectObj *o = server_obj.get_obj_ptr();
            o->set_field("fd", Value::make_int(listen_op->socket_fd));
            o->set_field("port", Value::make_int(listen_op->port));
            handle->result = ScriptRuntime::make_ok(server_obj);
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(listen_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    if (io_pool) {
        io_pool->submit(listen_op);
    }
    return Value::make_handle(handle);
}

// accept a single connection from a server. server.accept() -> handle resolving to conn.
Value ScriptRuntime::builtin_net_accept(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_object()) {
        return Value::none();
    }
    const ObjectObj *server = argvals[0].get_obj_ptr();
    const Value *fd_v = server->get_field("fd");
    if (!fd_v || !fd_v->is_int()) {
        return Value::none();
    }
    int server_fd = static_cast<int>(fd_v->get_int());

    auto accept_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpAccept);
    accept_op->socket_fd = server_fd;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    accept_op->callback = [this, handle, accept_op]() {
        handle->end_time = chrono::steady_clock::now();
        if (accept_op->success) {
            handle->result = ScriptRuntime::make_ok(build_conn_object(
                accept_op->client_fd, accept_op->client_ip, accept_op->client_port));
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(accept_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    if (io_pool) {
        io_pool->submit(accept_op);
    }
    return Value::make_handle(handle);
}

// Close a server socket. server.close() -> none.
Value ScriptRuntime::builtin_net_server_close(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_object()) {
        return Value::none();
    }
    const ObjectObj *server = argvals[0].get_obj_ptr();
    const Value *fd_v = server->get_field("fd");
    if (!fd_v || !fd_v->is_int()) {
        return Value::none();
    }
    int fd = static_cast<int>(fd_v->get_int());

    auto close_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpClose);
    close_op->socket_fd = fd;
    close_op->callback = []() {};

    if (io_pool) {
        io_pool->submit(close_op);
    }
    return Value::none();
}

// Build the raw UDP socket data object. Prelude wraps it with closures.
static Value build_udp_socket_object(int fd, int port) {
    auto sock_obj = Value::make_object();
    ObjectObj *o = sock_obj.get_obj_ptr();
    o->set_field("fd", Value::make_int(fd));
    o->set_field("port", Value::make_int(port));
    return sock_obj;
}

// UDP bind: net.udp_socket(port?) -> handle resolving to { fd, port, send, recv, close }
// port omitted or 0 -> ephemeral.
Value ScriptRuntime::builtin_udp_bind(const Value *argvals, size_t argc, const nari::CallExpr *) {
    int port = 0;
    if (argc >= 1 && !argvals[0].is_none()) {
        port = static_cast<int>(argvals[0].as_number());
    }

    auto bind_op = std::make_shared<UdpOperation>(IOOperation::Type::UdpBind);
    bind_op->port = port;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    bind_op->callback = [this, handle, bind_op]() {
        handle->end_time = chrono::steady_clock::now();
        if (bind_op->success) {
            handle->result = ScriptRuntime::make_ok(build_udp_socket_object(bind_op->socket_fd, bind_op->bound_port));
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(bind_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    if (io_pool) {
        io_pool->submit(bind_op);
    }
    return Value::make_handle(handle);
}

// UDP send: sock.send(host, port, data) -> handle resolving to none.
// Called as a method: argvals[0] is the socket object, argvals[1]=host, argvals[2]=port, argvals[3]=data.
Value ScriptRuntime::builtin_udp_send(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 4 || !argvals[0].is_object()) {
        return Value::none();
    }
    const ObjectObj *sock = argvals[0].get_obj_ptr();
    const Value *fd_v = sock->get_field("fd");
    if (!fd_v || !fd_v->is_int()) {
        return Value::none();
    }
    int fd = static_cast<int>(fd_v->get_int());
    std::string host = argvals[1].to_string();
    int port = static_cast<int>(argvals[2].as_number());
    std::string data = argvals[3].to_string();

    auto send_op = std::make_shared<UdpOperation>(IOOperation::Type::UdpSend);
    send_op->socket_fd = fd;
    send_op->host = host;
    send_op->port = port;
    send_op->data = std::move(data);

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    send_op->callback = [this, handle, send_op]() {
        handle->end_time = chrono::steady_clock::now();
        if (send_op->success) {
            handle->result = ScriptRuntime::make_ok(Value::make_int(static_cast<int64_t>(send_op->data.size())));
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(send_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    if (io_pool) {
        io_pool->submit(send_op);
    }
    return Value::make_handle(handle);
}

// UDP recv: sock.recv(timeout_ms?) -> handle resolving to { data, ip, port }.
// timeout_ms omitted or <0 -> blocks until a datagram is received or shutdown.
Value ScriptRuntime::builtin_udp_recv(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_object()) {
        return Value::none();
    }
    const ObjectObj *sock = argvals[0].get_obj_ptr();
    const Value *fd_v = sock->get_field("fd");
    if (!fd_v || !fd_v->is_int()) {
        return Value::none();
    }
    int fd = static_cast<int>(fd_v->get_int());

    int timeout_ms = -1;
    if (argc >= 2 && !argvals[1].is_none()) {
        timeout_ms = static_cast<int>(argvals[1].as_number());
    }

    auto recv_op = std::make_shared<UdpOperation>(IOOperation::Type::UdpRecv);
    recv_op->socket_fd = fd;
    recv_op->timeout_ms = timeout_ms;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    recv_op->callback = [this, handle, recv_op]() {
        handle->end_time = chrono::steady_clock::now();
        if (recv_op->success) {
            auto result = Value::make_object();
            ObjectObj *o = result.get_obj_ptr();
            o->set_field("data", Value::make_string(recv_op->result_string));
            o->set_field("ip", Value::make_string(recv_op->from_ip));
            o->set_field("port", Value::make_int(recv_op->from_port));
            handle->result = ScriptRuntime::make_ok(result);
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(recv_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    if (io_pool) {
        io_pool->submit(recv_op);
    }
    return Value::make_handle(handle);
}

// UDP close: sock.close() -> none.
Value ScriptRuntime::builtin_udp_close(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 1 || !argvals[0].is_object()) {
        return Value::none();
    }
    const ObjectObj *sock = argvals[0].get_obj_ptr();
    const Value *fd_v = sock->get_field("fd");
    if (!fd_v || !fd_v->is_int()) {
        return Value::none();
    }
    int fd = static_cast<int>(fd_v->get_int());

    auto close_op = std::make_shared<UdpOperation>(IOOperation::Type::UdpClose);
    close_op->socket_fd = fd;
    close_op->callback = []() {};

    if (io_pool) {
        io_pool->submit(close_op);
    }
    return Value::none();
}

// HTTP builtins
// Like builtin_http_get but accepts an options object { url, method, headers, body }
Value ScriptRuntime::builtin_http_fetch(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc < 2) {
        return Value::none();
    }

    std::string url = argvals[0].to_string();
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    std::string body;

    if (argvals[1].is_object()) {
        const ObjectObj *opts = argvals[1].get_obj_ptr();
        const Value *method_v = opts->get_field("method");
        if (method_v) {
            method = method_v->to_string();
        }

        const Value *headers_v = opts->get_field("headers");
        if (headers_v && headers_v->is_object()) {
            const ObjectObj *hdrs = headers_v->get_obj_ptr();
            for (const auto &name : hdrs->get_keys()) {
                if (const Value *val = hdrs->get_field(name)) {
                    headers[name] = val->to_string();
                }
            }
        }

        const Value *body_v = opts->get_field("body");
        if (body_v && !body_v->is_none()) {
            body = body_v->to_string();
        }
    }

    std::string full_url = url;
    std::string host;
    int port = 80;
    std::string path = "/";

    size_t proto_end = url.find("://");
    if (proto_end != std::string::npos) {
        url = url.substr(proto_end + 3);
    }

    size_t path_start = url.find('/');
    std::string host_port;
    if (path_start != std::string::npos) {
        host_port = url.substr(0, path_start);
        path = url.substr(path_start);
    } else {
        host_port = url;
    }

    size_t port_pos = host_port.find(':');
    if (port_pos != std::string::npos) {
        host = host_port.substr(0, port_pos);
        port = std::stoi(host_port.substr(port_pos + 1));
    } else {
        host = host_port;
    }

    auto http_op = std::make_shared<HttpOperation>();
    http_op->host = host;
    http_op->port = port;
    http_op->url_path = path;
    http_op->full_url = full_url;
    http_op->method = method;
    http_op->body = body;
    http_op->headers = headers;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;

    http_op->callback = [this, handle, http_op]() {
        handle->end_time = chrono::steady_clock::now();
        if (http_op->success) {
            auto response = Value::make_object();
            ObjectObj *resp_oobj = response.get_obj_ptr();
            resp_oobj->set_field("status_code", Value::make_int(http_op->status_code));
            resp_oobj->set_field("body", Value::make_string(http_op->result_string));
            auto resp_headers = Value::make_object();
            ObjectObj *hdrs_oobj = resp_headers.get_obj_ptr();
            for (const auto &[k, v] : http_op->response_headers) {
                hdrs_oobj->set_field(k, Value::make_string(v));
            }
            resp_oobj->set_field("headers", resp_headers);
            handle->result = ScriptRuntime::make_ok(response);
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(http_op->error_msg));
            handle->state = HandleData::Completed;
        }
        // Free large response data, process_completed_io will null this callback.
        http_op->result_string = {};
        http_op->response_headers.clear();
    };

    if (io_pool) {
        io_pool->submit(http_op);
    }
    return Value::make_handle(handle);
}
#endif // !DISABLE_HTTP
