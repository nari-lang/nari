#pragma once

// Platform sleep/socket shims shared by the I/O operation and worker headers.

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>

#ifndef NO_THREADS
#include <chrono>
#include <thread>
#endif

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
#define NARI_SLEEP_MICROS(us) esp_rom_delay_us(static_cast<uint32_t>((us)))
#endif
#else
// Generic bare-metal MCU: replace NARI_SLEEP_MILLIS / NARI_SLEEP_MICROS
// in your platform glue layer before including this header, or accept a
// no-op (async I/O paths are never reached in bytecode-only builds).
#warning                                                                                                                                   \
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
#define NARI_SLEEP_MILLIS(ms)                                                                                                              \
    do {                                                                                                                                   \
        struct timespec _ts{ 0, static_cast<long>((ms)) * 1000000L };                                                                      \
        nanosleep(&_ts, nullptr);                                                                                                          \
    } while (0)
#endif
#ifndef NARI_SLEEP_MICROS
#define NARI_SLEEP_MICROS(us)                                                                                                              \
    do {                                                                                                                                   \
        struct timespec _ts{ 0, static_cast<long>((us)) * 1000L };                                                                         \
        nanosleep(&_ts, nullptr);                                                                                                          \
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
#define NARI_CLOSE_SOCKET(fd) ::closesocket(static_cast<SOCKET>(fd))
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

#if !defined(NO_THREADS) && !defined(DISABLE_HTTP)
inline int nari_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline std::string nari_socket_error_message(int error = nari_socket_error()) {
#ifdef _WIN32
    return std::system_category().message(error);
#else
    return std::strerror(error);
#endif
}

inline bool nari_socket_interrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

inline bool nari_socket_would_block(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}

// Readiness is only advisory: a second operation can consume it before recv or
// accept runs. Nonblocking sockets let workers retry and observe shutdown.
inline bool nari_prepare_socket(nari_socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return false;
    }
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        return false;
    }
#endif
    return true;
#endif
}

#ifdef MSG_NOSIGNAL
inline constexpr int nari_socket_send_flags = MSG_NOSIGNAL;
#else
inline constexpr int nari_socket_send_flags = 0;
#endif
#endif
