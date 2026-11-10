#include "io.h"

#ifndef NO_THREADS
#include "io_event_loop.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

#ifndef DISABLE_HTTP
#include <ares.h>
#include <curl/curl.h>

namespace {
// Initialize once, before dispatchers start, and clean up after all runtimes.
struct NetworkLibraries {
    std::string error;
    bool winsock_ready = false;
    bool ares_ready = false;
    bool curl_ready = false;
    bool curl_async_dns = false;

    NetworkLibraries() {
#ifdef _WIN32
        WSADATA data;
        int winsock_status = WSAStartup(MAKEWORD(2, 2), &data);
        if (winsock_status != 0) {
            error = "Winsock initialization failed: " + nari_socket_error_message(winsock_status);
            return;
        }
        winsock_ready = true;
#endif
        int status = ares_library_init(ARES_LIB_INIT_ALL);
        if (status != ARES_SUCCESS) {
            error = "DNS initialization failed: " + std::string(ares_strerror(status));
            return;
        }
        ares_ready = true;
        CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            error = "HTTP initialization failed: " + std::string(curl_easy_strerror(code));
            return;
        }
        curl_ready = true;
        const auto *version = curl_version_info(CURLVERSION_NOW);
        curl_async_dns = version && version->ares;
    }
    ~NetworkLibraries() {
        if (curl_ready) {
            curl_global_cleanup();
        }
        if (ares_ready) {
            ares_library_cleanup();
        }
#ifdef _WIN32
        if (winsock_ready) {
            WSACleanup();
        }
#endif
    }
};

NetworkLibraries &network_libraries() {
    static NetworkLibraries libraries;
    return libraries;
}
} // namespace
#endif

struct IOEventLoop::Impl {
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Pending {
        IOOperationPtr op;
        Time submitted;
    };
    struct Timer {
        Time due;
        uint64_t order;
        IOOperationPtr op;
        bool operator<(const Timer &other) const {
            return due == other.due ? order > other.order : due > other.due;
        }
    };

    static Time deadline(Time now, int64_t delay_ms) {
        if (delay_ms <= 0) {
            return now;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(Time::max() - now).count();
        return delay_ms >= remaining ? Time::max() : now + std::chrono::milliseconds(delay_ms);
    }

#ifndef DISABLE_HTTP
    struct DnsResult {
        bool ready = false;
        sockaddr_in address{};
        std::string error;
    };
    struct DnsCallback {
        Impl &loop;
        std::shared_ptr<DnsResult> result;
        std::string host;
    };
#endif
    struct State {
        IOOperationPtr op;
        Time due;
        bool started = false;
        bool done = false;
#ifndef DISABLE_HTTP
        std::shared_ptr<DnsResult> dns;
        size_t written = 0;
        nari_socket_t wait_fd = NARI_INVALID_SOCKET;
        short wait_events = 0;
        CURL *easy = nullptr;
        curl_slist *headers = nullptr;
        bool added = false;
        ~State() {
            if (easy) {
                curl_easy_cleanup(easy);
            }
            if (headers) {
                curl_slist_free_all(headers);
            }
        }
#endif
        State(IOOperationPtr operation, Time submitted) : op(std::move(operation)), due(submitted) {
#ifndef DISABLE_HTTP
            if (op->type == IOOperation::Type::UdpRecv) {
                auto udp = std::static_pointer_cast<UdpOperation>(op);
                due = udp->timeout_ms < 0 ? Time::max() : deadline(submitted, udp->timeout_ms);
            }
#endif
        }
    };

    std::function<void(IOOperationPtr)> complete;
    std::mutex mutex;
    std::condition_variable wake;
    std::queue<Pending> incoming;
    std::priority_queue<Timer> timers;
    uint64_t next_timer_order = 0;
    std::list<State> active;
    std::atomic<bool> stopped{ false };
    std::thread dispatcher;
    std::once_flag shutdown_once;
#ifndef DISABLE_HTTP
    CURLM *multi = nullptr;
    ares_channel_t *resolver = nullptr;
    std::map<ares_socket_t, short> dns_sockets;
    size_t dns_pending = 0;
    std::string network_error;
#endif

    explicit Impl(std::function<void(IOOperationPtr)> callback) : complete(std::move(callback)) {
#ifndef DISABLE_HTTP
        network_error = network_libraries().error;
        if (network_error.empty()) {
            multi = curl_multi_init();
            if (!multi) {
                network_error = "HTTP dispatcher initialization failed";
            } else {
                curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, 64L);
                ares_options options{};
                options.sock_state_cb = dns_socket_changed;
                options.sock_state_cb_data = this;
                int status = ares_init_options(&resolver, &options, ARES_OPT_SOCK_STATE_CB);
                if (status != ARES_SUCCESS) {
                    network_error = "DNS dispatcher initialization failed: " + std::string(ares_strerror(status));
                }
            }
        }
#endif
        dispatcher = std::thread([this] { run(); });
    }

