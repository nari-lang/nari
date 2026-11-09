#pragma once

#include "core_types.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "gc.h"
#include "io.h"
#ifdef __linux__
#include <malloc.h>
#endif

#include "nari_platform.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// Only the nari names this header uses unqualified. Do not add a blanket
// using-directive here: it leaks into every translation unit including this
// header. Prefer explicit qualification at use sites instead.
using nari::CallExpr;
using nari::Function;

// forward declaration for friend class
namespace nari {
namespace bytecode {
class VM;
}
} // namespace nari

namespace Runtime {

enum class TraceLevel {
    None = 0,  // no tracing
    Error = 1, // only fatal/runtime errors
    Info = 2,  // important runtime events
    Debug = 3  // verbose tracing (enter/exit/stmt traces)
};

// global flag for graceful shutdown on SIGINT/SIGTERM
inline std::atomic<bool> g_shutdown_requested = false;
inline void reset_shutdown_flag() {
    g_shutdown_requested.store(false);
}
// global flag to signal runtime error occurred and execution should halt
inline std::atomic<bool> g_runtime_error_occurred{ false };
// reset the runtime error flag (useful for running multiple scripts or REPL scenarios)
inline void reset_runtime_error_flag() {
    g_runtime_error_occurred.store(false);
}

bool runtime_trace_enabled();
void set_runtime_trace_level(TraceLevel level);
TraceLevel get_runtime_trace_level();
void runtime_log(TraceLevel level, const std::string &msg);

} // namespace Runtime

#include "builtins/builtin_lists.h"

class ScriptRuntime {
    friend class nari::bytecode::VM;

  public:
    // external GC roots provider (set by bytecode VM to include its state)
    std::function<void(std::vector<const Value *> &)> external_gc_roots_provider;

    // temporary GC roots for C++ locals in builtins that hold heap Values across a nested callback
    std::vector<const Value *> gc_extra_value_roots;
    std::vector<const std::vector<Value> *> gc_extra_vec_roots;
    std::vector<const std::map<std::string, Value> *> gc_extra_map_roots;
    struct GcTempRoot {
        ScriptRuntime &rt;
        size_t vbase, cbase, mbase;
        explicit GcTempRoot(ScriptRuntime &r)
            : rt(r), vbase(r.gc_extra_value_roots.size()), cbase(r.gc_extra_vec_roots.size()), mbase(r.gc_extra_map_roots.size()) {
        }
        void add(const Value *v) {
            rt.gc_extra_value_roots.push_back(v);
        }
        void add_vec(const std::vector<Value> *c) {
            rt.gc_extra_vec_roots.push_back(c);
        }
        void add_map(const std::map<std::string, Value> *m) {
            rt.gc_extra_map_roots.push_back(m);
        }
        ~GcTempRoot() {
            rt.gc_extra_value_roots.resize(vbase);
            rt.gc_extra_vec_roots.resize(cbase);
            rt.gc_extra_map_roots.resize(mbase);
        }
        GcTempRoot(const GcTempRoot &) = delete;
        GcTempRoot &operator=(const GcTempRoot &) = delete;
    };

    // Async callback roots: a pending I/O op captures Values (callbacks) in a
    // lambda that lives in the io_pool's queues, out of reach of the normal root scan.
    std::unordered_map<const void *, std::vector<Value>> gc_async_roots;
    void async_root_set(const void *key, std::vector<Value> vals) {
        gc_async_roots[key] = std::move(vals);
    }
    void async_root_clear(const void *key) {
        gc_async_roots.erase(key);
    }

    // Persistent GC roots that outlive any single async op.
    // A TCP server's handler function is held by a long-lived accept thread, so it must stay alive for the server's lifetime.
    std::vector<Value> gc_persistent_roots;

    enum TypeofIdx : size_t {
        TY_NULL = 0,
        TY_INT,
        TY_FLOAT,
        TY_STRING,
        TY_BOOL,
        TY_ARRAY,
        TY_OBJECT,
        TY_FUNCTION,
        TY_REGEX,
        TY_HANDLE,
        TY_COUNT
    };
    std::array<Value, TY_COUNT> typeof_values;

