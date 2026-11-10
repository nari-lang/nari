// Network builtins: TCP/UDP sockets (split out of core.cpp).
#include "common.h"

namespace chrono = std::chrono;

#ifndef DISABLE_HTTP
// Network builtins
Value ScriptRuntime::builtin_net_createServer(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[1].is_function() && io_pool) {
        int port = static_cast<int>(argvals[0].as_number());
        Value callback_val = argvals[1];

        auto listen_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpListen);
        listen_op->port = port;
        ++active_servers;
        listen_op->callback = [this, callback_val, listen_op = listen_op.get()]() {
            if (!listen_op->success) {
                --active_servers;
                fprintf(stderr, "Failed to create server: %s\n", listen_op->error_msg.c_str());
                return;
            }
            int64_t id = adopt_socket(std::exchange(listen_op->socket_fd, NARI_INVALID_SOCKET));
            queue_server_accept(id, callback_val);
        };

        async_root_set(listen_op.get(), { callback_val });
        io_pool->submit(listen_op);
    }
    return Value::none();
}

// Each completion schedules the next accept on the main thread. Callback Values
// never cross to a second scheduling thread, and shutdown cannot leave one
// spinning on an operation after the workers have stopped.
void ScriptRuntime::queue_server_accept(int64_t id, Value callback) {
    auto socket = find_socket(id);
    if (!socket || !io_pool || io_pool->is_stopped() || Runtime::g_shutdown_requested.load()) {
        --active_servers;
        return;
    }
    auto op = std::make_shared<TcpOperation>(IOOperation::Type::TcpAccept);
    op->socket_owner = socket;
    op->socket_fd = socket->fd;
    op->callback = [this, id, callback, op = op.get()]() {
        if (!op->success) {
            --active_servers;
            close_socket(id);
            return;
        }
        Value conn = build_conn_object(std::exchange(op->client_fd, NARI_INVALID_SOCKET), op->client_ip, op->client_port);
        GcTempRoot root(*this);
        root.add(&conn);
        // Preserve the connection methods exposed by create_server callbacks.
        ObjectObj *obj = conn.get_obj_ptr();
        obj->set_field("read", Value::make_function("__net_conn_read"));
        obj->set_field("write", Value::make_function("__net_conn_write"));
        obj->set_field("close", Value::make_function("__net_conn_close"));
        queue_server_accept(id, callback);
        call_function_value(callback, { conn });
    };
    async_root_set(op.get(), { callback });
    io_pool->submit(op);
}

Value ScriptRuntime::builtin_net_conn_read(Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 2 && argvals[0].is_object()) {
        const ObjectObj *conn = argvals[0].get_obj_ptr();
        const Value *fd_v = conn->get_field("fd");
        if (fd_v && fd_v->is_int()) {
            auto socket = find_socket(fd_v->get_int());
            Value callback_val = argvals[1];

            auto read_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpRead);
            read_op->socket_owner = socket;
            read_op->socket_fd = socket ? socket->fd : NARI_INVALID_SOCKET;
            // capture the complete callback Value to keep lambdas alive
            read_op->callback = [this, callback_val, read_op = read_op.get()]() {
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
            auto socket = find_socket(fd2_v->get_int());
            std::string data = argvals[1].to_string();

            auto handle = Value::make_handle_ptr();
            handle->state = HandleData::Running;
            auto write_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpWrite);
            write_op->socket_owner = socket;
            write_op->socket_fd = socket ? socket->fd : NARI_INVALID_SOCKET;
            write_op->data = data;
            write_op->callback = [this, handle, write_op = write_op.get()]() {
                if (write_op->success) {
                    handle->result = ScriptRuntime::make_ok(Value::none());
                    handle->state = HandleData::Completed;
                } else {
                    handle->result = ScriptRuntime::make_err(Value::make_string(write_op->error_msg));
                    handle->state = HandleData::Completed;
                }
            };

            submit_handle_io(write_op, handle);
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
            close_socket(fd3_v->get_int());
        }
    }
    return Value::none();
}