    ~Impl() {
        shutdown();
#ifndef DISABLE_HTTP
        if (resolver) {
            ares_destroy(resolver);
        }
        if (multi) {
            curl_multi_cleanup(multi);
        }
#endif
    }

    static bool fail(State &state, std::string message) {
        state.op->success = false;
        state.op->error_msg = std::move(message);
        return true;
    }

#ifndef DISABLE_HTTP
    static void dns_socket_changed(void *data, ares_socket_t fd, int readable, int writable) {
        auto &loop = *static_cast<Impl *>(data);
        short events = (readable ? CURL_WAIT_POLLIN : 0) | (writable ? CURL_WAIT_POLLOUT : 0);
        if (events) {
            loop.dns_sockets[fd] = events;
        } else {
            loop.dns_sockets.erase(fd);
        }
    }

    static void dns_completed(void *data, int status, int, ares_addrinfo *addresses) {
        std::unique_ptr<DnsCallback> callback(static_cast<DnsCallback *>(data));
        auto &result = *callback->result;
        if (status == ARES_SUCCESS && addresses && addresses->nodes) {
            result.address = *reinterpret_cast<const sockaddr_in *>(addresses->nodes->ai_addr);
        } else {
            result.error =
                "Failed to resolve host: " + callback->host + ": " + (status == ARES_SUCCESS ? "No IPv4 address" : ares_strerror(status));
        }
        result.ready = true;
        --callback->loop.dns_pending;
        if (addresses) {
            ares_freeaddrinfo(addresses);
        }
    }

    bool resolve(State &state, const std::string &host, int port, int type) {
        if (!state.dns) {
            state.dns = std::make_shared<DnsResult>();
            ares_addrinfo_hints hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = type;
            hints.ai_flags = ARES_AI_NOSORT;
            ++dns_pending;
            // The query owns only native data. A closed socket can complete its
            // operation before DNS finishes without leaving a dangling State*.
            auto *callback = new DnsCallback{ *this, state.dns, host };
            ares_getaddrinfo(resolver, host.c_str(), nullptr, &hints, dns_completed, callback);
        }
        if (!state.dns->ready) {
            return false;
        }
        state.dns->address.sin_port = htons(port);
        if (!state.dns->error.empty()) {
            fail(state, state.dns->error);
        }
        return true;
    }

    static bool retry_socket(State &state, const char *action) {
        int code = nari_socket_error();
        if (nari_socket_would_block(code) || nari_socket_interrupted(code)) {
            return false;
        }
        return fail(state, std::string(action) + ": " + nari_socket_error_message(code));
    }

    static bool socket_ready(State &state) {
#ifdef _WIN32
        fd_set ready, failed;
        FD_ZERO(&ready);
        FD_ZERO(&failed);
        FD_SET(state.wait_fd, &ready);
        FD_SET(state.wait_fd, &failed);
        timeval timeout{};
        int result = select(0, nullptr, &ready, &failed, &timeout);
#else
        pollfd fd{ state.wait_fd, POLLOUT, 0 };
        int result = poll(&fd, 1, 0);
#endif
        if (result < 0) {
            state.done = retry_socket(state, "Socket wait failed");
        }
        return result > 0;
    }