    Value typeof_value(const Value &v) const {
        const uint16_t tag = v.tag_word();
        if (tag == Value::TAG_NONE) {
            return typeof_values[TY_NULL];
        }
        if (tag == Value::TAG_INT) {
            return typeof_values[TY_INT];
        }
        if (tag == Value::TAG_BOOL) {
            return typeof_values[TY_BOOL];
        }
        // is_float() is defined as exactly "not HEAP/INT/BOOL/NONE".
        if (tag != Value::TAG_HEAP) {
            return typeof_values[TY_FLOAT];
        }
        switch (v.heap_ptr()->type_tag) {
            case ValueTag::String:
                return typeof_values[TY_STRING];
            case ValueTag::Array:
                return typeof_values[TY_ARRAY];
            case ValueTag::Object:
                return typeof_values[TY_OBJECT];
            case ValueTag::Function:
                return typeof_values[TY_FUNCTION];
            case ValueTag::Regex:
                return typeof_values[TY_REGEX];
            case ValueTag::Handle:
                return typeof_values[TY_HANDLE];
            case ValueTag::ClassInstance:
                return Value::make_string(v.get_class_instance()->class_name);
            default:
                return typeof_values[TY_NULL];
        }
    }
    void persistent_root_add(const Value &v) {
        gc_persistent_roots.push_back(v);
    }

    // callback for dispatching function calls to the bytecode VM (for FFI callbacks)
    std::function<Value(const Value &, const std::vector<Value> &, const Value *)> external_call_function_value;
    std::function<Value(const Value &, const Value *, size_t, const Value *)> external_call_function_value_span;
    std::function<Value(const Value &)> external_catch_function_value;
    // resolve a global by name from the active execution tier
    std::function<Value(const std::string &)> external_global_lookup;
    // compile+run parsed source in the active execution tier (backs eval())
    std::function<Value(const FuncList &, const std::string &)> external_eval_source;
    std::function<void()> external_before_exit;
    // optional stdout sink used by the DAP server
    std::function<void(const std::string &)> stdout_writer;
    // optional statement hook used by the bytecode debugger

    // Panic from a builtin.
    // throwing is an *uncatchable* panic, since we have no try/catch by design.
    // This should *only* be used for situations where it's literally impossible to recover.
    // Otherwise you should be using Result/Option return values.
    bool has_pending_throw() const {
        return flags.throw_flag;
    }
    Value take_pending_throw() {
        Value v = flags.throw_value;
        flags.throw_flag = false;
        flags.throw_value = Value::none();
        return v;
    }

    ScriptRuntime(int argc = 0, char **argv = nullptr) : process_argc(argc) {
        static constexpr const char *type_names[] = { "null",  "int",    "float",    "string", "bool",
                                                      "array", "object", "function", "regex",  "handle" };
        static_assert(sizeof(type_names) / sizeof(type_names[0]) == TY_COUNT, "type_names must match TypeofIdx");
        for (size_t i = 0; i < typeof_values.size(); ++i) {
            typeof_values[i] = Value::make_const_string(type_names[i]);
        }
        if (argv) {
            for (int i = 0; i < argc; ++i) {
                process_argv.push_back(argv[i]);
            }
        }
#ifndef NO_THREADS
        io_pool = std::make_unique<IOThreadPool>(4);
#endif
    }

