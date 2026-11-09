// Network builtins: TCP/UDP sockets (split out of core.cpp).
#include "common.h"

namespace chrono = std::chrono;

#ifndef DISABLE_HTTP
// Network builtins
Value ScriptRuntime::builtin_net_createServer(Value *argvals, size_t argc, const nari::CallExpr *) {
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
                            GcTempRoot temp_root(*this);
                            temp_root.add(&conn_obj);
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

Value ScriptRuntime::builtin_net_conn_read(Value *argvals, size_t argc, const nari::CallExpr *) {
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
                        call_function_value(callback_val, { Value::none(), Value::make_string(read_op->result_string) });
                    } else {
                        call_function_value(callback_val, { Value::make_string(read_op->error_msg), Value::none() });
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

Value ScriptRuntime::builtin_net_conn_write(Value *argvals, size_t argc, const nari::CallExpr *) {
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

Value ScriptRuntime::builtin_net_conn_close(Value *argvals, size_t argc, const nari::CallExpr *) {
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

// Build the raw conn object
// Shared by listen/accept/connect.
static Value build_conn_object(int fd, const std::string &remote_ip, int remote_port) {
    auto conn_obj = Value::make_object();
    ObjectObj *obj_ptr = conn_obj.get_obj_ptr();
    obj_ptr->set_field("fd", Value::make_int(fd));
    obj_ptr->set_field("ip", Value::make_string(remote_ip));
    obj_ptr->set_field("port", Value::make_int(remote_port));
    return conn_obj;
}

// TCP client: net.connect(host, port) -> handle resolving to conn { fd, ip, port, read, write, close }
Value ScriptRuntime::builtin_net_connect(Value *argvals, size_t argc, const nari::CallExpr *) {
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

// TCP listen:
//  net.listen(port) -> handle resolving to server { fd, port, accept, close }
//  port == 0 -> kernel-assigned ephemeral port (read back into server.port).
Value ScriptRuntime::builtin_net_listen(Value *argvals, size_t argc, const nari::CallExpr *) {
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
            ObjectObj *obj_ptr = server_obj.get_obj_ptr();
            obj_ptr->set_field("fd", Value::make_int(listen_op->socket_fd));
            obj_ptr->set_field("port", Value::make_int(listen_op->port));
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
Value ScriptRuntime::builtin_net_accept(Value *argvals, size_t argc, const nari::CallExpr *) {
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
            handle->result = ScriptRuntime::make_ok(build_conn_object(accept_op->client_fd, accept_op->client_ip, accept_op->client_port));
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
Value ScriptRuntime::builtin_net_server_close(Value *argvals, size_t argc, const nari::CallExpr *) {
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
    ObjectObj *obj_ptr = sock_obj.get_obj_ptr();
    obj_ptr->set_field("fd", Value::make_int(fd));
    obj_ptr->set_field("port", Value::make_int(port));
    return sock_obj;
}

// UDP bind: net.udp_socket(port?) -> handle resolving to { fd, port, send, recv, close }
// port omitted or 0 -> ephemeral.
Value ScriptRuntime::builtin_udp_bind(Value *argvals, size_t argc, const nari::CallExpr *) {
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
// argvals[0] is the socket object, argvals[1]=host, argvals[2]=port, argvals[3]=data.
Value ScriptRuntime::builtin_udp_send(Value *argvals, size_t argc, const nari::CallExpr *) {
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
Value ScriptRuntime::builtin_udp_recv(Value *argvals, size_t argc, const nari::CallExpr *) {
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
            ObjectObj *obj_ptr = result.get_obj_ptr();
            obj_ptr->set_field("data", Value::make_string(recv_op->result_string));
            obj_ptr->set_field("ip", Value::make_string(recv_op->from_ip));
            obj_ptr->set_field("port", Value::make_int(recv_op->from_port));
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
Value ScriptRuntime::builtin_udp_close(Value *argvals, size_t argc, const nari::CallExpr *) {
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
#endif // !DISABLE_HTTP
