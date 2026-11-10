#pragma once
// I/O operation structs (split out of io.h).
// Included by io.h; safe to include directly (brings StringMap via core_types.h).
#include "core_types.h"
#include "nari_platform.h"

#include <atomic>
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
        TcpConnect,
        UdpBind,
        UdpSend,
        UdpRecv,
        HttpRequest
#endif
    };

    Type type;
    int64_t timer_ms = 0;
    // Callbacks may borrow this operation: the completion queue and runtime
    // retain it until invocation returns. Capturing a shared_ptr here cycles.
    std::function<void()> callback;
    bool success = false;
    std::string error_msg;

    IOOperation(Type t) : type(t) {
    }
};

// file operation
struct NariFileOperation : IOOperation {
    std::string file_path;
    std::string file_content;

    // The operation type determines which result is used; no manual union
    // lifetime or second type tag is needed.
    std::string result_string;
    std::vector<std::string> result_entries;
    bool result_bool = false;

    NariFileOperation(Type t) : IOOperation(t) {
    }
};

#ifndef DISABLE_HTTP
// A pending operation keeps the native socket alive after script code closes
// its ID. Workers observe cancellation before retrying; the OS cannot reuse the
// descriptor underneath an operation that was already polling it.
struct NariSocket {
    nari_socket_t fd;
    std::atomic<bool> closed{ false };

    explicit NariSocket(nari_socket_t socket) : fd(socket) {
    }
    ~NariSocket() {
        NARI_CLOSE_SOCKET(fd);
    }
    NariSocket(const NariSocket &) = delete;
    NariSocket &operator=(const NariSocket &) = delete;
};
using NariSocketPtr = std::shared_ptr<NariSocket>;

struct TcpOperation : IOOperation {
    nari_socket_t socket_fd = NARI_INVALID_SOCKET;
    NariSocketPtr socket_owner;
    int port = 0;
    nari_socket_t client_fd = NARI_INVALID_SOCKET;
    std::string client_ip;
    int client_port = 0;
    std::string host;
    std::string data;
    std::string result_string;

    TcpOperation(Type t) : IOOperation(t) {
    }

    // Newly created sockets belong to the operation until its callback adopts
    // them. This also closes results whose callbacks are discarded on shutdown.
    ~TcpOperation() {
        if ((type == Type::TcpListen || type == Type::TcpConnect) && socket_fd != NARI_INVALID_SOCKET) {
            NARI_CLOSE_SOCKET(socket_fd);
        }
        if (type == Type::TcpAccept && client_fd != NARI_INVALID_SOCKET) {
            NARI_CLOSE_SOCKET(client_fd);
        }
    }
};

struct UdpOperation : IOOperation {
    nari_socket_t socket_fd = NARI_INVALID_SOCKET;
    NariSocketPtr socket_owner;
    int port = 0;              // requested port (0 = ephemeral) for Bind; dest port for Send
    int bound_port = 0;        // actual port assigned to socket after Bind
    std::string host;          // dest host for Send (empty for Bind)
    std::string data;          // payload to send (binary-safe, std::string used as bytes)
    std::string result_string; // received datagram (binary-safe)
    std::string from_ip;       // sender ip after Recv
    int from_port = 0;         // sender port after Recv
    int timeout_ms = -1;       // -1 = block forever; >=0 = poll with timeout for Recv

    UdpOperation(Type t) : IOOperation(t) {
    }

    ~UdpOperation() {
        if (type == Type::UdpBind && socket_fd != NARI_INVALID_SOCKET) {
            NARI_CLOSE_SOCKET(socket_fd);
        }
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
    int64_t max_response_bytes = -1;
    long timeout_ms = 0;
    bool follow_redirects = true;
    int status_code = 0;
    StringMap response_headers;
    std::string result_string;

    HttpOperation() : IOOperation(Type::HttpRequest) {
    }
};
#endif // !DISABLE_HTTP

using IOOperationPtr = std::shared_ptr<IOOperation>;
