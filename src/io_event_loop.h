#pragma once

#include "io_ops.h"

#include <functional>
#include <memory>

// Timers and nonblocking network operations share one dispatcher. Completion
// callbacks are queued by the pool and invoked only by the runtime thread.
class IOEventLoop {
  public:
    explicit IOEventLoop(std::function<void(IOOperationPtr)> complete);
    ~IOEventLoop();
    void submit(IOOperationPtr op);
    void shutdown();

    IOEventLoop(const IOEventLoop &) = delete;
    IOEventLoop &operator=(const IOEventLoop &) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
