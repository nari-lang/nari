#pragma once

#include "core_types.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace nari {
namespace bytecode {
class VM;
struct FunctionMeta;
} // namespace bytecode

namespace dbg {

enum class StepMode {
    None,  // running freely
    Pause, // pause: stop on next instruction
    Over,  // step over: stop on next instruction at same or shallower frame depth
    In,    // step into: stop on next instruction
    Out,   // step out: stop when frame depth becomes < this->step_anchor_depth
};

enum class StopReason {
    Entry, // just before first instruction (launch stopOnEntry)
    Breakpoint,
    Step,
    Pause,
    Exception, // uncaught throw
};

// snapshot of one call frame for DAP stackTrace responses.
struct FrameSnapshot {
    std::string name;                                  // function name (or "<script>" for the top-level)
    std::string source_file;                           // absolute-ish path of the source
    int line = 0;                                      // 1-based source line
    size_t stack_slot_base = 0;                        // index into VM::stack where this frame's locals begin
    const bytecode::FunctionMeta *meta = nullptr;      // for var_names lookup
    std::vector<std::pair<std::string, Value>> locals; // populated for synthetic AST-backed frames
};

// snapshot published when the VM pauses
struct StopSnapshot {
    StopReason reason = StopReason::Entry;
    std::vector<FrameSnapshot> frames; // 0 = innermost (current)
};

class DebugController {
  public:
    struct SyntheticFrame {
        std::string name;
        std::string source_file;
        int line = 0;
        size_t runtime_call_stack_index = (size_t)-1;
        bool has_this = false;
        Value this_value;
    };

    static DebugController &instance() {
        static DebugController inst;
        return inst;
    }

    // when disabled, `maybe_stop()` is a no-op.
    void set_enabled(bool on) {
        this->is_enabled.store(on, std::memory_order_relaxed);
    }
    bool enabled() const {
        return this->is_enabled.load(std::memory_order_relaxed);
    }

    // controls whether the VM pauses before executing the first instruction of the main function
    void set_stop_on_entry(bool on) {
        stop_on_entry = on;
    }
    bool pending_entry_stop() const;

    // set breakpoints for a source file, called from the DAP thread in response to setBreakpoints.
    void set_breakpoints(const std::string &path, std::vector<int> lines);

    // check whether a given (source_file, 1-based line) tuple is a breakpoint.
    bool has_breakpoint(const std::string &path, int line) const;

    // build a StopSnapshot by walking the VM's frames. Called on the VM thread just before it blocks in wait_for_resume().
    StopSnapshot snapshot_frames(const bytecode::VM &vm, StopReason reason) const;

    // if this returns true, the VM should immediately publish_stop() + wait.
    bool should_stop(const bytecode::VM &vm, size_t pc, size_t frame_depth, int cur_line, const std::string &cur_file);

    // publish a stop event with the current state, this blocks the VM thread until DAP resumes
    void publish_stop_and_wait(StopSnapshot snap);

    // true until the first stop fires, VM uses this to tag the first stop as Entry rather than Step.
    bool has_fired_first_stop() const {
        return first_stop_fired.load(std::memory_order_relaxed);
    }
    void mark_first_stop_fired() {
        first_stop_fired.store(true, std::memory_order_relaxed);
    }

    // commands from the DAP thread

    void cmd_continue(); // resume after a stop
    void cmd_next();     // step over
    void cmd_step_in();  // step into
    void cmd_step_out(); // step out
    void cmd_pause();    // pause

    void push_synthetic_frame(
        std::string name,
        std::string source_file,
        int line,
        size_t runtime_call_stack_index = (size_t)-1,
        Value this_value = Value::none());
    void update_synthetic_frame_line(int line);
    void pop_synthetic_frame();
    size_t synthetic_frame_depth() const;

    // polled by the DAP thread (or set by the VM on exit)
    bool is_stopped() const {
        std::lock_guard<std::mutex> lock(this->mtx);
        return stopped;
    }

    // the VM thread calls this as its last action, wakes any DAP thread waiting for a stop event
    void notify_exited(int exit_code);
    bool has_exited() const {
        std::lock_guard<std::mutex> lock(this->mtx);
        return exited;
    }
    int exit_code() const {
        std::lock_guard<std::mutex> lock(this->mtx);
        return this->exit_code_;
    }

    // ownership of the last published snapshot, DAP thread reads this after a stopped event
    StopSnapshot last_snapshot() const {
        std::lock_guard<std::mutex> lock(this->mtx);
        return last_stop;
    }

    // wait until the VM stops or exits.
    // used after `launch` to sync up with the first stopOnEntry / uncaught-throw event
    bool wait_for_stop_or_exit();

    // register a callback fired on every VM-thread stop
    // the DAP server uses this to push a `stopped` event as soon as it's published.
    using StopListener = std::function<void(const StopSnapshot &)>;
    void set_stop_listener(StopListener cb) {
        stop_listener = std::move(cb);
    }

    using ExitListener = std::function<void(int)>;
    void set_exit_listener(ExitListener cb) {
        exit_listener = std::move(cb);
    }

  private:
    DebugController() = default;

    std::atomic<bool> is_enabled{ false };
    std::atomic<bool> first_stop_fired{ false };
    bool stop_on_entry = true;
    bool entry_stop_done = false;

    mutable std::mutex mtx;
    std::condition_variable stopped_cv;
    std::condition_variable resume_cv;
    bool stopped = false;
    bool exited = false;
    int exit_code_ = 0;
    StopSnapshot last_stop;

    StepMode step_mode = StepMode::None;
    size_t step_anchor_depth = 0;
    int last_line = 0;
    std::string last_file;
    size_t last_depth = 0;

    // breakpoints keyed by canonicalised absolute path
    std::unordered_map<std::string, std::set<int>> breakpoints;
    std::vector<SyntheticFrame> synthetic_frames;

    StopListener stop_listener;
    ExitListener exit_listener;
};

// normalise a file path the way both the DAP client and the VM will see it:
// weakly-canonical absolute, with forward slashes on all platforms.
std::string canonicalise_path(const std::string &in);

} // namespace dbg
} // namespace nari
