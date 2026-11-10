#include "file_io.h"
#include "nari_fs.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#ifdef _WIN32
#include <corecrt_io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__) && !defined(NO_THREADS)
#define NARI_CANCELLABLE_POSIX_FILES
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#endif

namespace nari {
namespace {

std::error_code stream_error() {
    return std::error_code(errno ? errno : EIO, std::generic_category());
}

FILE *open_file(const std::string &path, const char *mode, std::error_code &error) {
    if (!fs::validate_path(path, error)) {
        return nullptr;
    }
    errno = 0;
    FILE *file = std::fopen(path.c_str(), mode);
    if (!file) {
        error = stream_error();
    }
    return file;
}

bool cancelled(const std::atomic<bool> *cancel, std::error_code &error) {
    if (cancel && cancel->load()) {
        error = std::make_error_code(std::errc::operation_canceled);
        return true;
    }
    return false;
}

bool finish_file(FILE *file, bool success, std::error_code &error) {
    std::error_code close_error;
    if (!close_file(file, close_error) && success) {
        error = close_error;
        success = false;
    }
    return success;
}

#ifdef NARI_CANCELLABLE_POSIX_FILES
struct FileDescriptor {
    int fd;
    explicit FileDescriptor(int value) : fd(value) {
    }
    ~FileDescriptor() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    bool close(std::error_code &error) {
        int value = fd;
        fd = -1;
        if (::close(value) != 0) {
            error = stream_error();
            return false;
        }
        return true;
    }
};

// A broken pipe is an I/O error, not a reason to terminate the process. Block
// SIGPIPE on this thread and consume only a newly generated write signal.
struct PipeSignalGuard {
    sigset_t signal_set{}, previous{}, pending{};
    int status;
    bool already_pending = false;
    bool broken = false;
    PipeSignalGuard() {
        sigemptyset(&signal_set);
        sigaddset(&signal_set, SIGPIPE);
        status = pthread_sigmask(SIG_BLOCK, &signal_set, &previous);
        if (status == 0 && sigpending(&pending) == 0) {
            already_pending = sigismember(&pending, SIGPIPE) == 1;
        }
    }
    ~PipeSignalGuard() {
        if (status == 0) {
            if (broken && !already_pending && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
                int received;
                sigwait(&signal_set, &received);
            }
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        }
    }
};

bool wait_file(int fd, short events, const std::atomic<bool> *cancel, std::error_code &error) {
    while (!cancelled(cancel, error)) {
        pollfd descriptor{ fd, events, 0 };
        int ready = ::poll(&descriptor, 1, 20);
        if (ready > 0) {
            if (descriptor.revents & POLLNVAL) {
                error = std::make_error_code(std::errc::bad_file_descriptor);
                return false;
            }
            return true; // Let read/write report EOF, hangup, and device errors.
        }
        if (ready < 0 && errno != EINTR) {
            error = stream_error();
            return false;
        }
    }
    return false;
}

int open_cancellable(const std::string &path, int flags, const std::atomic<bool> *cancel, std::error_code &error) {
    if (!fs::validate_path(path, error)) {
        return -1;
    }
    while (!cancelled(cancel, error)) {
        int fd = ::open(path.c_str(), flags | O_NONBLOCK | O_CLOEXEC, 0666);
        if (fd >= 0) {
            return fd;
        }
        int saved = errno;
        if (saved == EINTR) {
            continue;
        }
        // A FIFO writer waits for its reader without blocking inside open().
        struct stat info{};
        if (saved == ENXIO && (flags & O_WRONLY) && ::stat(path.c_str(), &info) == 0 && S_ISFIFO(info.st_mode)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        error = std::error_code(saved, std::generic_category());
        return -1;
    }
    return -1;
}

bool read_cancellable(const std::string &path, std::string &output, std::error_code &error, const std::atomic<bool> *cancel) {
    FileDescriptor file(open_cancellable(path, O_RDONLY, cancel, error));
    if (file.fd < 0) {
        return false;
    }
    char buffer[4096];
    while (wait_file(file.fd, POLLIN, cancel, error)) {
        auto bytes = ::read(file.fd, buffer, sizeof(buffer));
        if (bytes == 0) {
            return file.close(error);
        }
        if (bytes > 0) {
            output.append(buffer, static_cast<size_t>(bytes));
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            error = stream_error();
            return false;
        }
    }
    return false;
}

bool write_cancellable(
    const std::string &path, const void *data, size_t size, std::error_code &error, bool append, const std::atomic<bool> *cancel
) {
    PipeSignalGuard signals;
    if (signals.status != 0) {
        error = std::error_code(signals.status, std::generic_category());
        return false;
    }
    FileDescriptor file(open_cancellable(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), cancel, error));
    if (file.fd < 0) {
        return false;
    }
    auto *bytes = static_cast<const char *>(data);
    while (size != 0 && wait_file(file.fd, POLLOUT, cancel, error)) {
        auto written = ::write(file.fd, bytes, std::min<size_t>(size, 65536));
        if (written > 0) {
            bytes += written;
            size -= static_cast<size_t>(written);
        } else if (written == 0) {
            error = std::make_error_code(std::errc::io_error);
            return false;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            signals.broken = errno == EPIPE;
            error = stream_error();
            return false;
        }
    }
    if (size != 0 || cancelled(cancel, error)) {
        return false;
    }
    return file.close(error);
}
#endif

} // namespace

bool close_file(FILE *&stream, std::error_code &error) {
    error.clear();
    FILE *file = stream;
    stream = nullptr;
    if (!file) {
        return true;
    }
    errno = 0;
    if (std::fclose(file) != 0) {
        error = stream_error();
        return false;
    }
    return true;
}

bool seek_file(FILE *stream, int64_t offset, std::error_code &error) {
    error.clear();
    if (offset < 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    errno = 0;
#ifdef _WIN32
    int status = ::_fseeki64(stream, offset, SEEK_SET);
#else
    if (static_cast<uint64_t>(offset) > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        error = std::make_error_code(std::errc::value_too_large);
        return false;
    }
    int status = ::fseeko(stream, static_cast<off_t>(offset), SEEK_SET);
#endif
    if (status != 0) {
        error = stream_error();
        return false;
    }
    return true;
}

bool resize_file(FILE *stream, int64_t size, std::error_code &error) {
    error.clear();
    if (size < 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    errno = 0;
    if (std::fflush(stream) != 0) {
        error = stream_error();
        return false;
    }
#ifdef _WIN32
    int status = ::_chsize_s(::_fileno(stream), size);
    if (status != 0) {
        error = std::error_code(status, std::generic_category());
        return false;
    }
#else
    if (static_cast<uint64_t>(size) > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        error = std::make_error_code(std::errc::value_too_large);
        return false;
    }
    if (::ftruncate(::fileno(stream), static_cast<off_t>(size)) != 0) {
        error = stream_error();
        return false;
    }
#endif
    return true;
}

bool read_stream(FILE *stream, std::string &output, std::error_code &error, const std::atomic<bool> *cancel) {
    output.clear();
    error.clear();
    char buffer[4096];
    for (;;) {
        if (cancelled(cancel, error)) {
            output.clear();
            return false;
        }
        errno = 0;
        size_t bytes = std::fread(buffer, 1, sizeof(buffer), stream);
        if (std::ferror(stream)) {
            error = stream_error();
            output.clear();
            return false;
        }
        output.append(buffer, bytes);
        if (std::feof(stream)) {
            return true;
        }
        if (bytes == 0) {
            error = std::make_error_code(std::errc::io_error);
            output.clear();
            return false;
        }
    }
}

bool write_stream(FILE *stream, const void *data, size_t size, std::error_code &error, const std::atomic<bool> *cancel) {
    error.clear();
    const char *bytes = static_cast<const char *>(data);
    while (size != 0) {
        if (cancelled(cancel, error)) {
            return false;
        }
        errno = 0;
        size_t written = std::fwrite(bytes, 1, std::min<size_t>(size, 65536), stream);
        if (std::ferror(stream) || written == 0) {
            error = stream_error();
            return false;
        }
        bytes += written;
        size -= written;
    }
    if (cancelled(cancel, error)) {
        return false;
    }
    errno = 0;
    if (std::fflush(stream) != 0 || std::ferror(stream)) {
        error = stream_error();
        return false;
    }
    return true;
}

bool read_file(const std::string &path, std::string &output, std::error_code &error, const std::atomic<bool> *cancel) {
    output.clear();
    error.clear();
    if (cancelled(cancel, error)) {
        return false;
    }
#ifdef NARI_CANCELLABLE_POSIX_FILES
    if (cancel) {
        bool success = read_cancellable(path, output, error, cancel);
        if (!success) {
            output.clear();
        }
        return success;
    }
#endif
    FILE *file = open_file(path, "rb", error);
    if (!file) {
        return false;
    }
    bool success = read_stream(file, output, error, cancel);
    success = finish_file(file, success, error);
    if (!success) {
        output.clear();
    }
    return success;
}

bool write_file(
    const std::string &path, const void *data, size_t size, std::error_code &error, bool append, const std::atomic<bool> *cancel
) {
    error.clear();
    if (cancelled(cancel, error)) {
        return false;
    }
#ifdef NARI_CANCELLABLE_POSIX_FILES
    if (cancel) {
        return write_cancellable(path, data, size, error, append, cancel);
    }
#endif
    FILE *file = open_file(path, append ? "ab" : "wb", error);
    if (!file) {
        return false;
    }
    bool success = write_stream(file, data, size, error, cancel);
    return finish_file(file, success, error);
}

} // namespace nari
