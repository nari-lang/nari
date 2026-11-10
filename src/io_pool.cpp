#include "file_io.h"
#include "io.h"
#include "nari_fs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <queue>

#ifndef NO_THREADS
#include "io_event_loop.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
#endif

struct IOThreadPool::Impl {
    std::queue<IOOperationPtr> completed;
#ifdef NO_THREADS
    bool stopped = false;
    explicit Impl(size_t) {
    }
    void shutdown() {
        stopped = true;
    }
    void submit(IOOperationPtr op) {
        if (!op) {
            return;
        }
        op->success = false;
        op->error_msg = stopped ? "I/O pool is stopped" : "Asynchronous I/O is not supported in this build";
        completed.push(std::move(op));
    }
#else
    struct Registry {
        std::mutex mutex;
        std::vector<Impl *> pools;
    };
    static Registry &registry() {
        static Registry value;
        return value;
    }
    static void shutdown_at_exit() {
        auto &value = registry();
        std::lock_guard<std::mutex> lock(value.mutex);
        for (auto *pool : value.pools) {
            pool->shutdown();
        }
    }
    std::mutex completed_mutex;
    std::atomic<size_t> pending{ 0 };
    std::atomic<bool> stopped{ false };
    std::queue<IOOperationPtr> work;
    std::mutex work_mutex;
    std::condition_variable wake;
    std::vector<std::thread> workers;
    std::atomic<size_t> live_workers{ 0 };
    std::unique_ptr<IOEventLoop> events;
    std::once_flag shutdown_once;

    void complete(IOOperationPtr op) {
        {
            std::lock_guard<std::mutex> lock(completed_mutex);
            completed.push(std::move(op));
        }
        --pending;
    }

    explicit Impl(size_t count) {
        events = std::make_unique<IOEventLoop>([this](IOOperationPtr op) { complete(std::move(op)); });
        for (size_t i = 0; i < std::max<size_t>(1, count); ++i) {
            ++live_workers;
            workers.emplace_back([this] {
                for (;;) {
                    IOOperationPtr op;
                    {
                        std::unique_lock<std::mutex> lock(work_mutex);
                        wake.wait(lock, [this] { return stopped || !work.empty(); });
                        if (work.empty()) {
                            break;
                        }
                        op = std::move(work.front());
                        work.pop();
                    }
                    if (stopped) {
                        op->success = false;
                        op->error_msg = "Shutdown requested";
                        complete(std::move(op));
                        continue;
                    }
                    switch (op->type) {
                        case IOOperation::Type::FileRead: {
                            auto file_op = std::static_pointer_cast<NariFileOperation>(op);
                            std::error_code error;
                            file_op->success = nari::read_file(file_op->file_path, file_op->result_string, error, &stopped);
                            if (!file_op->success) {
                                file_op->error_msg = "Failed to read file '" + file_op->file_path + "': " + error.message();
                            }
                            break;
                        }

                        case IOOperation::Type::FileWrite:
                        case IOOperation::Type::FileAppend: {
                            auto file_op = std::static_pointer_cast<NariFileOperation>(op);
                            std::error_code error;
                            file_op->success = nari::write_file(
                                file_op->file_path, file_op->file_content.data(), file_op->file_content.size(), error,
                                op->type == IOOperation::Type::FileAppend, &stopped
                            );
                            if (!file_op->success) {
                                file_op->error_msg = "Failed to write file '" + file_op->file_path + "': " + error.message();
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
                                    auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
                                    std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), lower);
                                    std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), lower);
                                    return a_lower < b_lower;
                                });

                                file_op->result_entries = std::move(filenames);
                                file_op->success = true;
                            }
                            break;
                        }
                        default:
                            op->error_msg = "Unsupported file operation";
                            break;
                    }
                    complete(std::move(op));
                }
                --live_workers;
                wake.notify_all();
            });
        }
        // Fatal libc exit paths bypass runtime destructors. 
        // Register after network initialization so workers stop before library/stdio cleanup.
        // process.exit() stops this pool explicitly before calling _exit().
        auto &value = registry();
        std::lock_guard<std::mutex> lock(value.mutex);
        value.pools.push_back(this);
        static const bool registered = [] {
            std::atexit(shutdown_at_exit);
            return true;
        }();
        (void)registered;
    }

    void shutdown() {
        std::call_once(shutdown_once, [this] {
            {
                std::lock_guard<std::mutex> lock(work_mutex);
                stopped = true;
            }
            wake.notify_all();
            events->shutdown();
#ifdef _WIN32
            // A cancellation can race the next native call. Repeat until all
            // workers exit, rather than joining after a single cancel attempt.
            while (live_workers.load() != 0) {
                for (auto &worker : workers) {
                    CancelSynchronousIo(worker.native_handle());
                }
                std::unique_lock<std::mutex> lock(work_mutex);
                wake.wait_for(lock, std::chrono::milliseconds(20), [this] { return live_workers.load() == 0; });
            }
#endif
            for (auto &worker : workers) {
                worker.join();
            }
        });
    }

    void submit(IOOperationPtr op) {
        if (!op) {
            return;
        }
        std::unique_lock<std::mutex> lock(work_mutex);
        if (stopped) {
            op->success = false;
            op->error_msg = "I/O pool is stopped";
            std::lock_guard<std::mutex> completed_lock(completed_mutex);
            completed.push(std::move(op));
            return;
        }
        ++pending;
        switch (op->type) {
            case IOOperation::Type::FileRead:
            case IOOperation::Type::FileWrite:
            case IOOperation::Type::FileAppend:
            case IOOperation::Type::FileExists:
            case IOOperation::Type::FileDelete:
            case IOOperation::Type::ListDir:
                work.push(std::move(op));
                lock.unlock();
                wake.notify_one();
                break;
            default:
                events->submit(std::move(op));
                break;
        }
    }
#endif
    ~Impl() {
        shutdown();
#ifndef NO_THREADS
        auto &value = registry();
        std::lock_guard<std::mutex> lock(value.mutex);
        value.pools.erase(std::remove(value.pools.begin(), value.pools.end(), this), value.pools.end());
#endif
    }
};

IOThreadPool::IOThreadPool(size_t count) : impl(std::make_unique<Impl>(count)) {
}
IOThreadPool::~IOThreadPool() = default;
void IOThreadPool::shutdown() {
    impl->shutdown();
}
void IOThreadPool::submit(IOOperationPtr op) {
    impl->submit(std::move(op));
}
bool IOThreadPool::is_stopped() const {
    return impl->stopped;
}
bool IOThreadPool::has_completed() {
#ifndef NO_THREADS
    std::lock_guard<std::mutex> lock(impl->completed_mutex);
#endif
    return !impl->completed.empty();
}
bool IOThreadPool::has_pending() {
#ifndef NO_THREADS
    if (impl->pending != 0) {
        return true;
    }
#endif
    return has_completed();
}
IOOperationPtr IOThreadPool::pop_completed() {
#ifndef NO_THREADS
    std::lock_guard<std::mutex> lock(impl->completed_mutex);
#endif
    if (impl->completed.empty()) {
        return nullptr;
    }
    auto op = std::move(impl->completed.front());
    impl->completed.pop();
    return op;
}
