#pragma once

#include "io_ops.h"

#include <cstddef>
#include <memory>

// File operations use workers; timers and networking use the event dispatcher.
// Include io.h so MCU/Emscripten threading flags are selected consistently.
class IOThreadPool {
  public:
    explicit IOThreadPool(size_t num_threads = 4);
    ~IOThreadPool();
    void shutdown();
    void submit(IOOperationPtr op);
    bool is_stopped() const;
    bool has_completed();
    bool has_pending();
    IOOperationPtr pop_completed();

    IOThreadPool(const IOThreadPool &) = delete;
    IOThreadPool &operator=(const IOThreadPool &) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
