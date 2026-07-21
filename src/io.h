#pragma once

#include "nari_fs.h"

#include <cstdio>
#include <functional>
#include <map>
#include <queue>
#include <stdint.h>
// MCU targets have no pthreads, so we force NO_THREADS so the synchronous stub IOThreadPool is compiled instead.
#ifdef NARI_MCU
#ifndef NO_THREADS
#define NO_THREADS
#endif
#endif

#ifndef NO_THREADS
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

typedef std::map<std::string, std::string> StringMap;

// Under NO_THREADS (Emscripten / NARI_MCU) there is no std::this_thread.
// NARI_MCU bare-metal targets should override these with the SDK's own function for delaying.
#ifdef NO_THREADS
#ifdef NARI_MCU
#ifdef NARI_ESP_IDF
// vTaskDelay for ms sleep; esp_rom_delay_us for us.
// These headers are on the compiler include path when building with idf.py.
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifndef NARI_SLEEP_MILLIS
#define NARI_SLEEP_MILLIS(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#endif
#ifndef NARI_SLEEP_MICROS
#define NARI_SLEEP_MICROS(us) esp_rom_delay_us((uint32_t)(us))
#endif
#else
// Generic bare-metal MCU: replace NARI_SLEEP_MILLIS / NARI_SLEEP_MICROS
// in your platform glue layer before including this header, or accept a
// no-op (async I/O paths are never reached in bytecode-only builds).
#warning                                                                                                               \
    "No sleep implementation defined for this MCU platform. Timers will not work! Define NARI_SLEEP_MILLIS and NARI_SLEEP_MICROS to appropriate functions for your platform."
#ifndef NARI_SLEEP_MILLIS
#define NARI_SLEEP_MILLIS(ms) ((void)(ms))
#endif
#ifndef NARI_SLEEP_MICROS
#define NARI_SLEEP_MICROS(us) ((void)(us))
#endif
#endif // NARI_ESP_IDF
#else
// use POSIX nanosleep for emscripten or other NO_THREADS desktop builds
#include <time.h>
#ifndef NARI_SLEEP_MILLIS
#define NARI_SLEEP_MILLIS(ms)                                                                                          \
    do {                                                                                                               \
        struct timespec _ts{ 0, (long)(ms) * 1000000L };                                                               \
        nanosleep(&_ts, nullptr);                                                                                      \
    } while (0)
#endif
#ifndef NARI_SLEEP_MICROS
#define NARI_SLEEP_MICROS(us)                                                                                          \
    do {                                                                                                               \
        struct timespec _ts{ 0, (long)(us) * 1000L };                                                                  \
        nanosleep(&_ts, nullptr);                                                                                      \
    } while (0)
#endif
#endif
#else
namespace chrono = std::chrono;
#define NARI_SLEEP_MILLIS(ms) std::this_thread::sleep_for(chrono::milliseconds(ms))
#define NARI_SLEEP_MICROS(us) std::this_thread::sleep_for(chrono::microseconds(us))
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <corecrt_io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SSIZE_T ssize_t;
// Prevent any other header from redefining ssize_t with a conflicting type.
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#endif
#define close _close
#define poll WSAPoll
#define NARI_CLOSE_SOCKET(fd) ::closesocket((SOCKET)fd)
using nari_socket_t = SOCKET;
#define NARI_INVALID_SOCKET INVALID_SOCKET
#elif defined(NARI_MCU)
// MCU means no POSIX networking. Socket/poll headers are provided by
// the platform SDK (e.g. ESP-IDF lwIP) if networking is ever re-enabled
// however networking is currently not supported on MCU targets.
#include <unistd.h>
#define NARI_CLOSE_SOCKET(fd) ::close(fd)
using nari_socket_t = int;
#define NARI_INVALID_SOCKET (-1)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
// On POSIX sockets are ordinary file descriptors
#define NARI_CLOSE_SOCKET(fd) ::close(fd)
using nari_socket_t = int;
#define NARI_INVALID_SOCKET (-1)
#endif
#include <cstring>
#ifndef DISABLE_HTTP
#include <curl/curl.h>
#endif