    bool step_tcp(State &state) {
        auto op = std::static_pointer_cast<TcpOperation>(state.op);
        if (op->socket_owner && op->socket_owner->closed.load()) {
            return fail(state, "Socket closed");
        }
        switch (op->type) {
            case IOOperation::Type::TcpListen: {
                op->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (op->socket_fd == NARI_INVALID_SOCKET) {
                    return fail(state, "Failed to create socket: " + nari_socket_error_message());
                }
                if (!nari_prepare_socket(op->socket_fd)) {
                    return fail(state, "Failed to configure socket: " + nari_socket_error_message());
                }
                int enabled = 1;
                setsockopt(op->socket_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&enabled), sizeof(enabled));
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = INADDR_ANY;
                address.sin_port = htons(op->port);
                if (bind(op->socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
                    return fail(state, "Failed to bind to port " + std::to_string(op->port) + ": " + nari_socket_error_message());
                }
                if (listen(op->socket_fd, 128) < 0) {
                    return fail(state, "Failed to listen: " + nari_socket_error_message());
                }
                socklen_t length = sizeof(address);
                if (getsockname(op->socket_fd, reinterpret_cast<sockaddr *>(&address), &length) < 0) {
                    return fail(state, "Failed to read bound port: " + nari_socket_error_message());
                }
                op->port = ntohs(address.sin_port);
                op->success = true;
                return true;
            }
            case IOOperation::Type::TcpConnect: {
                if (!resolve(state, op->host, op->port, SOCK_STREAM)) {
                    return false;
                }
                if (!op->error_msg.empty()) {
                    return true;
                }
                if (!state.started) {
                    state.started = true;
                    op->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
                    if (op->socket_fd == NARI_INVALID_SOCKET) {
                        return fail(state, "Failed to create socket: " + nari_socket_error_message());
                    }
                    if (!nari_prepare_socket(op->socket_fd)) {
                        return fail(state, "Failed to configure socket: " + nari_socket_error_message());
                    }
                    if (connect(op->socket_fd, reinterpret_cast<sockaddr *>(&state.dns->address), sizeof(sockaddr_in)) == 0) {
                        op->success = true;
                        return true;
                    }
                    if (retry_socket(state, "Failed to connect")) {
                        return true;
                    }
                    state.wait_fd = op->socket_fd;
                    state.wait_events = CURL_WAIT_POLLOUT;
                    return false;
                }
                if (!socket_ready(state)) {
                    return state.done;
                }
                int error = 0;
                socklen_t length = sizeof(error);
                if (getsockopt(op->socket_fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length) < 0) {
                    error = nari_socket_error();
                }
                if (error != 0) {
                    return fail(state, "Failed to connect: " + nari_socket_error_message(error));
                }
                op->success = true;
                return true;
            }
            default:
                break;
        }
        if (op->socket_fd == NARI_INVALID_SOCKET) {
            return fail(state, "Invalid socket");
        }
        state.wait_fd = op->socket_fd;
        state.wait_events = op->type == IOOperation::Type::TcpWrite ? CURL_WAIT_POLLOUT : CURL_WAIT_POLLIN;
        if (op->type == IOOperation::Type::TcpAccept) {
            sockaddr_in address{};
            socklen_t length = sizeof(address);
            op->client_fd = accept(op->socket_fd, reinterpret_cast<sockaddr *>(&address), &length);
            if (op->client_fd == NARI_INVALID_SOCKET) {
                return retry_socket(state, "Failed to accept connection");
            }
            if (!nari_prepare_socket(op->client_fd)) {
                return fail(state, "Failed to configure connection: " + nari_socket_error_message());
            }
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
            op->client_ip = ip;
            op->client_port = ntohs(address.sin_port);
        } else if (op->type == IOOperation::Type::TcpRead) {
            char buffer[8192];
            auto bytes = recv(op->socket_fd, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (bytes < 0) {
                return retry_socket(state, "Failed to read from socket");
            }
            op->result_string.assign(buffer, static_cast<size_t>(bytes));
        } else {
            if (state.written < op->data.size()) {
                int length = static_cast<int>(std::min<size_t>(65536, op->data.size() - state.written));
                auto bytes = send(op->socket_fd, op->data.data() + state.written, length, nari_socket_send_flags);
                if (bytes < 0) {
                    return retry_socket(state, "Failed to write to socket");
                }
                if (bytes == 0) {
                    return fail(state, "Socket write made no progress");
                }
                state.written += static_cast<size_t>(bytes);
                if (state.written != op->data.size()) {
                    return false;
                }
            }
        }
        op->success = true;
        return true;
    }

    bool step_udp(State &state) {
        auto op = std::static_pointer_cast<UdpOperation>(state.op);
        if (op->socket_owner && op->socket_owner->closed.load()) {
            return fail(state, "Socket closed");
        }
        if (op->type == IOOperation::Type::UdpBind) {
            op->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (op->socket_fd == NARI_INVALID_SOCKET) {
                return fail(state, "Failed to create UDP socket: " + nari_socket_error_message());
            }
            if (!nari_prepare_socket(op->socket_fd)) {
                return fail(state, "Failed to configure UDP socket: " + nari_socket_error_message());
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = INADDR_ANY;
            address.sin_port = htons(op->port);
            if (bind(op->socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
                return fail(state, "Failed to bind UDP socket: " + nari_socket_error_message());
            }
            socklen_t length = sizeof(address);
            if (getsockname(op->socket_fd, reinterpret_cast<sockaddr *>(&address), &length) < 0) {
                return fail(state, "Failed to read bound port: " + nari_socket_error_message());
            }
            op->bound_port = ntohs(address.sin_port);
            op->success = true;
            return true;
        }
        if (op->socket_fd == NARI_INVALID_SOCKET) {
            return fail(state, "Invalid UDP socket");
        }
        state.wait_fd = op->socket_fd;
        if (op->type == IOOperation::Type::UdpSend) {
            if (op->data.size() > 65507) {
                return fail(state, "UDP datagram too large");
            }
            if (!resolve(state, op->host, op->port, SOCK_DGRAM)) {
                return false;
            }
            if (!op->error_msg.empty()) {
                return true;
            }
            state.wait_events = CURL_WAIT_POLLOUT;
            auto bytes = sendto(
                op->socket_fd, op->data.data(), static_cast<int>(op->data.size()), 0, reinterpret_cast<sockaddr *>(&state.dns->address),
                sizeof(sockaddr_in)
            );
            if (bytes < 0) {
                return retry_socket(state, "Failed to send UDP datagram");
            }
        } else {
            state.wait_events = CURL_WAIT_POLLIN;
            char buffer[65536];
            sockaddr_in address{};
            socklen_t length = sizeof(address);
            auto bytes =
                recvfrom(op->socket_fd, buffer, static_cast<int>(sizeof(buffer)), 0, reinterpret_cast<sockaddr *>(&address), &length);
            if (bytes < 0) {
                if (retry_socket(state, "Failed to receive UDP datagram")) {
                    return true;
                }
                return Clock::now() >= state.due ? fail(state, "Recv timeout") : false;
            }
            op->result_string.assign(buffer, static_cast<size_t>(bytes));
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
            op->from_ip = ip;
            op->from_port = ntohs(address.sin_port);
        }
        op->success = true;
        return true;
    }

    bool start_http(State &state) {
        if (!network_libraries().curl_async_dns) {
            return fail(state, "HTTP requires libcurl built with c-ares for cancellable DNS");
        }
        auto op = std::static_pointer_cast<HttpOperation>(state.op);
        state.easy = curl_easy_init();
        if (!state.easy) {
            return fail(state, "HTTP request initialization failed");
        }
        auto set = [&](CURLoption option, auto value) {
            if (op->error_msg.empty()) {
                CURLcode code = curl_easy_setopt(state.easy, option, value);
                if (code != CURLE_OK) {
                    fail(state, "HTTP setup failed: " + std::string(curl_easy_strerror(code)));
                }
            }
        };
        set(CURLOPT_PRIVATE, &state);
        set(CURLOPT_URL, op->full_url.c_str());
        set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
        set(CURLOPT_FOLLOWLOCATION, op->follow_redirects ? 1L : 0L);
        set(CURLOPT_MAXREDIRS, 5L);
        set(CURLOPT_PROTOCOLS_STR, "http,https");
        // Redirects from HTTPS must retain transport security.
        bool secure = op->full_url.size() >= 8 &&
                      std::equal(op->full_url.begin(), op->full_url.begin() + 8, "https://", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
        set(CURLOPT_REDIR_PROTOCOLS_STR, secure ? "https" : "http,https");
        if (op->timeout_ms > 0) {
            set(CURLOPT_TIMEOUT_MS, op->timeout_ms);
            set(CURLOPT_CONNECTTIMEOUT_MS, std::min(op->timeout_ms, 10000L));
        }
        if (op->max_response_bytes >= 0) {
            set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(op->max_response_bytes));
        }
        set(CURLOPT_NOSIGNAL, 1L);
        set(CURLOPT_SSL_VERIFYPEER, 1L);
        set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_USERAGENT, "Nari-HTTP/1.0");
        set(
            CURLOPT_WRITEFUNCTION, +[](char *data, size_t size, size_t count, void *target) -> size_t {
                auto &response = *static_cast<HttpOperation *>(target);
                if (size != 0 && count > SIZE_MAX / size) { return 0; }
                size_t bytes = size * count;
                const uint64_t limit = static_cast<uint64_t>(response.max_response_bytes);
                if (response.max_response_bytes >= 0 &&
                    (response.result_string.size() > limit || bytes > limit - response.result_string.size())) {
                    response.error_msg = "HTTP response exceeds max_bytes";
                    return 0;
                }
                response.result_string.append(data, bytes);
                return bytes;
            }
        );
        set(CURLOPT_WRITEDATA, op.get());
        set(
            CURLOPT_HEADERFUNCTION, +[](char *data, size_t size, size_t count, void *target) -> size_t {
                auto &response = *static_cast<HttpOperation *>(target);
                size_t bytes = size * count;
                std::string header(data, bytes);
                while (!header.empty() && (header.back() == '\r' || header.back() == '\n')) {
                    header.pop_back();
                }
                if (header.compare(0, 5, "HTTP/") == 0) {
                    response.response_headers.clear();
                    response.result_string.clear();
                }
                size_t colon = header.find(':');
                if (colon != std::string::npos) {
                    size_t value = header.find_first_not_of(" \t", colon + 1);
                    response.response_headers[header.substr(0, colon)] = value == std::string::npos ? "" : header.substr(value);
                }
                return bytes;
            }
        );
        set(CURLOPT_HEADERDATA, op.get());
        for (const auto &[key, value] : op->headers) {
            curl_slist *headers = curl_slist_append(state.headers, (key + ": " + value).c_str());
            if (!headers) {
                return fail(state, "HTTP header allocation failed");
            }
            state.headers = headers;
        }
        if (state.headers) {
            set(CURLOPT_HTTPHEADER, state.headers);
        }
        if (op->method == "POST") {
            set(CURLOPT_POST, 1L);
        } else if (op->method != "GET") {
            set(CURLOPT_CUSTOMREQUEST, op->method.c_str());
        }
        if (op->method == "POST" || (op->method != "GET" && !op->body.empty())) {
            set(CURLOPT_POSTFIELDS, op->body.data());
            set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(op->body.size()));
        }
        if (!op->error_msg.empty()) {
            return true;
        }
        CURLMcode code = curl_multi_add_handle(multi, state.easy);
        if (code != CURLM_OK) {
            return fail(state, "HTTP dispatch failed: " + std::string(curl_multi_strerror(code)));
        }
        state.added = true;
        return false;
    }

    void pump_http() {
        int running = 0;
        CURLMcode code = curl_multi_perform(multi, &running);
        if (code != CURLM_OK) {
            for (auto &state : active) {
                if (state.easy) {
                    state.done = fail(state, "HTTP dispatch failed: " + std::string(curl_multi_strerror(code)));
                }
            }
        }
        int remaining = 0;
        while (CURLMsg *message = curl_multi_info_read(multi, &remaining)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            State *state = nullptr;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &state);
            if (!state) {
                continue;
            }
            auto op = std::static_pointer_cast<HttpOperation>(state->op);
            if (message->data.result == CURLE_OK) {
                long status = 0;
                curl_easy_getinfo(state->easy, CURLINFO_RESPONSE_CODE, &status);
                op->status_code = static_cast<int>(status);
                op->success = true;
            } else {
                fail(*state, "HTTP request failed: " + std::string(curl_easy_strerror(message->data.result)));
            }
            state->done = true;
        }
    }
#endif

