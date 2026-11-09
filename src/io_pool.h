#pragma once
// IO thread pool (split out of io.h).
// Threading is selected by io.h before this file: NARI_MCU and
// __EMSCRIPTEN__ force NO_THREADS there. Include io.h, not this file.
#include "io_ops.h"
#include "nari_fs.h"
#include "nari_platform.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#ifndef NO_THREADS
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif
#ifndef DISABLE_HTTP
#include <curl/curl.h>
#endif

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
        auto *pool = static_cast<IOThreadPool *>(userp);
        if (data == CURL_LOCK_DATA_DNS) {
            pool->curl_share_lock_dns.lock();
        } else if (data == CURL_LOCK_DATA_SSL_SESSION) {
            pool->curl_share_lock_ssl.lock();
        }
    }

    static void curl_share_unlock(CURL *, curl_lock_data data, void *userp) {
        auto *pool = static_cast<IOThreadPool *>(userp);
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
                                for (const auto &entry : nari::fs::list_directory(nari::fs::Path(file_op->file_path), err)) {
                                    filenames.push_back(entry.filename().string());
                                }

                                if (err) {
                                    file_op->success = false;
                                    file_op->error_msg = "Failed to list directory: " + err.message();
                                } else {
                                    // sort files, prioritize dot files/folders, and then case-insensitive alphabetical
                                    std::sort(filenames.begin(), filenames.end(), [](const std::string &a, const std::string &b) {
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
                                setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));

                                struct sockaddr_in addr;
                                memset(&addr, 0, sizeof(addr));
                                addr.sin_family = AF_INET;
                                addr.sin_addr.s_addr = INADDR_ANY;
                                addr.sin_port = htons(tcp_op->port);

                                if (bind(sock_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
                                    tcp_op->success = false;
                                    tcp_op->error_msg =
                                        "Failed to bind to port " + std::to_string(tcp_op->port) + ": " + std::string(strerror(errno));

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
                                    if (getsockname(sock_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_len) == 0) {
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
                                    tcp_op->error_msg = stop ? "Shutdown requested" : "Poll failed: " + std::string(strerror(errno));
                                    break;
                                }

                                struct sockaddr_in client_addr;
                                socklen_t client_len = sizeof(client_addr);

                                nari_socket_t raw_client_fd = accept(tcp_op->socket_fd, (struct sockaddr *)&client_addr, &client_len);
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
                                    tcp_op->error_msg = "Failed to connect to " + tcp_op->host + ":" + std::to_string(tcp_op->port) + ": " +
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

                                if (bind(sock_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
                                    udp_op->success = false;
                                    udp_op->error_msg = "Failed to bind UDP socket to port " + std::to_string(udp_op->port) + ": " +
                                                        std::string(strerror(errno));
                                    NARI_CLOSE_SOCKET(sock_fd);
                                    break;
                                }

                                // Read back the actual bound port (matters when port==0)
                                struct sockaddr_in bound_addr;
                                socklen_t bound_len = sizeof(bound_addr);
                                if (getsockname(sock_fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_len) == 0) {
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

                                ssize_t sent = sendto(
                                    udp_op->socket_fd, udp_op->data.data(), udp_op->data.size(), 0, reinterpret_cast<sockaddr *>(&addr),
                                    sizeof(addr)
                                );
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
                                ssize_t bytes = recvfrom(
                                    udp_op->socket_fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&from_addr), &from_len
                                );
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
                                    curl, CURLOPT_WRITEFUNCTION, +[](char *ptr, size_t size, size_t nmemb, void *ud) {
                                        (static_cast<std::string *>(ud))->append(ptr, size * nmemb);
                                        return size * nmemb;
                                    }
                                );
                                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &http_op->result_string);

                                // Collect response headers
                                curl_easy_setopt(
                                    curl, CURLOPT_HEADERFUNCTION, +[](char *ptr, size_t size, size_t nmemb, void *ud) {
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
                                            (*(static_cast<StringMap *>(ud)))[key] = val;
                                        }
                                        return size * nmemb;
                                    }
                                );

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
                                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(http_op->body.size()));
                                } else if (http_op->method != "GET") {
                                    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, http_op->method.c_str());
                                    if (!http_op->body.empty()) {
                                        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, http_op->body.c_str());
                                        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(http_op->body.size()));
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
                                    http_op->error_msg = "HTTP request failed: " + std::string(curl_easy_strerror(result));
                                }

                                if (hdrs) {
                                    curl_slist_free_all(hdrs);
                                }
                                curl_easy_cleanup(curl);
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