#ifdef __EMSCRIPTEN__
#define NO_THREADS
#endif

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
        enum class Type { None, String, Array, Bool } type = Type::None;

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

#ifdef NO_THREADS
// synchronous stub for emscripten, run operations immediately without any threading
// TODO: (!!) this is... bad, I mostly did this only to make the emscripten build
// work but it might just be better to compile the emscripten build with
// threading, or expand this out.
class IOThreadPool {
  private:
    std::queue<IOOperationPtr> completed_queue;

  public:
    IOThreadPool(size_t num_threads = 0) {
        // No threads created
        (void)num_threads;
    }

    ~IOThreadPool() = default;

    void shutdown() {
        // Nothing to shutdown
    }

    void submit(IOOperationPtr op) {
        if (!op) {
            return;
        }

        // execute synchronously
        switch (op->type) {
            case IOOperation::Type::Timer:
                // skip timers in emscripten
                fprintf(stderr, "Warning: Timer ops are not supported in this build and will be ignored!\n");
                op->success = false;
                break;

            case IOOperation::Type::FileRead:
            case IOOperation::Type::FileWrite:
            case IOOperation::Type::FileAppend:
            case IOOperation::Type::FileExists:
            case IOOperation::Type::FileDelete:
            case IOOperation::Type::ListDir:
                // these could work in emscripten's virtual FS, but for now, mark as not supported
                op->success = false;
                fprintf(stderr, "Warning: File I/O ops are not supported in this build and will be ignored!\n");
                break;

            default:
                op->success = false;
                break;
        }

        op->completed = true;
        completed_queue.push(op);
    }

    bool has_completed() {
        return !completed_queue.empty();
    }

    bool has_pending() {
        return false; // Everything runs immediately
    }

    IOOperationPtr pop_completed() {
        if (completed_queue.empty()) {
            return nullptr;
        }
        IOOperationPtr op = completed_queue.front();
        completed_queue.pop();
        return op;
    }
};

#else
// thread pool for I/O
class IOThreadPool {
  private:
    std::vector<std::thread> workers;
    std::queue<IOOperationPtr> work_queue;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{ false };

    std::queue<IOOperationPtr> completed_queue;
    std::mutex completed_mutex;
    std::atomic<int> pending_count{ 0 };

#ifndef DISABLE_HTTP
    // shared curl handle to allow SSL sessions and DNS results to be reused across easy handles
    CURLSH *curl_share = nullptr;
    std::mutex curl_share_lock_dns;
    std::mutex curl_share_lock_ssl;

    static void curl_share_lock(CURL *, curl_lock_data data, curl_lock_access, void *userp) {
        auto *pool = (IOThreadPool *)userp;
        if (data == CURL_LOCK_DATA_DNS) {
            pool->curl_share_lock_dns.lock();
        } else if (data == CURL_LOCK_DATA_SSL_SESSION) {
            pool->curl_share_lock_ssl.lock();
        }
    }

    static void curl_share_unlock(CURL *, curl_lock_data data, void *userp) {
        auto *pool = (IOThreadPool *)userp;
        if (data == CURL_LOCK_DATA_DNS) {
            pool->curl_share_lock_dns.unlock();
        } else if (data == CURL_LOCK_DATA_SSL_SESSION) {
            pool->curl_share_lock_ssl.unlock();
        }
    }
#endif // !DISABLE_HTTP

