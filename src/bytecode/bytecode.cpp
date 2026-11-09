#include "bytecode.h"
#include "ast.h"
#include "compiler_support.h"
#include "debugger.h"
#include "gdb_bridge.h"
#include "int_overflow.h"
#include "parser_api.h"
#include "runtime.h"
#ifndef DISABLE_JIT
#include "jit/runtime/session.h"
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

static const nari::ClassField *bc_find_field(const nari::ClassDecl *class_decl, const std::string &field_name) {
    if (!class_decl) {
        return nullptr;
    }
    for (const auto &field : class_decl->fields) {
        if (field.name == field_name) {
            return &field;
        }
    }
    if (!class_decl->parent_name.empty()) {
        const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
        if (parent) {
            return bc_find_field(parent, field_name);
        }
    }
    return nullptr;
}

VM::VM(int argc, char **argv, bool enable_jit) : chunk(nullptr), gdb_nari_active(gdb_nari_functions_enabled()) {
#ifdef NARI_MCU
    // smaller reserve given less memory on MCUs
    stack.reserve(256);
    frames.reserve(16);
#else
    stack.reserve(1024);
    frames.reserve(128);
#endif

    runtime = std::make_unique<ScriptRuntime>(argc, argv);

    // register GC roots
    runtime->external_gc_roots_provider = [&](std::vector<const Value *> &roots) {
        for (const auto &[key, val] : globals) {
            roots.push_back(&val);
        }
        for (const auto &val : stack) {
            roots.push_back(&val);
        }
        for (const auto &boundary : native_catch_stack) {
            if (boundary.caught) {
                roots.push_back(&boundary.error);
            }
        }
        for (const auto &[key, val] : builtins) {
            roots.push_back(&val);
        }
        if (chunk) {
            for (size_t i = 0; i < chunk->const_string_cache.size() && i < chunk->const_string_valid.size(); i++) {
                if (chunk->const_string_valid[i]) {
                    roots.push_back(&chunk->const_string_cache[i]);
                }
            }
        }
        // prevent cycle-collector from freeing objects the cache points to
        for (size_t i = 0; i < global_cache.size(); i++) {
            if (global_cache_valid[i]) {
                roots.push_back(&global_cache[i]);
            }
        }
        if (!js_undefined_value.is_none()) {
            roots.push_back(&js_undefined_value);
        }
        // root captures, receivers, and open upvalues in all active call frames
        for (const auto &frame : frames) {
            if (!frame.closure_root.is_none()) {
                roots.push_back(&frame.closure_root);
            }
            if (!frame.receiver.is_none()) {
                roots.push_back(&frame.receiver);
            }
            if (frame.captures) {
                for (const auto &cell : *frame.captures) {
                    if (cell) {
                        roots.push_back(cell.get());
                    }
                }
            }
            if (frame.inline_upvalue) {
                roots.push_back(frame.inline_upvalue.get());
            }
            if (frame.open_upvalues) {
                for (const auto &[idx, cell] : *frame.open_upvalues) {
                    if (cell) {
                        roots.push_back(cell.get());
                    }
                }
            }
        }
        // C++ temporaries held across nested execution
        for (const Value *root : gc_temp_roots) {
            if (root) {
                roots.push_back(root);
            }
        }
    };

    register_all_builtins();

#ifndef DISABLE_JIT
    if (enable_jit) {
        jit_session = jit::create_session();
    }
#else
    (void)enable_jit;
#endif

#ifdef NARI_MCU
    // collect more aggressively to keep heap pressure low
    GarbageCollector::instance().set_collection_threshold(100);
#endif
}



} // namespace bytecode
} // namespace nari