// Build the raw conn object
// Shared by listen/accept/connect.
Value ScriptRuntime::build_conn_object(nari_socket_t fd, const std::string &remote_ip, int remote_port) {
    auto conn_obj = Value::make_object();
    ObjectObj *obj_ptr = conn_obj.get_obj_ptr();
    obj_ptr->set_field("fd", Value::make_int(adopt_socket(fd)));
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
    connect_op->callback = [this, handle, connect_op = connect_op.get(), host, port]() {
        handle->end_time = chrono::steady_clock::now();
        if (connect_op->success) {
            handle->result =
                ScriptRuntime::make_ok(build_conn_object(std::exchange(connect_op->socket_fd, NARI_INVALID_SOCKET), host, port));
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(connect_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    submit_handle_io(connect_op, handle);
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
    listen_op->callback = [this, handle, listen_op = listen_op.get()]() {
        handle->end_time = chrono::steady_clock::now();
        if (listen_op->success) {
            auto server_obj = Value::make_object();
            ObjectObj *obj_ptr = server_obj.get_obj_ptr();
            obj_ptr->set_field("fd", Value::make_int(adopt_socket(std::exchange(listen_op->socket_fd, NARI_INVALID_SOCKET))));
            obj_ptr->set_field("port", Value::make_int(listen_op->port));
            handle->result = ScriptRuntime::make_ok(server_obj);
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(listen_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    submit_handle_io(listen_op, handle);
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
    auto socket = find_socket(fd_v->get_int());

    auto accept_op = std::make_shared<TcpOperation>(IOOperation::Type::TcpAccept);
    accept_op->socket_owner = socket;
    accept_op->socket_fd = socket ? socket->fd : NARI_INVALID_SOCKET;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    accept_op->callback = [this, handle, accept_op = accept_op.get()]() {
        handle->end_time = chrono::steady_clock::now();
        if (accept_op->success) {
            handle->result = ScriptRuntime::make_ok(
                build_conn_object(std::exchange(accept_op->client_fd, NARI_INVALID_SOCKET), accept_op->client_ip, accept_op->client_port)
            );
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(accept_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    submit_handle_io(accept_op, handle);
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
    close_socket(fd_v->get_int());
    return Value::none();
}

// Build the raw UDP socket data object. Prelude wraps it with closures.
Value ScriptRuntime::build_udp_socket_object(nari_socket_t fd, int port) {
    auto sock_obj = Value::make_object();
    ObjectObj *obj_ptr = sock_obj.get_obj_ptr();
    obj_ptr->set_field("fd", Value::make_int(adopt_socket(fd)));
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
    bind_op->callback = [this, handle, bind_op = bind_op.get()]() {
        handle->end_time = chrono::steady_clock::now();
        if (bind_op->success) {
            handle->result = ScriptRuntime::make_ok(
                build_udp_socket_object(std::exchange(bind_op->socket_fd, NARI_INVALID_SOCKET), bind_op->bound_port)
            );
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(bind_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    submit_handle_io(bind_op, handle);
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
    auto socket = find_socket(fd_v->get_int());
    std::string host = argvals[1].to_string();
    int port = static_cast<int>(argvals[2].as_number());
    std::string data = argvals[3].to_string();

    auto send_op = std::make_shared<UdpOperation>(IOOperation::Type::UdpSend);
    send_op->socket_owner = socket;
    send_op->socket_fd = socket ? socket->fd : NARI_INVALID_SOCKET;
    send_op->host = host;
    send_op->port = port;
    send_op->data = std::move(data);

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    send_op->callback = [this, handle, send_op = send_op.get()]() {
        handle->end_time = chrono::steady_clock::now();
        if (send_op->success) {
            handle->result = ScriptRuntime::make_ok(Value::make_int(static_cast<int64_t>(send_op->data.size())));
            handle->state = HandleData::Completed;
        } else {
            handle->result = ScriptRuntime::make_err(Value::make_string(send_op->error_msg));
            handle->state = HandleData::Completed;
        }
    };

    submit_handle_io(send_op, handle);
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
    auto socket = find_socket(fd_v->get_int());

    int timeout_ms = -1;
    if (argc >= 2 && !argvals[1].is_none()) {
        timeout_ms = static_cast<int>(argvals[1].as_number());
    }

    auto recv_op = std::make_shared<UdpOperation>(IOOperation::Type::UdpRecv);
    recv_op->socket_owner = socket;
    recv_op->socket_fd = socket ? socket->fd : NARI_INVALID_SOCKET;
    recv_op->timeout_ms = timeout_ms;

    auto handle = Value::make_handle_ptr();
    handle->state = HandleData::Running;
    recv_op->callback = [this, handle, recv_op = recv_op.get()]() {
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

    submit_handle_io(recv_op, handle);
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
    close_socket(fd_v->get_int());
    return Value::none();
}
#endif // !DISABLE_HTTP