  public:
    IOThreadPool(size_t num_threads = 4) {
#if defined(_WIN32) && !defined(DISABLE_HTTP)
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
#ifndef DISABLE_HTTP
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_share = curl_share_init();
        // Share SSL sessions so subsequent requests to the same host get TLS
        // session resumption instead of a full (CA-bundle-loading) handshake.
        curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        // share DNS results to avoid repeated resolver round-trips.
        curl_share_setopt(curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(curl_share, CURLSHOPT_LOCKFUNC, &IOThreadPool::curl_share_lock);
        curl_share_setopt(curl_share, CURLSHOPT_UNLOCKFUNC, &IOThreadPool::curl_share_unlock);
        curl_share_setopt(curl_share, CURLSHOPT_USERDATA, this);
#endif
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    IOOperationPtr op;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        cv.wait(lock, [this] { return stop || !work_queue.empty(); });
                        if (stop && work_queue.empty()) {
                            return;
                        }
                        op = work_queue.front();
                        work_queue.pop();
                    }

                    if (op) {
                        switch (op->type) {
                            case IOOperation::Type::Timer: {
                                std::this_thread::sleep_for(chrono::milliseconds(op->timer_ms));
                                op->success = true;
                                break;
                            }

                            case IOOperation::Type::FileRead: {
                                auto file_op = std::static_pointer_cast<NariFileOperation>(op);
                                FILE *fp = fopen(file_op->file_path.c_str(), "rb");
                                if (fp) {
                                    fseek(fp, 0, SEEK_END);
                                    long file_size = ftell(fp);
                                    fseek(fp, 0, SEEK_SET);
                                    std::string content;
                                    if (file_size > 0) {
                                        content.resize(file_size);
                                        size_t bytes_read = fread(&content[0], 1, file_size, fp);
                                        content.resize(bytes_read);
                                    }
                                    fclose(fp);
                                    file_op->result = NariFileOperation::Result::from_string(content);
                                    file_op->success = true;
                                } else {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to open file: " + file_op->file_path;
                                }
                                break;
                            }

                            case IOOperation::Type::FileWrite: {
                                auto file_op = std::static_pointer_cast<NariFileOperation>(op);
                                FILE *fp = fopen(file_op->file_path.c_str(), "wb");
                                if (fp) {
                                    fwrite(file_op->file_content.data(), 1, file_op->file_content.size(), fp);
                                    fclose(fp);
                                    file_op->success = true;
                                } else {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to write file: " + file_op->file_path;
                                }
                                break;
                            }

                            case IOOperation::Type::FileAppend: {
                                auto file_op = std::static_pointer_cast<NariFileOperation>(op);
                                FILE *fp = fopen(file_op->file_path.c_str(), "ab");
                                if (fp) {
                                    fwrite(file_op->file_content.data(), 1, file_op->file_content.size(), fp);
                                    fclose(fp);
                                    file_op->success = true;
                                } else {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to append to file: " + file_op->file_path;
                                }
                                break;
                            }

                            case IOOperation::Type::FileExists: {
                                auto file_op = std::static_pointer_cast<NariFileOperation>(op);
                                file_op->result_bool = nari::fs::exists(nari::fs::Path(file_op->file_path));
                                file_op->success = true;
                                break;
                            }

                            case IOOperation::Type::FileDelete: {
                                auto file_op = std::static_pointer_cast<NariFileOperation>(op);
                                std::error_code err;
                                file_op->result_bool = nari::fs::remove(nari::fs::Path(file_op->file_path), err);
                                if (err) {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to delete file: " + err.message();
                                } else {
                                    file_op->success = true;
                                }
                                break;
                            }

                            case IOOperation::Type::ListDir: {
                                auto file_op = std::static_pointer_cast<NariFileOperation>(op);
                                std::error_code err;

                                // Collect all filenames first
                                std::vector<std::string> filenames;
                                for (const auto &entry :
                                     nari::fs::list_directory(nari::fs::Path(file_op->file_path), err)) {
                                    filenames.push_back(entry.filename().string());
                                }

                                if (err) {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to list directory: " + err.message();
                                } else {
                                    // sort files, prioritize dot files/folders, and then case-insensitive alphabetical
                                    std::sort(
                                        filenames.begin(), filenames.end(),
                                        [](const std::string &a, const std::string &b) {
                                            bool a_dot = !a.empty() && a[0] == '.';
                                            bool b_dot = !b.empty() && b[0] == '.';
                                            if (a_dot != b_dot) {
                                                return a_dot; // dot files first
                                            }

                                            std::string a_lower = a;
                                            std::string b_lower = b;
                                            std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), ::tolower);
                                            std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), ::tolower);
                                            return a_lower < b_lower;
                                        });

                                    file_op->result = NariFileOperation::Result::from_array(filenames);
                                    file_op->success = true;
                                }
                                break;
                            }