    ~ScriptRuntime() {
        // shut down the io_pool to set the stop flag
        // this causes poll() loops in workers to exit on their next timeout check
        if (io_pool) {
            io_pool->shutdown();
        }
#ifndef NO_THREADS
        std::lock_guard<std::mutex> socket_lock(server_sockets_mutex);
        for (int fd : server_sockets) {
            NARI_CLOSE_SOCKET(fd);
        }
        server_sockets.clear();

        std::lock_guard<std::mutex> thread_lock(accept_threads_mutex);
        for (auto &thread : accept_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        accept_threads.clear();
#endif
    }

    // call a function Value (for FFI callbacks)
    Value call_function_value(const Value &func_val, const std::vector<Value> &args, const Value *receiver = nullptr) {
        if (!func_val.is_function()) {
            return Value::none();
        }
        const auto &fn = func_val.get_function();

        (void)fn;
        if (external_call_function_value) {
            return external_call_function_value(func_val, args, receiver);
        }
        return Value::none();
    }

    // same resolution order, but this case passes the args straight through without making a vector<Value>.
    Value call_function_value(const Value &func_val, const Value *args, size_t argc, const Value *receiver = nullptr) {
        if (!func_val.is_function()) {
            return Value::none();
        }
        const auto &fn = func_val.get_function();

        (void)fn;
        if (external_call_function_value_span) {
            return external_call_function_value_span(func_val, args, argc, receiver);
        }
        if (external_call_function_value) {
            return external_call_function_value(func_val, std::vector<Value>(args, args + argc), receiver);
        }
        return Value::none();
    }

    // resolve a global by name from whichever execution tier is active
    Value resolve_global(const std::string &name) {
        if (external_global_lookup) {
            return external_global_lookup(name);
        }
        auto it = globals.find(name);
        if (it != globals.end()) {
            return it->second;
        }
        return Value::none();
    }

    Value make_ok(const Value &value) {
        Value ctor = resolve_global("Ok");
        return ctor.is_function() ? construct_result_variant("Ok", value, ctor) : construct_variant("Ok", "Result", { value }, true);
    }
    Value make_err(const Value &error) {
        Value ctor = resolve_global("Err");
        return ctor.is_function() ? construct_result_variant("Err", error, ctor) : construct_variant("Err", "Result", { error }, true);
    }
    Value make_none() {
        return construct_variant("None", "Option", {}, false);
    }

  private:
    struct ResultMethodTmpl {
        size_t slot = 0;
        std::string name;
        int32_t jit_func_idx = -1;
        uint32_t jit_locals_count = 0;
        nari::bytecode::FunctionMeta *jit_meta = nullptr;
        JitInlineKind jit_inline_kind = JitInlineKind::None;
        int32_t jit_native_kind = 0;
        int64_t jit_inline_imm = 0;
        // ResultMethodTmpl is built with a positional aggregate initializer,
        // so new members must not shift the existing ones.
        uint8_t jit_param_count = 0;
        int8_t jit_rest_param_index = -1;
        bool jit_js_undefined_params = false;
    };
    struct ResultConstructorTmpl {
        Value constructor;
        const ObjectShape *shape = nullptr;
        std::vector<Value> fields;
        std::vector<ResultMethodTmpl> methods;
        size_t data_slot = 0;
        uint32_t shape_version = 0;
        bool initialized = false;
        bool usable = false;
        bool unwrap_returns_payload = false;
    };
    ResultConstructorTmpl ok_template;
    ResultConstructorTmpl err_template;
    Value standard_ok_constructor;
    Value standard_err_constructor;

    Value construct_result_variant(const char *variant, const Value &payload, const Value &constructor);
    bool initialize_result_template(ResultConstructorTmpl &cache, const Value &constructor, const Value &result, const Value &payload);
    // `cache` must outlive the returned object (a runtime-owned template); its
    // address is retained as the instance's lazy-field context (read as const).
    Value instantiate_result_template(ResultConstructorTmpl &cache, const Value &payload);
    static Value make_result_method(void *context, const ObjectObj *obj, uint32_t slot);
    static bool invoke_result_method(void *context, const ObjectObj *obj, uint32_t slot, const Value *args, size_t argc, Value &result);

    Value construct_variant(const char *variant, const char *enum_name, std::vector<Value> args, bool has_data) {
        Value ctor = resolve_global(variant);
        if (ctor.is_function()) {
            if (has_data && !args.empty() && std::strcmp(enum_name, "Result") == 0) {
                return construct_result_variant(variant, args[0], ctor);
            }
            return call_function_value(ctor, args);
        }
        // fallback to building a lite variant object so at least pattern matching keeps working
        // this will have no methods attached
        Value obj = Value::make_object();
        auto *o = obj.get_obj_ptr();
        o->set_field("__variant", Value::make_string(variant));
        o->set_field("__enum", Value::make_string(enum_name));
        if (has_data && !args.empty()) {
            o->set_field("__data", args[0]);
        }
        return obj;
    }

  public:
    Value delegate_get(const Value &del, const Value &key);
    void delegate_set(const Value &del, const Value &key, const Value &val);
    bool delegate_has(const Value &del, const Value &key);
    Value delegate_call(const Value &del, const std::vector<Value> &args);
    Value delegate_call(const Value &del, const Value *args, size_t argc);
    Value delegate_call_method(const Value &del, const std::string &method, std::vector<Value> args);

    // Fixed-arity trap invocation
    //  (get/has: {target,key}; set: {target,key,val}; call: {target,argsArr})
    Value invoke_trap(const Value &trap, const Value &a0, const Value &a1);
    Value invoke_trap(const Value &trap, const Value &a0, const Value &a1, const Value &a2);
    // default (no-trap) behavior against the wrapped target, also used directly by the trap dispatchers.
    Value delegate_default_get(const Value &target, const Value &key);
    void delegate_default_set(Value &target, const Value &key, const Value &val);
    bool delegate_default_has(const Value &target, const Value &key);

    void collect_garbage();
    std::vector<const Value *> collect_gc_roots() const;

    // Extension builtin registration. Call register_extension(name, fn)
    // BEFORE constructing the VM, the name becomes a regular Nari builtin.
    using ExtBuiltinFn = Value (*)(const Value *, size_t);
    static void register_extension(const std::string &name, ExtBuiltinFn fn) {
        get_extension_table()[name] = fn;
    }

    // Pointer to a builtin member function. Public so the VM can cache resolved builtins per call site
    using BuiltinFn = Value (ScriptRuntime::*)(Value *, size_t, const CallExpr *);

    // Resolve a builtin name to its member-function pointer (global table first, then method-only table).
    // Returns nullptr if `name` is not a member builtin (e.g. an extension builtin).
    BuiltinFn lookup_builtin_member(const std::string &name) const {
        const auto &gt = get_global_builtin_table();
        auto it = gt.find(name);
        if (it != gt.end()) {
            return it->second;
        }
        const auto &mt = get_method_builtin_table();
        auto it2 = mt.find(name);
        if (it2 != mt.end()) {
            return it2->second;
        }
        return nullptr;
    }

    // invoke a previously-resolved builtin member pointer directly.
    Value call_builtin_member(BuiltinFn fn, Value *argv, size_t argc, const CallExpr *callExpr) {
        return (this->*fn)(argv, argc, callExpr);
    }

    static std::vector<BuiltinFn> &jit_builtin_table() {
        static std::vector<BuiltinFn> table(1); // slot 0 reserved as the "none" sentinel
        return table;
    }
    static uint16_t intern_jit_builtin(BuiltinFn fn) {
        auto &t = jit_builtin_table();
        t.push_back(fn);
        return static_cast<uint16_t>((t.size() - 1));
    }

  private:
#define BUILTIN_NARI_ENTRY(bname, method) { bname, &ScriptRuntime::method },

    // builtin dispatch via hash lookup instead of linear string comparison
    // builtins take a raw pointer + count
    using BuiltinFnList = std::unordered_map<std::string, BuiltinFn>;
    using ExtBuiltinFnList = std::unordered_map<std::string, ExtBuiltinFn>;

    static ExtBuiltinFnList &get_extension_table() {
        static ExtBuiltinFnList table;
        return table;
    }

    static const BuiltinFnList &get_global_builtin_table() {
        static const BuiltinFnList table = { BUILTIN_FUNCTIONS(BUILTIN_NARI_ENTRY) };
        return table;
    }

    static const BuiltinFnList &get_method_builtin_table() {
        static const BuiltinFnList table = { METHOD_ONLY_BUILTINS(BUILTIN_NARI_ENTRY) };
        return table;
    }

    bool is_builtin_name(const std::string &name) const {
        return get_global_builtin_table().count(name) || get_method_builtin_table().count(name) || get_extension_table().count(name);
    }

#define NARI_ENTRY(bname, method) bname,

#define GET_TYPE_METHODS(typename)                                                                                                         \
    static const std::unordered_set<std::string> &get_##typename##_methods() {                                                             \
        static const std::unordered_set<std::string> names = { typename##_methods(NARI_ENTRY) };                                           \
        return names;                                                                                                                      \
    }

    // type-specific method tables for validation
    GET_TYPE_METHODS(string);
    GET_TYPE_METHODS(array);
    GET_TYPE_METHODS(object);
    GET_TYPE_METHODS(regex);
    GET_TYPE_METHODS(universal);

    // check if a method is valid for a given value type
    // call_builtin: raw pointer + count, zero heap allocation.
    Value call_builtin(const std::string &name, Value *argv, size_t argc, const CallExpr *callExpr) {
        auto &globals_tbl = get_global_builtin_table();
        auto it = globals_tbl.find(name);
        if (it != globals_tbl.end()) {
            return (this->*(it->second))(argv, argc, callExpr);
        }

        auto &methods_tbl = get_method_builtin_table();
        auto it2 = methods_tbl.find(name);
        if (it2 != methods_tbl.end()) {
            return (this->*(it2->second))(argv, argc, callExpr);
        }

        // Fall through to extension builtins (platform/hardware modules).
        auto &ext_tbl = get_extension_table();
        auto it3 = ext_tbl.find(name);
        if (it3 != ext_tbl.end()) {
            return it3->second(argv, argc);
        }

        return Value::none();
    }

    // NOTE: there is intentionally no const-vector overload. All callers pass
    // mutable storage (VM stack slots, stack arrays, local vectors).

    std::unordered_map<std::string, std::unique_ptr<Function>> functions;
    std::map<std::string, Value> globals;
    std::unordered_map<std::string, std::map<std::string, Value>> module_local_vars;
    Flags flags;

    std::unique_ptr<IOThreadPool> io_pool;
    std::unordered_map<int64_t, IntervalData> active_intervals;
    int64_t next_interval_id = 1;
    // set_timeout ops in flight.
    // counted separately from io_pool->has_pending() so the end-of-program timer loop waits for timers
    //  without also waiting on a blocking TcpAccept, which would never complete and would hang every server script.
    int64_t pending_timeouts = 0;

    int process_argc;
    std::vector<std::string> process_argv;

#ifndef NO_THREADS
    // server socket tracking
    std::vector<int> server_sockets;
    std::mutex server_sockets_mutex;

    // accept loop threads
    std::vector<std::thread> accept_threads;
    std::mutex accept_threads_mutex;
#endif

    // Is there timer work still owed to the script? Deliberately narrower than
    // io_pool->has_pending(): see pending_timeouts above.
    bool has_pending_timers() const {
        return pending_timeouts > 0 || !active_intervals.empty();
    }

    // Runs every interval whose deadline has passed.
    void fire_due_intervals();

    void process_completed_io() {
        if (!io_pool) {
            return;
        }

        bool did_any = false;
        while (io_pool->has_completed()) {
            IOOperationPtr op = io_pool->pop_completed();
            if (op && op->callback) {
                // run cb on main thread
                op->callback();
                op->callback = nullptr;     // break retain cycle (e.g. http_op captures itself)
                async_root_clear(op.get()); // drop async keepalive roots for this op
                did_any = true;
            }
        }

        if (did_any) {
            // Run the cycle-collecting GC if the threshold has been reached
            collect_garbage();

#ifdef __linux__
            // ask the os to release freed memory back to the system immediately
            malloc_trim(0);
#endif
        }
    }

    Value builtin_eval(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_print(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_write_stdout(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_write_stderr(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_panic(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_nari_catch(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_nari_invoke_with_this(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_setTimeout(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_setInterval(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_clearInterval(Value *argvals, size_t argc, const CallExpr *);

    Value builtin_math_floor(Value *, size_t, const CallExpr *);
    Value builtin_math_ceil(Value *, size_t, const CallExpr *);
    Value builtin_math_sqrt(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_rand(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_sin(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_cos(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_tan(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_log(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_exp(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_atan(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_atan2(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_readFile(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_writeFile(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_appendFile(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_fileExists(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_isDirectory(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_mkdirAll(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_deleteFile(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_listDir(Value *argvals, size_t argc, const CallExpr *);

#ifndef DISABLE_HTTP
    Value builtin_net_createServer(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_conn_read(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_conn_write(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_conn_close(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_connect(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_listen(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_accept(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_server_close(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_udp_bind(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_udp_send(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_udp_recv(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_udp_close(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_http_get(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_http_fetch(Value *argvals, size_t argc, const CallExpr *);
#endif
    Value builtin_push(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_pop(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_length(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_slice(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_concat(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_sort(Value *, size_t, const CallExpr *);
    Value builtin_map(Value *, size_t, const CallExpr *);
    Value builtin_filter(Value *, size_t, const CallExpr *);
    Value builtin_reduce(Value *, size_t, const CallExpr *);
    Value builtin_find(Value *, size_t, const CallExpr *);
    Value builtin_find_index(Value *, size_t, const CallExpr *);
    Value builtin_reverse(Value *, size_t, const CallExpr *);
    Value builtin_includes(Value *, size_t, const CallExpr *);
    Value builtin_every(Value *, size_t, const CallExpr *);
    Value builtin_some(Value *, size_t, const CallExpr *);
    Value builtin_for_each(Value *, size_t, const CallExpr *);
    Value builtin_splice(Value *, size_t, const CallExpr *);
    Value builtin_fill(Value *, size_t, const CallExpr *);
    Value builtin_flat(Value *, size_t, const CallExpr *);
    Value builtin_flat_map(Value *, size_t, const CallExpr *);
    Value builtin_keys(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_values(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_has_key(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_entries(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_assign(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_freeze(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_is_frozen(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_yield(Value *, size_t, const CallExpr *);
    Value builtin_shutdown_requested(Value *, size_t, const CallExpr *);
    Value builtin_substr(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_index_of(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_last_index_of(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_split(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_replace(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_replace_all(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_trim(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_to_upper(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_to_lower(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_starts_with(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ends_with(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_char_at(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_char_code_at(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_pad_start(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_pad_end(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_repeat(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_trim_start(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_trim_end(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_string_at(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_to_char_array(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_from_char_code(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_hex_encode(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_hex_decode(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_base64_encode(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_base64_decode(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_join(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_typeof(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_regex_test(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_regex_exec(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_regex_new(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toNumber(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_js_toNumber(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toString(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_formatValue(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toBool(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isNumber(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isString(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isBool(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isArray(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isObject(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isFunction(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_delegate_new(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isDelegate(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_delegateTarget(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_delegateHandler(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_module_import_namespace(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_module_import_named(Value *argvals, size_t argc, const CallExpr *);
#ifndef NARI_ESP_IDF
    Value builtin_readLine(Value *, size_t, const CallExpr *);
    Value builtin_readAll(Value *, size_t, const CallExpr *);
#endif
    Value builtin_time(Value *, size_t, const CallExpr *);
    Value builtin_time_now_ms(Value *, size_t, const CallExpr *);
    Value builtin_time_components(Value *, size_t, const CallExpr *);
    Value builtin_time_from_components(Value *, size_t, const CallExpr *);
    Value builtin_time_format(Value *, size_t, const CallExpr *);
    Value builtin_time_parse_iso(Value *, size_t, const CallExpr *);
    Value builtin_url_encode(Value *, size_t, const CallExpr *);
    Value builtin_url_decode(Value *, size_t, const CallExpr *);

#ifndef DISABLE_FFI
    Value builtin_ffi_load_library(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_get_symbol(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_call(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_membersof(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_alloc(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_alloc_struct(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_managed_struct(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_read_struct(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_write_struct(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_sizeof(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_utf16(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_utf16_read(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_free(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_create_callback(Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_free_callback(Value *argvals, size_t argc, const CallExpr *);
#endif

    Value builtin_platform_arch(Value *, size_t, const CallExpr *);
    Value builtin_platform_os(Value *, size_t, const CallExpr *);
    Value builtin_platform_endianness(Value *, size_t, const CallExpr *);
    Value builtin_platform_hostname(Value *, size_t, const CallExpr *);
    Value builtin_platform_getenv(Value *, size_t, const CallExpr *);
    Value builtin_platform_environ(Value *, size_t, const CallExpr *);
    Value builtin_platform_cwd(Value *, size_t, const CallExpr *);
    Value builtin_platform_isatty(Value *, size_t, const CallExpr *);

    Value builtin_process_argc(Value *, size_t, const CallExpr *);
    Value builtin_process_argv(Value *, size_t, const CallExpr *);
    Value builtin_process_exec(Value *, size_t, const CallExpr *);
    Value builtin_process_exit(Value *, size_t, const CallExpr *);

    Value builtin_gc_collect(Value *, size_t, const CallExpr *);
    Value builtin_gc_stats(Value *, size_t, const CallExpr *);
    Value builtin_gc_enable(Value *, size_t, const CallExpr *);
    Value builtin_gc_set_threshold(Value *, size_t, const CallExpr *);
    Value builtin_gc_set_memory_limit(Value *, size_t, const CallExpr *);
    Value builtin_gc_get_memory_usage(Value *, size_t, const CallExpr *);

    // utility builtins (shared by AST interpreter and bytecode VM)

    Value builtin_parse_int(Value *, size_t, const CallExpr *);
    Value builtin_parse_float(Value *, size_t, const CallExpr *);
    Value builtin_random(Value *, size_t, const CallExpr *);
    Value builtin_range(Value *, size_t, const CallExpr *);
    Value builtin_contains(Value *, size_t, const CallExpr *);
    Value builtin_json_parse(Value *, size_t, const CallExpr *);
    Value builtin_json_stringify(Value *, size_t, const CallExpr *);

    // cryptographic hashes (mbedtls-backed); see src/builtins/hash.cpp
    Value builtin_hash_sha1(Value *, size_t, const CallExpr *);
    Value builtin_hash_sha256(Value *, size_t, const CallExpr *);
    Value builtin_hash_sha256_file(Value *, size_t, const CallExpr *);

    // archive read/write (libarchive-backed); see src/builtins/archive.cpp
    Value builtin_archive_list(Value *, size_t, const CallExpr *);
    Value builtin_archive_extract(Value *, size_t, const CallExpr *);
    Value builtin_archive_create(Value *, size_t, const CallExpr *);
};

constexpr size_t MAX_CALL_DEPTH = 1000;

// Fatal runtime errors use status returns, not exceptions. Both helpers below
// print the diagnostic, latch Runtime::g_runtime_error_occurred, and return
// Value::none(). Callers must `return` the result immediately; the VM
// converts the latched flag into has_error at the next builtin boundary.
Value runtime_fatal(const std::string &msg, const nari::ASTNode *node = nullptr);
inline Value bytecode_runtime_fatal(const std::string &msg, const std::string &fname, int line = 0, int col = 0) {
    fprintf(stderr, "Runtime error: %s\n", fname.c_str());
    fprintf(stderr, "  %s\n", msg.c_str());

    Runtime::g_runtime_error_occurred.store(true);
    return Value::none();
}
