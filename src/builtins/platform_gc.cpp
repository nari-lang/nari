#include "common.h"

Value ScriptRuntime::builtin_platform_arch(const Value *, size_t, const nari::CallExpr *) {
#if defined(__EMSCRIPTEN__)
#ifdef __wasm64__
    return Value::make_string("wasm64");
#else
    return Value::make_string("wasm32");
#endif
#endif

#if defined(__x86_64__) || defined(_M_X64)
    return Value::make_string("x86_64");
#elif defined(__i386) || defined(_M_IX86)
    return Value::make_string("x86");
#elif defined(__aarch64__)
    return Value::make_string("arm64");
#elif defined(__arm__) || defined(_M_ARM)
    return Value::make_string("arm");
#elif defined(__ppc64__) || defined(__PPC64__)
    return Value::make_string("ppc64");
#elif defined(__ppc__) || defined(__PPC__)
    return Value::make_string("ppc");
#else
    return Value::make_string("unknown");
#endif
}

Value ScriptRuntime::builtin_platform_os(const Value *, size_t, const nari::CallExpr *) {
#if defined(__EMSCRIPTEN__)
    return Value::make_string("emscripten");
#endif

#if defined(_WIN32)
    return Value::make_string("windows");
#elif defined(__APPLE__) && defined(__MACH__)
    return Value::make_string("macos");
#elif defined(__linux__)
    return Value::make_string("linux");
#elif defined(__unix__) || defined(__unix)
    return Value::make_string("unix");
#else
    return Value::make_string("unknown");
#endif
}

#ifdef __BYTE_ORDER
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define ENDIAN_STRING "little"
#else
#define ENDIAN_STRING "big"
#endif
#else
#define ENDIAN_STRING (nari::compat::endian::native == nari::compat::endian::little) ? "little" : "big"
#endif

Value ScriptRuntime::builtin_platform_endianness(const Value *, size_t, const nari::CallExpr *) {
    return Value::make_string(ENDIAN_STRING);
}

#ifndef NARI_ESP_IDF
Value ScriptRuntime::builtin_platform_hostname(const Value *, size_t, const nari::CallExpr *) {
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    return Value::make_string(hostname);
}
#else
Value ScriptRuntime::builtin_platform_hostname(const Value *, size_t, const nari::CallExpr *) {
    return Value::make_string("esp32");
}
#endif

Value ScriptRuntime::builtin_platform_getenv(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1 && argvals[0].is_string()) {
        const char *value = getenv(argvals[0].get_string().c_str());
        if (value) {
            return Value::make_string(value);
        }
    }
    return Value::make_string("");
}

Value ScriptRuntime::builtin_process_exit(const Value *argvals, size_t argc, const nari::CallExpr *) {
    int exit_code = 0;
    if (argc >= 1 && argvals[0].is_int()) {
        exit_code = static_cast<int>(argvals[0].get_int());
    }
    std::exit(exit_code);
}

Value ScriptRuntime::builtin_process_argc(const Value *, size_t, const nari::CallExpr *) {
    return Value::make_int(static_cast<int64_t>(this->process_argc));
}

// TODO?: should we cache this? array building can be expensive.
Value ScriptRuntime::builtin_process_argv(const Value *, size_t, const nari::CallExpr *) {
    std::vector<Value> args;
    for (int i = 0; i < this->process_argc; ++i) {
        args.push_back(Value::make_string(this->process_argv[i]));
    }
    return Value::make_array(std::move(args));
}

Value ScriptRuntime::builtin_system_exec(const Value *argvals, size_t argc, const nari::CallExpr *) {
    if (argc >= 1 && argvals[0].is_string()) {
        std::string command = argvals[0].get_string();
        int ret = system(command.c_str());
        return Value::make_int(ret);
    }
    return Value::make_int(-1);
}