#ifndef DISABLE_HTTP
                            case IOOperation::Type::TcpListen: {
                                auto tcp_op = std::static_pointer_cast<TcpOperation>(op);
                                nari_socket_t raw_fd = socket(AF_INET, SOCK_STREAM, 0);
                                if (raw_fd == NARI_INVALID_SOCKET) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to create socket: " + std::string(strerror(errno));
                                    break;
                                }
                                int sock_fd = raw_fd;

                                int opt = 1;
                                setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

                                struct sockaddr_in addr;
                                memset(&addr, 0, sizeof(addr));
                                addr.sin_family = AF_INET;
                                addr.sin_addr.s_addr = INADDR_ANY;
                                addr.sin_port = htons(tcp_op->port);

                                if (bind(sock_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to bind to port " + std::to_string(tcp_op->port) +
                                                        ": " + std::string(strerror(errno));

                                    NARI_CLOSE_SOCKET(sock_fd);
                                    break;
                                }

                                if (listen(sock_fd, 128) < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to listen: " + std::string(strerror(errno));
                                    NARI_CLOSE_SOCKET(sock_fd);
                                    break;
                                }

                                // If the caller asked for an ephemeral port (port == 0),
                                // read back the kernel-assigned one so script code can use it.
                                if (tcp_op->port == 0) {
                                    struct sockaddr_in bound_addr;
                                    socklen_t bound_len = sizeof(bound_addr);
                                    if (getsockname(sock_fd, (sockaddr *)&bound_addr, &bound_len) == 0) {
                                        tcp_op->port = ntohs(bound_addr.sin_port);
                                    }
                                }

                                tcp_op->socket_fd = sock_fd;
                                tcp_op->success = true;
                                break;
                            }

                            case IOOperation::Type::TcpAccept: {
                                auto tcp_op = std::static_pointer_cast<TcpOperation>(op);

                                struct pollfd pfd;
                                pfd.fd = tcp_op->socket_fd;
                                pfd.events = POLLIN;

                                int poll_result;
                                do {
                                    if (stop) {
                                        tcp_op->success = false;
                                        tcp_op->error_msg = "Shutdown requested";
                                        break;
                                    }
                                    poll_result = poll(&pfd, 1, 100); // 100ms timeout
                                } while (poll_result == 0); // timeout

                                if (stop || poll_result < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg =
                                        stop ? "Shutdown requested" : "Poll failed: " + std::string(strerror(errno));
                                    break;
                                }

                                struct sockaddr_in client_addr;
                                socklen_t client_len = sizeof(client_addr);

                                nari_socket_t raw_client_fd =
                                    accept(tcp_op->socket_fd, (struct sockaddr *)&client_addr, &client_len);
                                if (raw_client_fd == NARI_INVALID_SOCKET) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to accept connection: " + std::string(strerror(errno));
                                    break;
                                }
                                int client_fd = raw_client_fd;

                                tcp_op->client_fd = client_fd;
                                tcp_op->client_ip = inet_ntoa(client_addr.sin_addr);
                                tcp_op->client_port = ntohs(client_addr.sin_port);
                                tcp_op->success = true;
                                break;
                            }

                            case IOOperation::Type::TcpRead: {
                                auto tcp_op = std::static_pointer_cast<TcpOperation>(op);
                                char buffer[8192];
                                ssize_t bytes_read = recv(tcp_op->socket_fd, buffer, sizeof(buffer) - 1, 0);

                                if (bytes_read < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to read from socket: " + std::string(strerror(errno));
                                } else if (bytes_read == 0) {
                                    tcp_op->success = true;
                                    tcp_op->result_string = "";
                                } else {
                                    buffer[bytes_read] = '\0';
                                    tcp_op->result_string = std::string(buffer, bytes_read);
                                    tcp_op->success = true;
                                }
                                break;
                            }

                            case IOOperation::Type::TcpWrite: {
                                auto tcp_op = std::static_pointer_cast<TcpOperation>(op);
                                const char *data = tcp_op->data.c_str();
                                size_t total = tcp_op->data.size();
                                size_t sent = 0;

                                while (sent < total) {
                                    ssize_t n = send(tcp_op->socket_fd, data + sent, total - sent, 0);
                                    if (n < 0) {
                                        tcp_op->success = false;
                                        tcp_op->error_msg =
                                            "Failed to write to socket: " + std::string(strerror(errno));
                                        break;
                                    }
                                    sent += n;
                                }

                                if (sent == total) {
                                    tcp_op->success = true;
                                }
                                break;
                            }

                            case IOOperation::Type::TcpClose: {
                                auto tcp_op = std::static_pointer_cast<TcpOperation>(op);
                                if (tcp_op->socket_fd >= 0) {
                                    NARI_CLOSE_SOCKET(tcp_op->socket_fd);
                                    tcp_op->success = true;
                                } else {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Invalid socket";
                                }
                                break;
                            }

                            case IOOperation::Type::TcpConnect: {
                                auto tcp_op = std::static_pointer_cast<TcpOperation>(op);
                                nari_socket_t raw_fd = socket(AF_INET, SOCK_STREAM, 0);
                                if (raw_fd == NARI_INVALID_SOCKET) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to create socket: " + std::string(strerror(errno));
                                    break;
                                }
                                int sock_fd = raw_fd;

                                struct hostent *server = gethostbyname(tcp_op->host.c_str());
                                if (server == nullptr) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to resolve host: " + tcp_op->host;
                                    NARI_CLOSE_SOCKET(sock_fd);
                                    break;
                                }

                                struct sockaddr_in addr;
                                memset(&addr, 0, sizeof(addr));
                                addr.sin_family = AF_INET;
                                memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
                                addr.sin_port = htons(tcp_op->port);

                                if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to connect to " + tcp_op->host + ":" +
                                                        std::to_string(tcp_op->port) + ": " +
                                                        std::string(strerror(errno));

                                    NARI_CLOSE_SOCKET(sock_fd);
                                    break;
                                }

                                tcp_op->socket_fd = sock_fd;
                                tcp_op->success = true;
                                break;
                            }

                            case IOOperation::Type::UdpBind: {
                                auto udp_op = std::static_pointer_cast<UdpOperation>(op);
                                nari_socket_t raw_fd = socket(AF_INET, SOCK_DGRAM, 0);
                                if (raw_fd == NARI_INVALID_SOCKET) {
                                    udp_op->success = false;
                                    udp_op->error_msg = "Failed to create UDP socket: " + std::string(strerror(errno));
                                    break;
                                }
                                int sock_fd = raw_fd;

                                struct sockaddr_in addr;
                                memset(&addr, 0, sizeof(addr));
                                addr.sin_family = AF_INET;
                                addr.sin_addr.s_addr = INADDR_ANY;
                                addr.sin_port = htons(udp_op->port); // 0 = kernel-assigned ephemeral port

                                if (bind(sock_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
                                    udp_op->success = false;
                                    udp_op->error_msg = "Failed to bind UDP socket to port " +
                                                        std::to_string(udp_op->port) + ": " +
                                                        std::string(strerror(errno));
                                    NARI_CLOSE_SOCKET(sock_fd);
                                    break;
                                }

                                // Read back the actual bound port (matters when port==0)
                                struct sockaddr_in bound_addr;
                                socklen_t bound_len = sizeof(bound_addr);
                                if (getsockname(sock_fd, (sockaddr *)&bound_addr, &bound_len) == 0) {
                                    udp_op->bound_port = ntohs(bound_addr.sin_port);
                                } else {
                                    udp_op->bound_port = udp_op->port;
                                }

                                udp_op->socket_fd = sock_fd;
                                udp_op->success = true;
                                break;
                            }

                            case IOOperation::Type::UdpSend: {
                                auto udp_op = std::static_pointer_cast<UdpOperation>(op);

                                struct hostent *server = gethostbyname(udp_op->host.c_str());
                                if (server == nullptr) {
                                    udp_op->success = false;
                                    udp_op->error_msg = "Failed to resolve host: " + udp_op->host;
                                    break;
                                }

                                struct sockaddr_in addr;
                                memset(&addr, 0, sizeof(addr));
                                addr.sin_family = AF_INET;
                                memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
                                addr.sin_port = htons(udp_op->port);

                                ssize_t sent = sendto(udp_op->socket_fd, udp_op->data.data(), udp_op->data.size(), 0,
                                                      (sockaddr *)&addr, sizeof(addr));
                                if (sent < 0) {
                                    udp_op->success = false;
                                    udp_op->error_msg = "Failed to send UDP datagram: " + std::string(strerror(errno));
                                } else {
                                    udp_op->success = true;
                                }
                                break;
                            }

                            case IOOperation::Type::UdpRecv: {
                                auto udp_op = std::static_pointer_cast<UdpOperation>(op);

                                // optionally poll with timeout so a hung peer doesn't pin a worker forever.
                                // always poll in 100ms ticks so shutdown is responsive.
                                if (udp_op->timeout_ms >= 0 || true) {
                                    struct pollfd pfd;
                                    pfd.fd = udp_op->socket_fd;
                                    pfd.events = POLLIN;
                                    int total_waited = 0;
                                    int poll_result = 0;
                                    while (true) {
                                        if (stop) {
                                            udp_op->success = false;
                                            udp_op->error_msg = "Shutdown requested";
                                            break;
                                        }
                                        poll_result = poll(&pfd, 1, 100);
                                        if (poll_result > 0) {
                                            break; // data available
                                        }
                                        if (poll_result < 0) {
                                            udp_op->success = false;
                                            udp_op->error_msg = "Poll failed: " + std::string(strerror(errno));
                                            break;
                                        }
                                        // poll_result == 0 -> timeout tick
                                        if (udp_op->timeout_ms >= 0) {
                                            total_waited += 100;
                                            if (total_waited >= udp_op->timeout_ms) {
                                                udp_op->success = false;
                                                udp_op->error_msg = "Recv timeout";
                                                break;
                                            }
                                        }
                                    }
                                    if (!udp_op->error_msg.empty() || stop) {
                                        break;
                                    }
                                }

                                char buffer[65536]; // max UDP datagram payload
                                struct sockaddr_in from_addr;
                                socklen_t from_len = sizeof(from_addr);
                                ssize_t bytes = recvfrom(udp_op->socket_fd, buffer, sizeof(buffer), 0,
                                                         (sockaddr *)&from_addr, &from_len);
                                if (bytes < 0) {
                                    udp_op->success = false;
                                    udp_op->error_msg = "Failed to recv UDP datagram: " + std::string(strerror(errno));
                                } else {
                                    udp_op->result_string.assign(buffer, bytes);
                                    udp_op->from_ip = inet_ntoa(from_addr.sin_addr);
                                    udp_op->from_port = ntohs(from_addr.sin_port);
                                    udp_op->success = true;
                                }
                                break;
                            }

                            case IOOperation::Type::UdpClose: {
                                auto udp_op = std::static_pointer_cast<UdpOperation>(op);
                                if (udp_op->socket_fd >= 0) {
                                    NARI_CLOSE_SOCKET(udp_op->socket_fd);
                                    udp_op->success = true;
                                } else {
                                    udp_op->success = false;
                                    udp_op->error_msg = "Invalid UDP socket";
                                }
                                break;
                            }

                            case IOOperation::Type::HttpRequest: {
                                auto http_op = std::static_pointer_cast<HttpOperation>(op);
                                try {
                                    CURL *curl = curl_easy_init();
                                    if (!curl) {
                                        http_op->success = false;
                                        http_op->error_msg = "curl_easy_init() failed";
                                        break;
                                    }

                                    if (curl_share) {
                                        curl_easy_setopt(curl, CURLOPT_SHARE, curl_share);
                                    }

                                    // Force HTTP/1.1 -- HTTP/2 (nghttp2) keeps per-session HPACK
                                    // tables alive on the heap between requests and adds overhead.
                                    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

                                    curl_easy_setopt(curl, CURLOPT_URL, http_op->full_url.c_str());
                                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

                                    // Defense in depth: explicitly require TLS peer + host
                                    // verification rather than relying on libcurl defaults.
                                    // These match libcurl's defaults but pin them so a
                                    // future compile-time flag change can't silently
                                    // disable verification.
                                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

                                    // Collect response body
                                    curl_easy_setopt(
                                        curl, CURLOPT_WRITEFUNCTION,
                                        +[](char *ptr, size_t size, size_t nmemb, void *ud) {
                                            ((std::string *)ud)->append(ptr, size * nmemb);
                                            return size * nmemb;
                                        });
                                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &http_op->result_string);

                                    // Collect response headers
                                    curl_easy_setopt(
                                        curl, CURLOPT_HEADERFUNCTION,
                                        +[](char *ptr, size_t size, size_t nmemb, void *ud) {
                                            std::string hdr(ptr, size * nmemb);
                                            while (!hdr.empty() && (hdr.back() == '\r' || hdr.back() == '\n')) {
                                                hdr.pop_back();
                                            }
                                            size_t colon = hdr.find(':');
                                            if (colon != std::string::npos) {
                                                std::string key = hdr.substr(0, colon);
                                                std::string val = hdr.substr(colon + 1);
                                                while (!val.empty() && val.front() == ' ') {
                                                    val.erase(0, 1);
                                                }
                                                (*(StringMap *)(ud))[key] = val;
                                            }
                                            return size * nmemb;
                                        });

                                    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &http_op->response_headers);
                                    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nari-HTTP/1.0");

                                    // custom request headers
                                    curl_slist *hdrs = nullptr;
                                    for (const auto &[k, v] : http_op->headers) {
                                        hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
                                    }
                                    if (hdrs) {
                                        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
                                    }

                                    // method + optional body
                                    if (http_op->method == "POST") {
                                        curl_easy_setopt(curl, CURLOPT_POST, 1L);
                                        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, http_op->body.c_str());
                                        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)http_op->body.size());
                                    } else if (http_op->method != "GET") {
                                        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, http_op->method.c_str());
                                        if (!http_op->body.empty()) {
                                            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, http_op->body.c_str());
                                            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)http_op->body.size());
                                        }
                                    }

                                    CURLcode result = curl_easy_perform(curl);
                                    if (result == CURLE_OK) {
                                        long status = 0;
                                        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
                                        http_op->status_code = status;
                                        http_op->success = true;
                                    } else {
                                        http_op->success = false;
                                        http_op->error_msg =
                                            "HTTP request failed: " + std::string(curl_easy_strerror(result));
                                    }

                                    if (hdrs) {
                                        curl_slist_free_all(hdrs);
                                    }
                                    curl_easy_cleanup(curl);
                                } catch (const std::exception &e) {
                                    http_op->success = false;
                                    http_op->error_msg = "HTTP request exception: " + std::string(e.what());
                                }
                                break;
                            }
