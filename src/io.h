#pragma once

#include <stdint.h>
#include <cstdio>
#include <functional>
#include <map>
#include <queue>
#ifndef NO_THREADS
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#endif
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
#endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <io.h>
    typedef SSIZE_T ssize_t;
    #define close _close
    #define poll WSAPoll
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
#endif
#include <cstring>
#ifndef NO_OPENSSL
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#endif


#ifdef __EMSCRIPTEN__
#define NO_THREADS
#endif

// base I/O operation
// async work for the thread pool
struct IOOperation {
    enum class Type {
        Timer, FileRead, FileWrite, FileAppend, FileExists, FileDelete, ListDir,
        TcpListen, TcpAccept, TcpRead, TcpWrite, TcpClose, TcpConnect,
        HttpRequest
    };

    Type type;
    int64_t timer_ms = 0;
    std::function<void()> callback;
    bool success = false;
    bool completed = false;

    IOOperation(Type t) : type(t) {}
};

// file operation
struct FileOperation : IOOperation {
    std::string file_path;
    std::string file_content;

    struct Result {
        enum class Type { None, String, Array, Bool } type = Type::None;

        union {
            std::string str_value;
            std::vector<std::string> array_value;
            bool bool_value;
        };

        Result() : type(Type::None), bool_value(false) {}

        ~Result() {
            clear();
        }

        Result(const Result& other) : type(other.type) {
            copy_from(other);
        }

        Result& operator=(const Result& other) {
            if (this != &other) {
                clear();
                type = other.type;
                copy_from(other);
            }
            return *this;
        }

        Result(Result&& other) noexcept : type(other.type) {
            move_from(std::move(other));
        }

        Result& operator=(Result&& other) noexcept {
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

        void copy_from(const Result& other) {
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

        void move_from(Result&& other) {
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

        static Result from_string(const std::string& s) {
            Result r;
            r.type = Type::String;
            new (&r.str_value) std::string(s);
            return r;
        }

        static Result from_array(const std::vector<std::string>& arr) {
            Result r;
            r.type = Type::Array;
            new (&r.array_value) std::vector<std::string>(arr);
            return r;
        }

        static Result from_bool(bool b) {
            Result r;
            r.type = Type::Bool;
            r.bool_value = b;
            return r;
        }
    } result;

    std::string error_msg;
    bool result_bool = false;

    FileOperation(Type t) : IOOperation(t) {}
};

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

    TcpOperation(Type t) : IOOperation(t) {}
};

struct HttpOperation : IOOperation {
    std::string host;
    int port = 0;
    std::string method;
    std::string url_path;
    std::map<std::string, std::string> headers;
    std::string body;
    int status_code = 0;
    std::map<std::string, std::string> response_headers;
    std::string result_string;
    std::string error_msg;

    HttpOperation() : IOOperation(Type::HttpRequest) {}
};

using IOOperationPtr = std::shared_ptr<IOOperation>;

#ifdef NO_THREADS
// synchronous stub for emscripten, run operations immediately without any threading
// TODO: this is kinda bad, I mostly did this only to make the emscripten build work
// but it might just be better to compile the emscripten build with threading, or expand this out.
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
        if (!op) return;
        
        // Execute synchronously
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
                // these could work in emscripten's virtual FS
                // but for now, mark as not supported
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
        if (completed_queue.empty()) return nullptr;
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
    std::atomic<bool> stop{false};

    std::queue<IOOperationPtr> completed_queue;
    std::mutex completed_mutex;
    std::atomic<int> pending_count{0};

public:
    IOThreadPool(size_t num_threads = 4) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    IOOperationPtr op;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        cv.wait(lock, [this] { return stop || !work_queue.empty(); });
                        if (stop && work_queue.empty()) return;
                        op = work_queue.front();
                        work_queue.pop();
                    }