// Garbage Collector builtin functions
Value ScriptRuntime::builtin_gc_collect(const Value *, size_t, const CallExpr *) {
    auto &gc = GarbageCollector::instance();

    auto roots = collect_gc_roots();
    size_t collected = gc.force_collect(roots);

    auto stats = gc.get_stats();
    auto result_gc = Value::make_object();
    ObjectObj *gc_oobj = result_gc.get_obj_ptr();

    gc_oobj->set_field("collected", Value::make_int(collected));
    gc_oobj->set_field("tracked", Value::make_int(stats.tracked_count));
    gc_oobj->set_field("totalCollections", Value::make_int(stats.total_collections));
    gc_oobj->set_field("totalCollected", Value::make_int(stats.total_collected));
    gc_oobj->set_field("totalAllocated", Value::make_int(stats.total_allocated));
    gc_oobj->set_field("peakTracked", Value::make_int(stats.peak_tracked));

    return result_gc;
}

Value ScriptRuntime::builtin_gc_stats(const Value *, size_t, const CallExpr *) {
    auto &gc = GarbageCollector::instance();
    auto stats = gc.get_stats();

    auto result_gs = Value::make_object();
    ObjectObj *gs_oobj = result_gs.get_obj_ptr();

    gs_oobj->set_field("tracked", Value::make_int(stats.tracked_count));
    gs_oobj->set_field("allocationCount", Value::make_int(stats.allocation_count));
    gs_oobj->set_field("totalCollections", Value::make_int(stats.total_collections));
    gs_oobj->set_field("totalCollected", Value::make_int(stats.total_collected));
    gs_oobj->set_field("totalAllocated", Value::make_int(stats.total_allocated));
    gs_oobj->set_field("peakTracked", Value::make_int(stats.peak_tracked));
    gs_oobj->set_field("threshold", Value::make_int(stats.collection_threshold));
    gs_oobj->set_field("enabled", Value::make_bool(stats.enabled));

    return result_gs;
}

Value ScriptRuntime::builtin_gc_enable(const Value *argvals, size_t argc, const CallExpr *) {
    auto &gc = GarbageCollector::instance();

    if (argc >= 1 && argvals[0].is_bool()) {
        gc.set_enabled(argvals[0].get_bool());
        return Value::make_bool(true);
    }

    return Value::make_bool(false);
}

Value ScriptRuntime::builtin_gc_set_threshold(const Value *argvals, size_t argc, const CallExpr *) {
    auto &gc = GarbageCollector::instance();

    if (argc >= 1) {
        int64_t threshold = 0;
        if (argvals[0].is_int()) {
            threshold = argvals[0].get_int();
        } else if (argvals[0].is_float()) {
            threshold = static_cast<int64_t>(argvals[0].get_float());
        } else {
            return Value::make_bool(false);
        }

        if (threshold > 0) {
            gc.set_collection_threshold(static_cast<size_t>(threshold));
            return Value::make_bool(true);
        }
    }

    return Value::make_bool(false);
}

// __gc_set_memory_limit(bytes) - set artificial memory limit (0 = unlimited)
// Useful for testing how apps behave under memory constraints
Value ScriptRuntime::builtin_gc_set_memory_limit(const Value *argvals, size_t argc, const CallExpr *) {
    auto &gc = GarbageCollector::instance();

    if (argc >= 1) {
        int64_t limit = 0;
        if (argvals[0].is_int()) {
            limit = argvals[0].get_int();
        } else if (argvals[0].is_float()) {
            limit = static_cast<int64_t>(argvals[0].get_float());
        } else {
            return Value::make_bool(false);
        }

        if (limit >= 0) {
            gc.set_memory_limit(static_cast<size_t>(limit));
            return Value::make_bool(true);
        }
    }

    return Value::make_bool(false);
}

// __gc_get_memory_usage() - Get estimated memory usage in bytes
Value ScriptRuntime::builtin_gc_get_memory_usage(const Value *argvals, size_t argc, const CallExpr *) {
    auto &gc = GarbageCollector::instance();
    return Value::make_int(static_cast<int64_t>(gc.get_estimated_memory_usage()));
}