#endif // !DISABLE_HTTP
                        }

                        op->completed = true;
                        {
                            std::lock_guard<std::mutex> lock(completed_mutex);
                            completed_queue.push(op);
                        }
                        pending_count--;
                    }
                }
            });
        }
    }

    ~IOThreadPool() {
        shutdown();
#ifndef DISABLE_HTTP
        if (curl_share) {
            curl_share_cleanup(curl_share);
        }
        curl_global_cleanup();
#endif
#if defined(_WIN32) && !defined(DISABLE_HTTP)
        WSACleanup();
#endif
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            bool expected = false;
            if (!stop.compare_exchange_strong(expected, true)) {
                return; // already stopped
            }
        }
        cv.notify_all();
        for (auto &worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void submit(IOOperationPtr op) {
        pending_count++;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            work_queue.push(op);
        }
        cv.notify_one();
    }

    bool has_completed() {
        std::lock_guard<std::mutex> lock(completed_mutex);
        return !completed_queue.empty();
    }

    bool has_pending() {
        return pending_count > 0 || has_completed();
    }

    IOOperationPtr pop_completed() {
        std::lock_guard<std::mutex> lock(completed_mutex);
        if (completed_queue.empty()) {
            return nullptr;
        }
        IOOperationPtr op = completed_queue.front();
        completed_queue.pop();
        return op;
    }
};
#endif // NO_THREADS
