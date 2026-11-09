// VM call/dispatch helpers (split out of bytecode.cpp).
#include "ast.h"
#include "bytecode.h"
#include "compiler_support.h"
#include "debugger.h"
#include "int_overflow.h"
#include "parser_api.h"
#include "runtime.h"
#ifndef DISABLE_JIT
#include "jit_helpers.h"
#include "jit_tls.h"
#include "trace_jit.h"
#endif
#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

// returns the number of nested FFI callback frames currently active on this thread.
extern "C" int ffi_reentry_depth();

namespace nari {
namespace bytecode {

Value VM::call_function_value_sync(const Value &func_val, const std::vector<Value> &args, const Value *receiver) {
    return call_function_value_span(func_val, args.data(), args.size(), receiver);
}

Value VM::call_function_value_span(const Value &func_val, const Value *args, size_t argc, const Value *receiver) {
    if (!func_val.is_function()) {
        return Value::none();
    }
    const auto &fn = func_val.get_function();

    // Resolve the bytecode function index
    // this runs on every trap invocation so we cache the resolved index on the FunctionData to avoid re-hashing
    uint32_t func_idx;
    if (fn.jit_func_idx >= 0) {
        func_idx = static_cast<uint32_t>(fn.jit_func_idx);
    } else {
        auto it = func_indices.find(fn.name);
        if (it == func_indices.end()) {
            return Value::none();
        }
        fn.jit_func_idx = static_cast<int32_t>(it->second);
        func_idx = it->second;
    }

    size_t saved_frame_depth = frames.size();
    size_t saved_stack_size = stack.size();

    // Pass the closure's captures so the frame owns them before the body runs.
    call_user_function_span(func_idx, args, argc, fn.captures, receiver);

    // Execute instructions until this function returns
    while (frames.size() > saved_frame_depth) {
        if (!execute_instruction()) {
            break;
        }
    }

    Value result = Value::none();
    if (stack.size() > saved_stack_size) {
        result = pop();
    }

    return result;
}

Value VM::call_class_function_sync(uint32_t func_idx, const Value *args, size_t argc, const Value *receiver) {
    size_t saved_frame_depth = frames.size();
    size_t saved_stack_size = stack.size();

    // NOTE: a JIT-compiled callee runs inline and never pushes a frame, so the drain loop
    // below is naturally skipped in that case -- same contract as call_function_value_span.
    call_user_function_span(func_idx, args, argc, {}, receiver);

    while (frames.size() > saved_frame_depth) {
        if (!execute_instruction()) {
            break;
        }
    }

    Value result = Value::none();
    if (stack.size() > saved_stack_size) {
        result = pop();
    }

    return result;
}

bool VM::capture_native_throw(Value error) {
    if (native_catch_stack.empty()) {
        return false;
    }
    NativeCatchBoundary &boundary = native_catch_stack.back();
    boundary.caught = true;
    boundary.error = std::move(error);
    return true;
}

Value VM::call_function_value_catching(const Value &func_val) {
    const size_t saved_frame_depth = frames.size();
    const size_t saved_stack_size = stack.size();
    const size_t saved_try_depth = try_stack.size();
    std::jmp_buf *saved_overflow_jmp = overflow_jmp;

    native_catch_stack.emplace_back();
    std::jmp_buf catch_jmp;
    overflow_jmp = &catch_jmp;
    const int jump_value = setjmp(catch_jmp);

    Value result = Value::none();
    if (jump_value == 0) {
        result = call_function_value_span(func_val, nullptr, 0);
    }

    overflow_jmp = saved_overflow_jmp;
    while (frames.size() > saved_frame_depth) {
        frames.pop_back();
    }
    if (stack.size() >= saved_stack_size) {
        stack.resize(saved_stack_size);
    }
    if (try_stack.size() > saved_try_depth) {
        try_stack.resize(saved_try_depth);
    }

    if (jump_value != 0 && jump_value != 4) {
        native_catch_stack.pop_back();
        if (saved_overflow_jmp) {
            std::longjmp(*saved_overflow_jmp, jump_value);
        }
        has_error = true;
        return Value::none();
    }

    NativeCatchBoundary boundary = std::move(native_catch_stack.back());
    native_catch_stack.pop_back();
    has_error = false;
    if (boundary.caught) {
        ScriptRuntime::GcTempRoot root(*runtime);
        root.add(&boundary.error);
        return runtime->make_err(boundary.error);
    }
    ScriptRuntime::GcTempRoot root(*runtime);
    root.add(&result);
    return runtime->make_ok(result);
}

bool VM::dispatch_throw(Value error) {
    // capture the call stack now, before any frame unwinding, so it can be printed if the exception turns out to be
    // uncaught.
    std::vector<std::string> trace;
    trace.reserve(frames.size());
    for (int fi = static_cast<int>(frames.size()) - 1; fi >= 0; fi--) {
        const CallFrame &frame = frames[fi];
        if (!frame.function) {
            continue;
        }
        const std::string &fn_name =
            (frame.function->name.empty() || frame.function->name == "<main>") ? "<top level>" : frame.function->name;
        size_t pc_offset = static_cast<size_t>((frame.ip - frame.function->code.data()));
        // if ip points one byte past the last consumed byte, back off by 1.
        if (pc_offset > 0) {
            pc_offset--;
        }
        int src_line = frame.function->resolve_line(pc_offset);
        std::string entry = "  at " + fn_name;
        if (!frame.function->source_file.empty()) {
            entry += " (" + frame.function->source_file;
            if (src_line > 0) {
                entry += ":" + std::to_string(src_line);
            }
            entry += ")";
        }
        trace.push_back(std::move(entry));
    }
    while (!try_stack.empty()) {
        TryHandler handler = try_stack.back();
        try_stack.pop_back();

        // basic check for a stale handler, we obviously can't target a frame that no longer exists
        if (handler.frame_depth > frames.size()) {
            fprintf(stderr, "warning: discarding stale try handler (frame_depth=%zu, size=%zu)\n", handler.frame_depth, frames.size());
            continue;
        }

        // unwind call frames back to the frame that installed the handler.
        while (frames.size() > handler.frame_depth) {
            size_t slot_base = current_frame().slot_base;
            frames.pop_back();
            if (!frames.empty()) {
                stack.resize(slot_base);
            }
        }

        // after unwinding, we must have at least one frame to land in.
        if (frames.empty()) {
            fprintf(stderr, "warning: try handler left no call frame; treating as uncaught!\n");
            break;
        }

        // stack_depth must not exceed the current unwound stack size, and we can't grow it here
        if (handler.stack_depth > stack.size()) {
            fprintf(stderr, "warning: try handler stack_depth=%zu > stack.size()=%zu; clamping\n", handler.stack_depth, stack.size());
            handler.stack_depth = stack.size();
        }
        stack.resize(handler.stack_depth);

        // catch_ip must be a valid offset inside the current function
        FunctionMeta *fn = current_function();
        if (!fn || handler.catch_ip >= fn->code.size()) {
            fprintf(
                stderr, "warning: try handler catch_ip=%zu out of range (code.size()=%zu)\n", handler.catch_ip, fn ? fn->code.size() : 0
            );
            break;
        }

        // push error value for the catch block and then jump to the handler.
        VM::push(error);
        ip() = fn->code.data() + handler.catch_ip;
        return true; // caught
    }

    // flush any remaining stale handlers so they cannot cause issues for a top level panic
    try_stack.clear();

    fprintf(stderr, "panic: %s\n", error.to_string().c_str());
    for (const auto &entry : trace) {
        fprintf(stderr, "%s\n", entry.c_str());
    }
    has_error = true;
    return false;
}

void VM::runtime_panic(Value error) {
    // Runtime panics are fatal even inside script try/catch blocks.
    try_stack.clear();
    dispatch_throw(std::move(error));
}

void VM::poll_io() {
    runtime->process_completed_io();
}

void VM::register_chunk_functions(size_t from) {
    for (size_t i = from; i < chunk->functions.size(); i++) {
        FunctionMeta &fmeta = chunk->functions[i];
        fmeta.self_idx = static_cast<uint32_t>(i);
        auto cls = jit_classify_inline(fmeta);
        fmeta.jit_inline_kind = cls.kind;
        fmeta.jit_inline_imm = cls.imm;
        const std::string &name = fmeta.name;
        if (name != "<main>" && !name.empty()) {
            func_indices[name] = i;
            // also register as global so OP_LOAD_GLOBAL can find them
            globals[name] = Value::make_function(name);
            // Populate JIT metadata now so the trace JIT can inline calls to these functions without aborting the trace
            FunctionData &fd = globals[name].get_function();
            fd.jit_func_idx = i;
            fd.jit_locals_count = fmeta.var_names.size();
            fd.jit_meta = &fmeta;
            fd.jit_param_count = static_cast<uint8_t>(fmeta.param_count);
            fd.jit_rest_param_index = static_cast<int8_t>(fmeta.rest_param_index);
            fd.jit_js_undefined_params = fmeta.js_undefined_params;

            fd.jit_inline_kind = fmeta.jit_inline_kind;
            fd.jit_inline_imm = fmeta.jit_inline_imm;
            if (name == "__js_lt") {
                fd.jit_native_kind = 2;
            } else if (name == "__js_gt") {
                fd.jit_native_kind = 3;
            } else if (name == "__js_le") {
                fd.jit_native_kind = 4;
            } else if (name == "__js_ge") {
                fd.jit_native_kind = 5;
            } else if (name == "__js_get_prop") {
                fd.jit_native_kind = 6;
            } else if (name == "__js_set_prop") {
                fd.jit_native_kind = 7;
            } else if (name == "__js_set_prop_static") {
                fd.jit_native_kind = 8;
            } else if (name == "__js_length") {
                fd.jit_native_kind = 9;
            } else if (name == "__js_str_char_code_at") {
                fd.jit_native_kind = 10;
            } else if (name == "__js_invoke") {
                fd.jit_native_kind = 11;
            } else if (name == "__js_loose_eq") {
                fd.jit_native_kind = 12;
            } else if (name == "__js_postinc") {
                fd.jit_native_kind = 13;
            } else if (name == "__js_str_code_point_at") {
                fd.jit_native_kind = 15;
            } else if (name == "__js_add") {
                fd.jit_native_kind = 16;
            } else if (name == "__js_to_string") {
                fd.jit_native_kind = 14;
            }

            std::string local_alias = Parser::get_exported_function_local_name(name);
            if (!local_alias.empty()) {
                globals[local_alias] = globals[name]; // copy with all metadata intact
            }
        }
    }
}

// Backs the eval() builtin: compile into the live chunk, register the new names so
// calls (and later evals) resolve them, run any eval'd module top-levels, then the entry.
Value VM::eval_compile_run(const FuncList &funcs, const std::string &entry_name) {
    if (!chunk) {
        return Value::none();
    }
    const size_t first_new = chunk->functions.size();
    AppendedCode added = compile_bytecode_append(chunk, funcs, entry_name);
    if (!added.ok) {
        return Value::none();
    }
    register_chunk_functions(first_new);

    for (uint32_t idx : added.toplevel_idxs) {
        call_class_function_sync(idx, nullptr, 0, nullptr);
    }
    if (added.entry_idx == UINT32_MAX) {
        return Value::none(); // declarations only
    }
    return call_class_function_sync(added.entry_idx, nullptr, 0, nullptr);
}

} // namespace bytecode
} // namespace nari