                    if (op) {
                        switch (op->type) {
                            case IOOperation::Type::Timer:
                                std::this_thread::sleep_for(std::chrono::milliseconds(op->timer_ms));
                                op->success = true;
                                break;

                            case IOOperation::Type::FileRead: {
                                auto file_op = std::static_pointer_cast<FileOperation>(op);
                                FILE* fp = fopen(file_op->file_path.c_str(), "rb");
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
                                    file_op->result = FileOperation::Result::from_string(content);
                                    file_op->success = true;
                                } else {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to open file: " + file_op->file_path;
                                }
                                break;
                            }

                            case IOOperation::Type::FileWrite: {
                                auto file_op = std::static_pointer_cast<FileOperation>(op);
                                FILE* fp = fopen(file_op->file_path.c_str(), "wb");
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
                                auto file_op = std::static_pointer_cast<FileOperation>(op);
                                FILE* fp = fopen(file_op->file_path.c_str(), "ab");
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
                                auto file_op = std::static_pointer_cast<FileOperation>(op);
                                file_op->result_bool = std::filesystem::exists(file_op->file_path);
                                file_op->success = true;
                                break;
                            }

                            case IOOperation::Type::FileDelete: {
                                auto file_op = std::static_pointer_cast<FileOperation>(op);
                                std::error_code ec;
                                file_op->result_bool = std::filesystem::remove(file_op->file_path, ec);
                                if (ec) {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to delete file: " + ec.message();
                                } else {
                                    file_op->success = true;
                                }
                                break;
                            }

                            case IOOperation::Type::ListDir: {
                                auto file_op = std::static_pointer_cast<FileOperation>(op);
                                std::error_code ec;

                                // Collect all filenames first
                                std::vector<std::string> filenames;
                                for (const auto& entry : std::filesystem::directory_iterator(file_op->file_path, ec)) {
                                    if (ec) break;
                                    filenames.push_back(entry.path().filename().string());
                                }

                                if (ec) {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to list directory: " + ec.message();
                                } else {
                                    // sort files, prioritize dot files/folders, and then case-insensitive alphabetical
                                    std::sort(filenames.begin(), filenames.end(), [](const std::string& a, const std::string& b) {
                                        bool a_dot = !a.empty() && a[0] == '.';
                                        bool b_dot = !b.empty() && b[0] == '.';
                                        if (a_dot != b_dot) return a_dot; // dot files first

                                        std::string a_lower = a;
                                        std::string b_lower = b;
                                        std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), ::tolower);
                                        std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), ::tolower);
                                        return a_lower < b_lower;
                                    });

                                    // return as array
                                    file_op->result = FileOperation::Result::from_array(filenames);
                                    file_op->success = true;
                                }
                                break;
                            }

                            case IOOperation::Type::TcpListen: {
                                auto tcp_op = std::static_pointer_cast<TcpOperation>(op);
                                int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
                                if (sock_fd < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to create socket: " + std::string(strerror(errno));
                                    break;
                                }

                                const char *opt = "1";
                                setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, opt, sizeof(opt));

                                struct sockaddr_in addr;
                                memset(&addr, 0, sizeof(addr));
                                addr.sin_family = AF_INET;
                                addr.sin_addr.s_addr = INADDR_ANY;
                                addr.sin_port = htons(tcp_op->port);

                                if (bind(sock_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to bind to port " + std::to_string(tcp_op->port) + ": " + std::string(strerror(errno));
                                    close(sock_fd);
                                    break;
                                }

                                if (listen(sock_fd, 128) < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to listen: " + std::string(strerror(errno));
                                    close(sock_fd);
                                    break;
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
                                    tcp_op->error_msg = stop ? "Shutdown requested" : "Poll failed: " + std::string(strerror(errno));
                                    break;
                                }

                                struct sockaddr_in client_addr;
                                socklen_t client_len = sizeof(client_addr);

                                int client_fd = accept(tcp_op->socket_fd, (struct sockaddr*)&client_addr, &client_len);
                                if (client_fd < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to accept connection: " + std::string(strerror(errno));
                                    break;
                                }

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
                                const char* data = tcp_op->data.c_str();
                                size_t total = tcp_op->data.size();
                                size_t sent = 0;

                                while (sent < total) {
                                    ssize_t n = send(tcp_op->socket_fd, data + sent, total - sent, 0);
                                    if (n < 0) {
                                        tcp_op->success = false;
                                        tcp_op->error_msg = "Failed to write to socket: " + std::string(strerror(errno));
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
                                    close(tcp_op->socket_fd);
                                    tcp_op->success = true;
                                } else {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Invalid socket";
                                }
                                break;
                            }

                            case IOOperation::Type::TcpConnect: {
                                auto tcp_op = std::static_pointer_cast<TcpOperation>(op);
                                int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
                                if (sock_fd < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to create socket: " + std::string(strerror(errno));
                                    break;
                                }

                                struct hostent* server = gethostbyname(tcp_op->host.c_str());
                                if (server == nullptr) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to resolve host: " + tcp_op->host;
                                    close(sock_fd);
                                    break;
                                }

                                struct sockaddr_in addr;
                                memset(&addr, 0, sizeof(addr));
                                addr.sin_family = AF_INET;
                                memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
                                addr.sin_port = htons(tcp_op->port);

                                if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg = "Failed to connect to " + tcp_op->host + ":" + std::to_string(tcp_op->port) + ": " + std::string(strerror(errno));
                                    close(sock_fd);
                                    break;
                                }

                                tcp_op->socket_fd = sock_fd;
                                tcp_op->success = true;
                                break;
                            }

                            case IOOperation::Type::HttpRequest: {
                                auto http_op = std::static_pointer_cast<HttpOperation>(op);
                                try {
                                    std::string host = http_op->host;
                                    int port = http_op->port;
                                    bool is_https = false;

                                    // probably not super efficient to do this, but meh :p
                                    if (host.find("https://") == 0) {
                                        host = host.substr(8);
                                        is_https = true;
                                    } else if (host.find("http://") == 0) {
                                        host = host.substr(7);
                                    }

                                    size_t slash_pos = host.find('/');
                                    if (slash_pos != std::string::npos) {
                                        host = host.substr(0, slash_pos);
                                    }

                                    httplib::Headers headers;
                                    headers.emplace("User-Agent", "Nari-HTTP/1.0");
                                    for (const auto& [key, value] : http_op->headers) {
                                        headers.emplace(key, value);
                                    }

                                    httplib::Result res;


                                    if (is_https) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
                                        httplib::SSLClient client(host, port);
                                        client.set_follow_location(true);

                                        // TODO: This should be configurable, unfortunately for windows
                                        // it's not as easy as pointing to /etc/ssl/certs like on linux
                                        // so for now we just enable verification on linux only
                                        #ifdef _WIN32
                                            client.enable_server_certificate_verification(false);
                                        #elif __linux__
                                            client.enable_server_certificate_verification(true);
                                            client.set_ca_cert_path("/etc/ssl/certs");
                                        #endif
                                        
                                        

                                        if (http_op->method == "GET") {
                                            res = client.Get(http_op->url_path, headers);
                                        } else if (http_op->method == "POST") {
                                            res = client.Post(http_op->url_path, headers, http_op->body, "application/octet-stream");
                                        } else {
                                            httplib::Request req;
                                            req.method = http_op->method;
                                            req.path = http_op->url_path;
                                            req.headers = headers;
                                            req.body = http_op->body;
                                            res = client.send(req);
                                        }
#else
                                        http_op->success = false;
                                        http_op->error_msg = "HTTPS support not compiled in.";
                                        break;
#endif
                                    } else {
                                        httplib::Client client(host, port);
                                        client.set_follow_location(true);

                                        if (http_op->method == "GET") {
                                            res = client.Get(http_op->url_path, headers);
                                        } else if (http_op->method == "POST") {
                                            res = client.Post(http_op->url_path, headers, http_op->body, "application/octet-stream");
                                        } else {
                                            httplib::Request req;
                                            req.method = http_op->method;
                                            req.path = http_op->url_path;
                                            req.headers = headers;
                                            req.body = http_op->body;
                                            res = client.send(req);
                                        }
                                    }

                                    if (res && res->status != -1) {
                                        http_op->status_code = res->status;
                                        http_op->result_string = res->body;

                                        for (const auto& [key, value] : res->headers) {
                                            http_op->response_headers[key] = value;
                                        }

                                        http_op->success = true;
                                    } else {
                                        http_op->success = false;
                                        auto err = res.error();
                                        http_op->error_msg = "HTTP request failed: " + httplib::to_string(err);
                                    }
                                } catch (const std::exception& e) {
                                    http_op->success = false;
                                    http_op->error_msg = "HTTP request exception: " + std::string(e.what());
                                }
                                break;
                            }
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
    }

    void shutdown() {
        // already stopped?
        bool expected = false;
        if (!stop.compare_exchange_strong(expected, true)) {
            return;
        }
        cv.notify_all();
        for (auto& worker : workers) {
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
        if (completed_queue.empty()) return nullptr;
        IOOperationPtr op = completed_queue.front();
        completed_queue.pop();
        return op;
    }
};
#endif // NO_THREADS