    bool step(State &state) {
#ifndef DISABLE_HTTP
        if (!network_error.empty()) {
            return fail(state, network_error);
        }
        switch (state.op->type) {
            case IOOperation::Type::TcpListen:
            case IOOperation::Type::TcpConnect:
            case IOOperation::Type::TcpAccept:
            case IOOperation::Type::TcpRead:
            case IOOperation::Type::TcpWrite:
                return step_tcp(state);
            case IOOperation::Type::UdpBind:
            case IOOperation::Type::UdpSend:
            case IOOperation::Type::UdpRecv:
                return step_udp(state);
            case IOOperation::Type::HttpRequest:
                if (!state.started) {
                    state.started = true;
                    return start_http(state);
                }
                return false;
            default:
                break;
        }
#endif
        return fail(state, "Unsupported event operation");
    }

    void publish_completed() {
        for (auto it = active.begin(); it != active.end();) {
            if (!it->done) {
                ++it;
                continue;
            }
#ifndef DISABLE_HTTP
            if (it->added) {
                curl_multi_remove_handle(multi, it->easy);
                it->added = false;
            }
#endif
            auto op = std::move(it->op);
            it = active.erase(it);
            complete(std::move(op));
        }
    }

    void run() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                bool idle = active.empty() && timers.empty();
#ifndef DISABLE_HTTP
                idle = idle && dns_pending == 0;
#endif
                if (idle) {
                    wake.wait(lock, [this] { return stopped || !incoming.empty(); });
                }
                while (!incoming.empty()) {
                    auto item = std::move(incoming.front());
                    incoming.pop();
                    if (item.op->type == IOOperation::Type::Timer) {
                        auto due = deadline(item.submitted, item.op->timer_ms);
                        timers.push({ due, next_timer_order++, std::move(item.op) });
                    } else {
                        active.emplace_back(std::move(item.op), item.submitted);
                    }
                }
            }
            if (stopped) {
                while (!timers.empty()) {
                    auto op = timers.top().op;
                    timers.pop();
                    op->success = false;
                    op->error_msg = "Shutdown requested";
                    complete(std::move(op));
                }
#ifndef DISABLE_HTTP
                if (resolver) {
                    ares_cancel(resolver);
                }
#endif
                for (auto &state : active) {
                    state.done = fail(state, "Shutdown requested");
                }
                publish_completed();
                return;
            }
            int timeout_ms = active.empty() ? 1000 : 20;
            auto now = Clock::now();
            while (!timers.empty() && timers.top().due <= now) {
                auto op = timers.top().op;
                timers.pop();
                op->success = true;
                complete(std::move(op));
            }
            if (!timers.empty()) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(timers.top().due - now).count();
                timeout_ms = static_cast<int>(std::min<int64_t>(timeout_ms, std::max<int64_t>(0, left)));
            }
            for (auto &state : active) {
                if (!state.done) {
                    state.done = step(state);
                }
#ifndef DISABLE_HTTP
                if (!state.done && state.due != Time::max() && state.op->type == IOOperation::Type::UdpRecv) {
                    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(state.due - Clock::now()).count();
                    timeout_ms = static_cast<int>(std::min<int64_t>(timeout_ms, std::max<int64_t>(0, left)));
                }
#endif
            }
