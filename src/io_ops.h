#pragma once
// I/O operation structs (split out of io.h).
// Included by io.h; safe to include directly (brings StringMap via core_types.h).
#include "core_types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// base I/O operation
// async work for the thread pool
struct IOOperation {
    enum class Type {
        Timer,
        FileRead,
        FileWrite,
        FileAppend,
        FileExists,
        FileDelete,
        ListDir,
#ifndef DISABLE_HTTP
        TcpListen,
        TcpAccept,
        TcpRead,
        TcpWrite,
        TcpClose,
        TcpConnect,
        UdpBind,
        UdpSend,
        UdpRecv,
        UdpClose,
        HttpRequest
#endif
    };

    Type type;
    int64_t timer_ms = 0;
    std::function<void()> callback;
    bool success = false;
    bool completed = false;

    IOOperation(Type t) : type(t) {
    }
};

// file operation
struct NariFileOperation : IOOperation {
    std::string file_path;
    std::string file_content;

    struct Result {
        enum class Type {
            None,
            String,
            Array,
            Bool
        } type = Type::None;

        union {
            std::string str_value;
            std::vector<std::string> array_value;
            bool bool_value;
        };

        Result() : type(Type::None), bool_value(false) {
        }

        ~Result() {
            clear();
        }

        Result(const Result &other) : type(other.type) {
            copy_from(other);
        }

        Result &operator=(const Result &other) {
            if (this != &other) {
                clear();
                type = other.type;
                copy_from(other);
            }
            return *this;
        }

        Result(Result &&other) noexcept : type(other.type) {
            move_from(std::move(other));
        }

        Result &operator=(Result &&other) noexcept {
            if (this != &other) {
                clear();
                type = other.type;
                move_from(std::move(other));
            }
            return *this;
        }

        void clear() {
            switch (type) {
                case Type::String:
                    str_value.~basic_string();
                    break;
                case Type::Array:
                    array_value.~vector();
                    break;
                default:
                    break;
            }
            type = Type::None;
        }

        void copy_from(const Result &other) {
            switch (other.type) {
                case Type::String:
                    new (&str_value) std::string(other.str_value);
                    break;
                case Type::Array:
                    new (&array_value) std::vector<std::string>(other.array_value);
                    break;
                case Type::Bool:
                    bool_value = other.bool_value;
                    break;
                default:
                    break;
            }
        }

        void move_from(Result &&other) {
            switch (other.type) {
                case Type::String:
                    new (&str_value) std::string(std::move(other.str_value));
                    break;
                case Type::Array:
                    new (&array_value) std::vector<std::string>(std::move(other.array_value));
                    break;
                case Type::Bool:
                    bool_value = other.bool_value;
                    break;
                default:
                    break;
            }
        }

        static Result from_string(const std::string &str) {
            Result result;
            result.type = Type::String;
            new (&result.str_value) std::string(str);
            return result;
        }

        static Result from_array(const std::vector<std::string> &arr) {
            Result result;
            result.type = Type::Array;
            new (&result.array_value) std::vector<std::string>(arr);
            return result;
        }

        static Result from_bool(bool _bool) {
            Result result;
            result.type = Type::Bool;
            result.bool_value = _bool;
            return result;
        }
    } result;

    std::string error_msg;
    bool result_bool = false;

    NariFileOperation(Type t) : IOOperation(t) {
    }
};

#ifndef DISABLE_HTTP
struct TcpOperation : IOOperation {
    int socket_fd = -1;
    int port = 0;
    int client_fd = -1;
    std::string client_ip;
    int client_port = 0;
    std::string host;
    std::string data;
    std::string result_string;
    std::string error_msg;

    TcpOperation(Type t) : IOOperation(t) {
    }
};

struct UdpOperation : IOOperation {
    int socket_fd = -1;
    int port = 0;              // requested port (0 = ephemeral) for Bind; dest port for Send
    int bound_port = 0;        // actual port assigned to socket after Bind
    std::string host;          // dest host for Send (empty for Bind)
    std::string data;          // payload to send (binary-safe, std::string used as bytes)
    std::string result_string; // received datagram (binary-safe)
    std::string from_ip;       // sender ip after Recv
    int from_port = 0;         // sender port after Recv
    int timeout_ms = -1;       // -1 = block forever; >=0 = poll with timeout for Recv
    std::string error_msg;

    UdpOperation(Type t) : IOOperation(t) {
    }
};

struct HttpOperation : IOOperation {
    std::string host;
    int port = 0;
    std::string method;
    std::string url_path;
    std::string full_url; // complete URL including scheme, for libcurl
    StringMap headers;
    std::string body;
    int status_code = 0;
    StringMap response_headers;
    std::string result_string;
    std::string error_msg;

    HttpOperation() : IOOperation(Type::HttpRequest) {
    }
};
#endif // !DISABLE_HTTP

using IOOperationPtr = std::shared_ptr<IOOperation>;
