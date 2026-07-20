#include "debugger.h"
#include "bytecode.h"
#include "nari_fs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace nari {
namespace dbg {

std::string canonicalise_path(const std::string &in) {
    if (in.empty()) {
        return in;
    }
    std::error_code ec;
    auto p = nari::fs::weakly_canonical(nari::fs::Path(in), ec);
    std::string s = ec ? in : p.generic_string();
    // on Windows weakly_canonical already uses forward slashes via generic_string.
    return s;
}

// optional debug log shared with the DAP server. Enabled by NARI_DAP_LOG.
namespace {
FILE *g_dbg_log = nullptr;
std::mutex g_dbg_log_mu;
bool g_dbg_log_init_done = false;

FILE *dbg_log() {
    if (!g_dbg_log_init_done) {
        g_dbg_log_init_done = true;
        const char *path = std::getenv("NARI_DAP_LOG");
        if (path && *path) {
            g_dbg_log = std::fopen(path, "a");
        }
    }
    return g_dbg_log;
}
} // namespace

void DebugController::set_breakpoints(const std::string &path, std::vector<int> lines) {
    std::lock_guard<std::mutex> lock(this->mtx);
    std::set<int> s(lines.begin(), lines.end());
    if (s.empty()) {
        breakpoints.erase(canonicalise_path(path));
    } else {
        breakpoints[canonicalise_path(path)] = std::move(s);
    }
}

bool DebugController::pending_entry_stop() const {
    std::lock_guard<std::mutex> lock(this->mtx);
    return stop_on_entry && !entry_stop_done;
}

bool DebugController::has_breakpoint(const std::string &path, int line) const {
    if (line <= 0) {
        return false;
    }
    auto it = breakpoints.find(path);
    if (it == breakpoints.end()) {
        return false;
    }
    return it->second.count(line) > 0;
}

StopSnapshot DebugController::snapshot_frames(const bytecode::VM &vm, StopReason reason) const {
    StopSnapshot snap;
    snap.reason = reason;
    snap.frames.reserve(synthetic_frames.size() + vm.frames.size());

    for (auto it = synthetic_frames.rbegin(); it != synthetic_frames.rend(); ++it) {
        FrameSnapshot fs;
        fs.name = it->name;
        fs.source_file = it->source_file;
        fs.line = it->line;
        if (it->runtime_call_stack_index != (size_t)-1 && vm.runtime) {
            const auto *locals = vm.runtime->debug_call_stack_frame(
                it->runtime_call_stack_index);
            if (locals) {
                if (it->has_this) {
                    fs.locals.emplace_back("this", it->this_value);
                }
                for (const auto &[name, value] : *locals) {
                    fs.locals.emplace_back(name, value);
                }
            } else if (it->has_this) {
                fs.locals.emplace_back("this", it->this_value);
            }
        } else if (it->has_this) {
            fs.locals.emplace_back("this", it->this_value);
        }
        snap.frames.push_back(std::move(fs));
    }

    const auto &frames = vm.frames;
    // iterate newest-to-oldest so index 0 is the current frame
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        const auto &fr = *it;
        FrameSnapshot fs;
        if (fr.function) {
            fs.name = fr.function->name.empty() ? std::string("<script>") : fr.function->name;
            fs.source_file = canonicalise_path(fr.function->source_file);
            if (fr.ip) {
                size_t pc_offset = (size_t)(fr.ip - fr.function->code.data());
                fs.line = fr.function->resolve_line(pc_offset);
            }
            fs.meta = fr.function;
        } else {
            fs.name = "<unknown>";
        }
        fs.stack_slot_base = fr.slot_base;
        snap.frames.push_back(std::move(fs));
    }
    return snap;
}

void DebugController::push_synthetic_frame(std::string name, std::string source_file, int line, size_t runtime_call_stack_index, Value this_value) {
    std::lock_guard<std::mutex> lock(this->mtx);
    SyntheticFrame frame;
    frame.name = std::move(name);
    frame.source_file = canonicalise_path(source_file);
    frame.line = line;
    frame.runtime_call_stack_index = runtime_call_stack_index;
    frame.has_this = !this_value.is_none();
    frame.this_value = std::move(this_value);
    synthetic_frames.push_back(std::move(frame));
}

void DebugController::update_synthetic_frame_line(int line) {
    std::lock_guard<std::mutex> lock(this->mtx);
    if (!synthetic_frames.empty()) {
        synthetic_frames.back().line = line;
    }
}

void DebugController::pop_synthetic_frame() {
    std::lock_guard<std::mutex> lock(this->mtx);
    if (!synthetic_frames.empty()) {
        synthetic_frames.pop_back();
    }
}

size_t DebugController::synthetic_frame_depth() const {
    std::lock_guard<std::mutex> lock(this->mtx);
    return synthetic_frames.size();
}