#ifndef DISABLE_HTTP
            if (multi) {
                pump_http();
            }
#endif
            publish_completed();
#ifndef DISABLE_HTTP
            std::vector<curl_waitfd> sockets;
            for (const auto &state : active) {
                if (state.wait_fd != NARI_INVALID_SOCKET && state.wait_events) {
                    sockets.push_back({ static_cast<curl_socket_t>(state.wait_fd), state.wait_events, 0 });
                }
            }
            for (const auto &[fd, events] : dns_sockets) {
                sockets.push_back({ static_cast<curl_socket_t>(fd), events, 0 });
            }
            if (resolver) {
                timeval maximum{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 }, next{};
                const timeval *wait = ares_timeout(resolver, &maximum, &next);
                if (wait && wait->tv_sec == 0) {
                    timeout_ms = std::min(timeout_ms, static_cast<int>((wait->tv_usec + 999) / 1000));
                }
            }
            if (multi) {
                CURLMcode code = curl_multi_poll(multi, sockets.data(), static_cast<unsigned>(sockets.size()), timeout_ms, nullptr);
                if (code != CURLM_OK) {
                    for (auto &state : active) {
                        state.done = fail(state, "Network poll failed: " + std::string(curl_multi_strerror(code)));
                    }
                }
            } else {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return stopped || !incoming.empty(); });
            }
            if (resolver) {
                std::vector<ares_fd_events_t> events;
                for (const auto &socket : sockets) {
                    if (!socket.revents || dns_sockets.find(static_cast<ares_socket_t>(socket.fd)) == dns_sockets.end()) {
                        continue;
                    }
                    unsigned flags = (socket.revents & CURL_WAIT_POLLIN ? ARES_FD_EVENT_READ : 0) |
                                     (socket.revents & CURL_WAIT_POLLOUT ? ARES_FD_EVENT_WRITE : 0);
                    if (!flags) {
                        flags = ARES_FD_EVENT_READ | ARES_FD_EVENT_WRITE;
                    }
                    events.push_back({ static_cast<ares_socket_t>(socket.fd), flags });
                }
                ares_process_fds(resolver, events.data(), events.size(), 0);
            }
#else
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return stopped || !incoming.empty(); });
#endif
        }
    }

    void submit(IOOperationPtr op) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            incoming.push({ std::move(op), Clock::now() });
        }
        wake.notify_one();
#ifndef DISABLE_HTTP
        if (multi) {
            curl_multi_wakeup(multi);
        }
#endif
    }

    void shutdown() {
        std::call_once(shutdown_once, [this] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopped = true;
            }
            wake.notify_one();
#ifndef DISABLE_HTTP
            if (multi) {
                curl_multi_wakeup(multi);
            }
#endif
            dispatcher.join();
        });
    }
};

IOEventLoop::IOEventLoop(std::function<void(IOOperationPtr)> callback) : impl(std::make_unique<Impl>(std::move(callback))) {
}
IOEventLoop::~IOEventLoop() = default;
void IOEventLoop::submit(IOOperationPtr op) {
    impl->submit(std::move(op));
}
void IOEventLoop::shutdown() {
    impl->shutdown();
}
#endif