bool DebugController::should_stop(const bytecode::VM &vm, size_t pc, size_t frame_depth, int cur_line, const std::string &cur_file) {
    std::unique_lock<std::mutex> lock(this->mtx);

    // We never stop on instructions that don't have a source mapping, since the top-level `<main>` function has an empty line_map
    const bool has_line = cur_line > 0 && !cur_file.empty();

    if (FILE *f = dbg_log()) {
        std::lock_guard<std::mutex> log_lock(g_dbg_log_mu);
        std::fprintf(f, "[vm] pc=%zu depth=%zu line=%d file='%s' mode=%d\n", pc, frame_depth, cur_line, cur_file.c_str(), this->step_mode);
        std::fflush(f);
    }

    // fire on the first instruction with a real source line
    if (!entry_stop_done && stop_on_entry) {
        if (!has_line) {
            return false;
        }
        entry_stop_done = true;
        return true;
    }
    entry_stop_done = true;

    // don't stop twice for the same source line/frame
    const bool same_pos = (cur_line == last_line) && (cur_file == last_file) && (frame_depth == last_depth);

    switch (this->step_mode) {
        case StepMode::Pause:
            if (!has_line) {
                return false;
            }
            return true;
        case StepMode::In:
            // step-into fires on the first new source line, whatever frame we are now in
            if (!has_line) {
                return false;
            }
            if (same_pos) {
                return false;
            }
            return true;
        case StepMode::Over:
            // frame deeper than the anchor means we've called into something. Run it to completion without stopping
            if (frame_depth > this->step_anchor_depth) {
                return false;
            }
            // stop once we land on a new source line with actual code
            if (!has_line) {
                return false;
            }
            if (same_pos && frame_depth == this->step_anchor_depth) {
                return false;
            }
            return true;
        case StepMode::Out:
            // run until we return to a shallower frame
            // once we're out, keep going until we hit a real source line so the UI has somewhere to point at
            if (frame_depth >= this->step_anchor_depth) {
                return false;
            }
            if (!has_line) {
                return false;
            }
            return true;
        case StepMode::None:
            break;
    }

    // only fire on a new (file, line) to avoid stopping many times per statement.
    if (has_line && !same_pos) {
        auto it = breakpoints.find(cur_file);
        if (it != breakpoints.end() && it->second.count(cur_line)) {
            return true;
        }
    }
    return false;
}

void DebugController::publish_stop_and_wait(StopSnapshot snap) {
    StopListener cb_snap;
    {
        std::unique_lock<std::mutex> lock(this->mtx);
        last_stop = std::move(snap);
        // remember the stop position so step/continue logic doesn't re-stop on the same statement.
        if (!last_stop.frames.empty()) {
            last_line = last_stop.frames[0].line;
            last_file = last_stop.frames[0].source_file;
            last_depth = last_stop.frames.size();
        }
        stopped = true;
        // snapshot the listener under the lock so unregistration is safe.
        cb_snap = stop_listener;
        stopped_cv.notify_all();
    }
    // fire the listener outside the lock so the DAP writer can itself touch the controller (e.g. to read last_snapshot) without deadlocking
    if (cb_snap) {
        cb_snap(last_stop);
    }

    std::unique_lock<std::mutex> lock(this->mtx);
    resume_cv.wait(lock, [&] {
        return !stopped || exited;
    });
}

static void set_step(DebugController *self, StepMode m, size_t anchor_depth) {
}

void DebugController::cmd_continue() {
    std::lock_guard<std::mutex> lock(this->mtx);
    this->step_mode = StepMode::None;
    stopped = false;
    resume_cv.notify_all();
}

void DebugController::cmd_next() {
    std::lock_guard<std::mutex> lock(this->mtx);
    this->step_mode = StepMode::Over;
    this->step_anchor_depth = last_depth;
    stopped = false;
    resume_cv.notify_all();
}

void DebugController::cmd_step_in() {
    std::lock_guard<std::mutex> lock(this->mtx);
    this->step_mode = StepMode::In;
    this->step_anchor_depth = last_depth;
    stopped = false;
    resume_cv.notify_all();
}

void DebugController::cmd_step_out() {
    std::lock_guard<std::mutex> lock(this->mtx);
    this->step_mode = StepMode::Out;
    this->step_anchor_depth = last_depth;
    stopped = false;
    resume_cv.notify_all();
}

void DebugController::cmd_pause() {
    std::lock_guard<std::mutex> lock(this->mtx);
    this->step_mode = StepMode::Pause;
}

void DebugController::notify_exited(int code) {
    ExitListener cb;
    {
        std::lock_guard<std::mutex> lock(this->mtx);
        exited = true;
        exit_code_ = code;
        cb = exit_listener;
        stopped_cv.notify_all();
        resume_cv.notify_all();
    }
    if (cb) {
        cb(code);
    }
}

bool DebugController::wait_for_stop_or_exit() {
    std::unique_lock<std::mutex> lock(this->mtx);
    stopped_cv.wait(lock, [&] { return stopped || exited; });
    return stopped;
}

} // namespace dbg
} // namespace nari
