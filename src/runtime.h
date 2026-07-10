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

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace nari;

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
inline std::atomic<bool> g_shutdown_requested{ false };
// global flag to signal runtime error occurred and execution should halt
inline std::atomic<bool> g_runtime_error_occurred{ false };
// reset the runtime error flag (useful for running multiple scripts or REPL scenarios)
inline void reset_runtime_error_flag() {
    g_runtime_error_occurred.store(false);
}

void run_program_with_runtime(FuncList &funcs, int argc = 0, char **argv = nullptr);

bool runtime_trace_enabled();
void set_runtime_trace_level(TraceLevel level);
TraceLevel get_runtime_trace_level();
void runtime_log(TraceLevel level, const std::string &msg);

} // namespace Runtime

// conditional builtin lists based on feature availability
#ifndef DISABLE_HTTP
#define BUILTIN_HTTP_LIST(X)                          \
    X("__net_createServer", builtin_net_createServer) \
    X("__net_conn_read", builtin_net_conn_read)       \
    X("__net_conn_write", builtin_net_conn_write)     \
    X("__net_conn_close", builtin_net_conn_close)     \
    X("__net_connect", builtin_net_connect)           \
    X("__net_listen", builtin_net_listen)             \
    X("__net_accept", builtin_net_accept)             \
    X("__net_server_close", builtin_net_server_close) \
    X("__udp_bind", builtin_udp_bind)                 \
    X("__udp_send", builtin_udp_send)                 \
    X("__udp_recv", builtin_udp_recv)                 \
    X("__udp_close", builtin_udp_close)               \
    X("__http_fetch", builtin_http_fetch)             
#else
#define BUILTIN_HTTP_LIST(X)
#endif

#ifndef NARI_ESP_IDF
#define BUILTIN_READLINE_LIST(X)    \
    X("read_line", builtin_readLine) \
    X("read_all", builtin_readAll)
#else
#define BUILTIN_READLINE_LIST(X)
#endif

#ifndef DISABLE_FFI
#define BUILTIN_FFI_LIST(X)                                 \
    X("__ffi_load_library", builtin_ffi_load_library)       \
    X("__ffi_get_symbol", builtin_ffi_get_symbol)           \
    X("__ffi_call", builtin_ffi_call)                       \
    X("__ffi_membersof", builtin_ffi_membersof)             \
    X("__ffi_alloc", builtin_ffi_alloc)                     \
    X("__ffi_alloc_struct", builtin_ffi_alloc_struct)       \
    X("__ffi_read_struct", builtin_ffi_read_struct)         \
    X("__ffi_write_struct", builtin_ffi_write_struct)       \
    X("__ffi_sizeof", builtin_ffi_sizeof)                   \
    X("__ffi_utf16", builtin_ffi_utf16)                     \
    X("__ffi_utf16_read", builtin_ffi_utf16_read)           \
    X("__ffi_free", builtin_ffi_free)                       \
    X("__ffi_create_callback", builtin_ffi_create_callback) \
    X("__ffi_free_callback", builtin_ffi_free_callback)
#else
#define BUILTIN_FFI_LIST(X)
#endif

#define BUILTIN_FUNCTIONS(X)                                        \
    X("__system_exec", builtin_system_exec)                         \
    X("print", builtin_print)                                       \
    X("set_timeout", builtin_setTimeout)                             \
    X("__math_floor", builtin_math_floor)                           \
    X("__math_ceil", builtin_math_ceil)                             \
    X("__math_sqrt", builtin_math_sqrt)                             \
    X("__math_rand", builtin_math_rand)                             \
    X("__math_sin", builtin_math_sin)                               \
    X("__math_cos", builtin_math_cos)                               \
    X("__math_tan", builtin_math_tan)                               \
    X("__math_log", builtin_math_log)                               \
    X("__math_exp", builtin_math_exp)                               \
    X("__math_atan", builtin_math_atan)                             \
    X("__math_atan2", builtin_math_atan2)                           \
    X("__fs_readFile", builtin_fs_readFile)                         \
    X("__fs_writeFile", builtin_fs_writeFile)                       \
    X("__fs_appendFile", builtin_fs_appendFile)                     \
    X("__fs_fileExists", builtin_fs_fileExists)                     \
    X("__fs_isDirectory", builtin_fs_isDirectory)                   \
    X("__fs_mkdirAll", builtin_fs_mkdirAll)                         \
    X("__fs_deleteFile", builtin_fs_deleteFile)                     \
    X("__fs_listDir", builtin_fs_listDir)                           \
    X("__platform_arch", builtin_platform_arch)                     \
    X("__platform_os", builtin_platform_os)                         \
    X("__platform_endianness", builtin_platform_endianness)         \
    X("__platform_hostname", builtin_platform_hostname)             \
    X("__platform_getenv", builtin_platform_getenv)                 \
    X("__process_exit", builtin_process_exit)                       \
    X("__process_argc", builtin_process_argc)                       \
    X("__process_argv", builtin_process_argv)                       \
    X("set_interval", builtin_setInterval)                           \
    X("clear_interval", builtin_clearInterval)                       \
    BUILTIN_READLINE_LIST(X)                                        \
    BUILTIN_HTTP_LIST(X)                                            \
    X("__yield", builtin_yield)                                     \
    X("__shutdown_requested", builtin_shutdown_requested)           \
    X("typeof", builtin_typeof)                                     \
    X("to_number", builtin_toNumber)                                 \
    X("to_string", builtin_toString)                                 \
    X("__format_value", builtin_formatValue)                        \
    X("to_bool", builtin_toBool)                                     \
    X("is_number", builtin_isNumber)                                 \
    X("is_string", builtin_isString)                                 \
    X("is_bool", builtin_isBool)                                     \
    X("is_array", builtin_isArray)                                   \
    X("is_object", builtin_isObject)                                 \
    X("is_function", builtin_isFunction)                             \
    X("Delegate", builtin_delegate_new)                             \
    X("is_delegate", builtin_isDelegate)                             \
    X("delegate_target", builtin_delegateTarget)                     \
    X("delegate_handler", builtin_delegateHandler)                   \
    X("time", builtin_time)                                         \
    BUILTIN_FFI_LIST(X)                                             \
    X("__gc_collect", builtin_gc_collect)                           \
    X("__gc_stats", builtin_gc_stats)                               \
    X("__gc_enable", builtin_gc_enable)                             \
    X("__gc_set_threshold", builtin_gc_set_threshold)               \
    X("__gc_set_memory_limit", builtin_gc_set_memory_limit)         \
    X("__gc_get_memory_usage", builtin_gc_get_memory_usage)         \
    X("parse_int", builtin_parseInt)                                 \
    X("parse_float", builtin_parseFloat)                             \
    X("random", builtin_random)                                     \
    X("range", builtin_range)                                       \
    X("contains", builtin_contains)                                 \
    X("__json_parse", builtin_json_parse)                           \
    X("__json_stringify", builtin_json_stringify)                   \
    X("__hash_sha256", builtin_hash_sha256)                         \
    X("__hash_sha256_file", builtin_hash_sha256_file)               \
    X("__archive_list", builtin_archive_list)                       \
    X("__archive_extract", builtin_archive_extract)                 \
    X("__archive_create", builtin_archive_create)                   \
    X("__module_import_namespace", builtin_module_import_namespace) \
    X("__module_import_named", builtin_module_import_named)         \
    X("from_char_code", builtin_fromCharCode)                         \
    X("__hex_encode", builtin_hex_encode)                           \
    X("__hex_decode", builtin_hex_decode)                           \
    X("__base64_encode", builtin_base64_encode)                     \
    X("__base64_decode", builtin_base64_decode)                     \
    X("__regex_new", builtin_regex_new)                             \
    X("__time_now_ms", builtin_time_now_ms)                         \
    X("__time_components", builtin_time_components)                 \
    X("__time_from_components", builtin_time_from_components)       \
    X("__time_format", builtin_time_format)                         \
    X("__time_parse_iso", builtin_time_parse_iso)                   \
    X("__url_encode", builtin_url_encode)                           \
    X("__url_decode", builtin_url_decode)

// methods that are only accessible via type.method() syntax
#define string_methods(X)                 \
    X("substr", builtin_substr)           \
    X("last_index_of", builtin_lastIndexOf) \
    X("split", builtin_split)             \
    X("replace", builtin_replace)         \
    X("replace_all", builtin_replaceAll)   \
    X("trim", builtin_trim)               \
    X("trim_start", builtin_trimStart)     \
    X("trim_end", builtin_trimEnd)         \
    X("to_upper", builtin_toUpper)         \
    X("to_lower", builtin_toLower)         \
    X("starts_with", builtin_startsWith)   \
    X("ends_with", builtin_endsWith)       \
    X("char_at", builtin_charAt)           \
    X("char_code_at", builtin_charCodeAt)   \
    X("pad_start", builtin_padStart)       \
    X("pad_end", builtin_padEnd)           \
    X("repeat", builtin_repeat)           \
    X("at", builtin_string_at)            \
    X("to_char_array", builtin_toCharArray)

#define array_methods(X)              \
    X("push", builtin_push)           \
    X("pop", builtin_pop)             \
    X("slice", builtin_slice)         \
    X("concat", builtin_concat)       \
    X("join", builtin_join)           \
    X("sort", builtin_sort)           \
    X("map", builtin_map)             \
    X("filter", builtin_filter)       \
    X("reduce", builtin_reduce)       \
    X("find", builtin_find)           \
    X("find_index", builtin_findIndex) \
    X("reverse", builtin_reverse)     \
    X("every", builtin_every)         \
    X("some", builtin_some)           \
    X("for_each", builtin_forEach)     \
    X("splice", builtin_splice)       \
    X("fill", builtin_fill)           \
    X("flat", builtin_flat)           \
    X("flat_map", builtin_flatMap)

#define object_methods(X)         \
    X("keys", builtin_keys)       \
    X("values", builtin_values)   \
    X("has_key", builtin_hasKey)   \
    X("entries", builtin_entries) \
    X("assign", builtin_assign)   \
    X("freeze", builtin_freeze)   \
    X("is_frozen", builtin_isFrozen)

#define regex_methods(X)          \
    X("test", builtin_regex_test) \
    X("exec", builtin_regex_exec)

// methods that work on multiple types
#define universal_methods(X)        \
    X("length", builtin_length)     \
    X("includes", builtin_includes) \
    X("index_of", builtin_indexOf)

// combined method table
#define METHOD_ONLY_BUILTINS(X) string_methods(X) array_methods(X) object_methods(X) regex_methods(X) universal_methods(X)

class ScriptRuntime {
    friend class nari::bytecode::VM;

  public:
    // external GC roots provider (set by bytecode VM to include its state)
    std::function<void(std::vector<const Value *> &)> external_gc_roots_provider;

    // temporary GC roots for C++ locals in builtins that hold heap Values across a nested callback
    std::vector<const Value *> gc_extra_value_roots;
    std::vector<const std::vector<Value> *> gc_extra_vec_roots;
    struct GcTempRoot {
        ScriptRuntime &rt;
        size_t vbase, cbase;
        explicit GcTempRoot(ScriptRuntime &r)
            : rt(r), vbase(r.gc_extra_value_roots.size()),
              cbase(r.gc_extra_vec_roots.size()) {
        }
        void add(const Value *v) {
            rt.gc_extra_value_roots.push_back(v);
        }
        void add_vec(const std::vector<Value> *c) {
            rt.gc_extra_vec_roots.push_back(c);
        }
        ~GcTempRoot() {
            rt.gc_extra_value_roots.resize(vbase);
            rt.gc_extra_vec_roots.resize(cbase);
        }
        GcTempRoot(const GcTempRoot &) = delete;
        GcTempRoot &operator=(const GcTempRoot &) = delete;
    };

    // Async callback roots: a pending I/O op captures Values (callbacks) in a
    // lambda that lives in the io_pool's queues, out of reach of the normal root
    // scan. Without RC, the captured function/object would be swept while the op
    // is in flight. We keep them alive here, keyed by the op pointer. Both the
    // register (at submit) and release (at completion in run_event_loop) happen
    // on the MAIN thread; io threads only perform socket I/O and never touch
    // Values, so this map needs no locking. Scanned by collect_gc_roots.
    std::unordered_map<const void *, std::vector<Value>> gc_async_roots;
    void async_root_set(const void *key, std::vector<Value> vals) {
        gc_async_roots[key] = std::move(vals);
    }
    void async_root_clear(const void *key) {
        gc_async_roots.erase(key);
    }

    // Persistent GC roots that outlive any single async op: a TCP server's
    // handler function is held by a long-lived accept thread (which
    // only copies the Value's raw bits and never dereferences it off-thread), so
    // it must stay alive for the server's lifetime. Registered on the main thread
    // at server creation; scanned by collect_gc_roots; never auto-cleared.
    std::vector<Value> gc_persistent_roots;
    void persistent_root_add(const Value &v) {
        gc_persistent_roots.push_back(v);
    }

    // callback for dispatching function calls to the bytecode VM (for FFI callbacks)
    std::function<Value(const Value &, const std::vector<Value> &)> external_call_function_value;
    // Optional stdout sink used by the DAP server to surface script output.
    std::function<void(const std::string &)> stdout_writer;
    // Optional statement hook used by the bytecode debugger for AST-dispatched code.
    std::function<void(const Stmt *)> debug_stmt_hook;

    // Raise a script-catchable throw from a builtin. Sets flags.throw_flag +
    // throw_value; runtime checks at every statement boundary. Use this for
    // user-actionable errors instead of runtime_fatal (which is uncatchable).
    // Callers that hold non-RAII resources must release them before return.
    Value script_throw(std::string msg) {
        flags.throw_value = Value::make_string(std::move(msg));
        flags.throw_flag = true;
        return Value::none();
    }

    // Inspect / consume a pending script_throw. Used by the JIT call helpers to
    // deliver a builtin's throw to an enclosing try/catch (the interpreter uses
    // flags.throw_flag directly via push_builtin_result).
    bool has_pending_throw() const {
        return flags.throw_flag;
    }
    Value take_pending_throw() {
        Value v = flags.throw_value;
        flags.throw_flag = false;
        flags.throw_value = Value::none();
        return v;
    }

    ScriptRuntime(FuncList &funcs, int argc = 0, char **argv = nullptr)
        : process_argc(argc) {
        if (argv) {
            for (int i = 0; i < argc; ++i) {
                process_argv.push_back(argv[i]);
            }
        }
#ifndef NO_THREADS
        io_pool = std::make_unique<IOThreadPool>(4);
#endif

        for (auto &f : funcs) {
            if (f) {
                // preserve insertion order for top-level functions
                if (f->name.find("__top_level__@") == 0) {
                    toplevel_order.push_back(f->name);
                }
                functions[f->name] = std::move(f);
            }
        }

        auto stdlibInit = functions.find("__stdlib_init__");
        if (stdlibInit != functions.end()) {
            call_user_function(stdlibInit->second.get(), {});
        }
        auto init = functions.find("__init__");
        if (init != functions.end()) {
            call_user_function(init->second.get(), {});
        }
    }

    ~ScriptRuntime() {
        // First, shut down the io_pool to set the stop flag
        // This causes poll() loops in workers to exit on their next timeout check
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

    void run_start(bool found_toplevel);
    void run_top_level();
    void step_task(HandlePtr handle);
    void run_event_loop();

    void push_block_scope() {
        block_scope_stack.emplace_back();
    }

    void pop_block_scope() {
        if (!block_scope_stack.empty()) {
            block_scope_stack.pop_back();
        }
    }

    Value eval_expr(const Expr *e);
    void exec_stmt(const Stmt *s);
    Value call_user_function(Function *f, const std::vector<Value> &args);

    Value lookup_variable(const std::string &name, const std::string &filename, bool &found);
    void store_variable(const std::string &name, const std::string &filename, const Value &value);
    const std::map<std::string, Value> *debug_call_stack_frame(size_t index) const {
        if (index >= call_stack.size()) {
            return nullptr;
        }
        return &call_stack[index];
    }

    // call a function Value (for FFI callbacks)
    Value call_function_value(const Value &func_val, const std::vector<Value> &args) {
        if (!func_val.is_function()) {
            return Value::none();
        }
        const auto &fn = func_val.get_function();

        // Try direct pointer first (for lambdas)
        if (fn.func_ptr) {
            return call_user_function(fn.func_ptr.get(), args);
        }

        // Fall back to global map lookup (for named functions)
        auto it = functions.find(fn.name);
        if (it != functions.end()) {
            return call_user_function(it->second.get(), args);
        }

        // Fall back to external handler (bytecode VM)
        if (external_call_function_value) {
            return external_call_function_value(func_val, args);
        }
        return Value::none();
    }

    // --- Delegate (Proxy-like) trap dispatch --------------------------------
    // Shared by all three execution tiers (AST interpreter, bytecode VM, JIT
    // helpers). Each is entered only after a value has been confirmed to be a
    // Delegate (heap tag 11), so the object shape / property inline caches on
    // the fast paths are never touched. When the corresponding handler trap is
    // absent, the operation falls through to the wrapped target's normal
    // behavior. Handler traps are invoked via call_function_value (the same
    // path map/filter use), so they work identically under the AST interpreter
    // and the VM. All heap Values held across the trap call (a safe-point) are
    // rooted with GcTempRoot.
    Value delegate_get(const Value &del, const Value &key);
    void delegate_set(const Value &del, const Value &key, const Value &val);
    bool delegate_has(const Value &del, const Value &key);
    Value delegate_call(const Value &del, const std::vector<Value> &args);
    Value delegate_call_method(const Value &del, const std::string &method,
                               std::vector<Value> args);

    // Reusable scratch storage for the fixed-arity trap argument lists
    // (get/has: {target,key}; set: {target,key,val}; call: {target,argsArr}).
    // Building a fresh std::vector<Value> per trap call heap-allocs+frees its
    // backing store every invocation (~3.4% of proxy_delegate cycles in
    // operator new[]/delete[]). invoke_trap() reuses this one buffer instead:
    // the callee (call_user_function) copies the args onto the VM stack / into
    // AST locals BEFORE executing any script, so the buffer is dead by the time
    // a trap body could re-enter another delegate op. trap_scratch_busy_ guards
    // the (benchmark-absent, chained-delegate) re-entrant case: if the buffer is
    // still borrowed, we fall back to a fresh heap vector, so correctness never
    // depends on the copy-before-execute invariant.
    std::vector<Value> trap_scratch_;
    bool trap_scratch_busy_ = false;
    Value invoke_trap(const Value &trap, const Value &a0, const Value &a1);
    Value invoke_trap(const Value &trap, const Value &a0, const Value &a1,
                      const Value &a2);
    // Default (no-trap) behavior against the wrapped target; also used directly
    // by the trap dispatchers.
    Value delegate_default_get(const Value &target, const Value &key);
    void delegate_default_set(const Value &target, const Value &key, const Value &val);
    bool delegate_default_has(const Value &target, const Value &key);

    // pattern matching helper
    bool match_pattern(const Pattern *pattern, const Value &value, Value &bindings);

    // garbage collection
    void collect_garbage();
    std::vector<const Value *> collect_gc_roots() const;

    std::unique_ptr<IOThreadPool> get_io_pool() {
        return std::move(io_pool);
    }

    // Extension builtin registration. Call register_extension(name, fn)
    // BEFORE constructing the VM; the name becomes a regular Nari builtin.
    using ExtBuiltinFn = Value (*)(const Value *, size_t);
    static void register_extension(const std::string &name, ExtBuiltinFn fn) {
        get_extension_table()[name] = fn;
    }

    // Pointer to a builtin member function. Public so the VM can cache resolved
    // builtins per call site (method inline cache).
    using BuiltinFn = Value (ScriptRuntime::*)(const Value *, size_t, const CallExpr *);

    // Resolve a builtin name to its member-function pointer (global table first,
    // then method-only table). Returns nullptr if `name` is not a member builtin
    // (e.g. an extension builtin, which keeps using the slow path). Lets the VM
    // build a per-name_idx inline cache that skips the per-call hash lookups.
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

    // Invoke a previously-resolved builtin member pointer directly.
    Value call_builtin_member(BuiltinFn fn, const Value *argv, size_t argc,
                              const CallExpr *callExpr) {
        return (this->*fn)(argv, argc, callExpr);
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
        static const BuiltinFnList table = {
            METHOD_ONLY_BUILTINS(BUILTIN_NARI_ENTRY)
        };
        return table;
    }

    bool is_builtin_name(const std::string &name) const {
        return get_global_builtin_table().count(name) || get_method_builtin_table().count(name) || get_extension_table().count(name);
    }

    bool is_global_builtin(const std::string &name) const {
        return get_global_builtin_table().count(name) > 0 || get_extension_table().count(name) > 0;
    }

#define NARI_ENTRY(bname, method) bname,

#define GET_TYPE_METHODS(typename)                                             \
    static const std::unordered_set<std::string> &get_##typename##_methods() { \
        static const std::unordered_set<std::string> names = {                 \
            typename##_methods(NARI_ENTRY)                                     \
        };                                                                     \
        return names;                                                          \
    }

    // Type-specific method tables for validation
    GET_TYPE_METHODS(string);
    GET_TYPE_METHODS(array);
    GET_TYPE_METHODS(object);
    GET_TYPE_METHODS(regex);
    GET_TYPE_METHODS(universal);

    // check if a method is valid for a given value type
    bool is_method_valid_for_type(const std::string &method_name, const Value &obj) const {
        // universal methods work on strings, arrays, and objects
        if (get_universal_methods().count(method_name)) {
            return obj.is_string() || obj.is_array() || obj.is_object();
        }

        if (get_string_methods().count(method_name)) {
            return obj.is_string();
        }
        if (get_array_methods().count(method_name)) {
            return obj.is_array();
        }
        if (get_object_methods().count(method_name)) {
            return obj.is_object();
        }
        if (get_regex_methods().count(method_name)) {
            return obj.is_regex();
        }

        return false;
    }

    // call_builtin: raw pointer + count, zero heap allocation.
    Value call_builtin(const std::string &name, const Value *argv, size_t argc, const CallExpr *callExpr) {
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

    // Backward-compat wrapper for AST interpreter call sites.
    Value call_builtin(const std::string &name, const std::vector<Value> &argvals, const CallExpr *callExpr) {
        return call_builtin(name, argvals.data(), argvals.size(), callExpr);
    }

    std::unordered_map<std::string, std::unique_ptr<Function>> functions;
    std::vector<std::string> toplevel_order; // preserves import execution order
    std::vector<std::map<std::string, Value>> call_stack;
    std::map<std::string, Value> globals;
    std::unordered_map<std::string, std::map<std::string, Value>> module_local_vars;
    std::unordered_set<std::string> executed_toplevel_modules;
    std::vector<std::string> module_stack;
    std::vector<std::string> func_stack;
    std::vector<std::map<std::string, Value>> block_scope_stack;
    Flags flags;
    int ast_try_depth = 0; // nesting depth of try blocks; TCO disabled when > 0

    std::shared_ptr<std::map<std::string, Value>> current_scope_closure;
    ClassInstancePtr current_instance; // current 'this' context
    std::string current_class_name;    // class name context for access control
    std::queue<HandlePtr> task_queue;
    std::unique_ptr<IOThreadPool> io_pool;
    std::unordered_map<int64_t, IntervalData> active_intervals;
    int64_t next_interval_id = 1;

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

    bool has_pending_io() {
        if (!io_pool) {
            return !active_intervals.empty();
        }
        return io_pool->has_pending() || !active_intervals.empty();
    }

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

    void execute_toplevel_function(Function *f);
    void ensure_module_loaded(const std::string &module_name, const ASTNode *site = nullptr);

    Value builtin_print(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_setTimeout(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_setInterval(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_clearInterval(const Value *argvals, size_t argc, const CallExpr *);

    Value builtin_math_floor(const Value *, size_t, const CallExpr *);
    Value builtin_math_ceil(const Value *, size_t, const CallExpr *);
    Value builtin_math_sqrt(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_rand(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_sin(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_cos(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_tan(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_log(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_exp(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_atan(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_math_atan2(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_readFile(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_writeFile(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_appendFile(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_fileExists(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_isDirectory(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_mkdirAll(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_deleteFile(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fs_listDir(const Value *argvals, size_t argc, const CallExpr *);

#ifndef DISABLE_HTTP
    Value builtin_net_createServer(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_conn_read(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_conn_write(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_conn_close(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_connect(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_listen(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_accept(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_net_server_close(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_udp_bind(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_udp_send(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_udp_recv(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_udp_close(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_http_get(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_http_fetch(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_http_request(const Value *argvals, size_t argc, const CallExpr *);
#endif
    Value builtin_push(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_pop(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_length(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_slice(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_concat(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_sort(const Value *, size_t, const CallExpr *);
    Value builtin_map(const Value *, size_t, const CallExpr *);
    Value builtin_filter(const Value *, size_t, const CallExpr *);
    Value builtin_reduce(const Value *, size_t, const CallExpr *);
    Value builtin_find(const Value *, size_t, const CallExpr *);
    Value builtin_findIndex(const Value *, size_t, const CallExpr *);
    Value builtin_reverse(const Value *, size_t, const CallExpr *);
    Value builtin_includes(const Value *, size_t, const CallExpr *);
    Value builtin_every(const Value *, size_t, const CallExpr *);
    Value builtin_some(const Value *, size_t, const CallExpr *);
    Value builtin_forEach(const Value *, size_t, const CallExpr *);
    Value builtin_splice(const Value *, size_t, const CallExpr *);
    Value builtin_fill(const Value *, size_t, const CallExpr *);
    Value builtin_flat(const Value *, size_t, const CallExpr *);
    Value builtin_flatMap(const Value *, size_t, const CallExpr *);
    Value builtin_keys(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_values(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_hasKey(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_entries(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_assign(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_freeze(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isFrozen(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_yield(const Value *, size_t, const CallExpr *);
    Value builtin_shutdown_requested(const Value *, size_t, const CallExpr *);
    Value builtin_substr(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_indexOf(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_lastIndexOf(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_split(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_replace(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_replaceAll(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_trim(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toUpper(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toLower(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_startsWith(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_endsWith(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_charAt(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_charCodeAt(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_padStart(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_padEnd(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_repeat(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_trimStart(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_trimEnd(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_string_at(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toCharArray(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_fromCharCode(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_hex_encode(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_hex_decode(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_base64_encode(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_base64_decode(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_join(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_typeof(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_regex_test(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_regex_exec(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_regex_new(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toNumber(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toString(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_formatValue(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_toBool(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isNumber(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isString(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isBool(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isArray(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isObject(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isFunction(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_delegate_new(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_isDelegate(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_delegateTarget(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_delegateHandler(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_module_import_namespace(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_module_import_named(const Value *argvals, size_t argc, const CallExpr *);
#ifndef NARI_ESP_IDF
    Value builtin_readLine(const Value *, size_t, const CallExpr *);
    Value builtin_readAll(const Value *, size_t, const CallExpr *);
#endif
    Value builtin_time(const Value *, size_t, const CallExpr *);
    Value builtin_time_now_ms(const Value *, size_t, const CallExpr *);
    Value builtin_time_components(const Value *, size_t, const CallExpr *);
    Value builtin_time_from_components(const Value *, size_t, const CallExpr *);
    Value builtin_time_format(const Value *, size_t, const CallExpr *);
    Value builtin_time_parse_iso(const Value *, size_t, const CallExpr *);
    Value builtin_url_encode(const Value *, size_t, const CallExpr *);
    Value builtin_url_decode(const Value *, size_t, const CallExpr *);

#ifndef DISABLE_FFI
    Value builtin_ffi_load_library(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_get_symbol(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_call(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_membersof(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_alloc(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_alloc_struct(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_read_struct(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_write_struct(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_sizeof(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_utf16(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_utf16_read(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_free(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_create_callback(const Value *argvals, size_t argc, const CallExpr *);
    Value builtin_ffi_free_callback(const Value *argvals, size_t argc, const CallExpr *);
#endif

    Value builtin_platform_arch(const Value *, size_t, const CallExpr *);
    Value builtin_platform_os(const Value *, size_t, const CallExpr *);
    Value builtin_platform_endianness(const Value *, size_t, const CallExpr *);
    Value builtin_platform_hostname(const Value *, size_t, const CallExpr *);
    Value builtin_platform_getenv(const Value *, size_t, const CallExpr *);

    Value builtin_process_exit(const Value *, size_t, const CallExpr *);
    Value builtin_process_argc(const Value *, size_t, const CallExpr *);
    Value builtin_process_argv(const Value *, size_t, const CallExpr *);

    Value builtin_system_exec(const Value *, size_t, const CallExpr *);

    Value builtin_gc_collect(const Value *, size_t, const CallExpr *);
    Value builtin_gc_stats(const Value *, size_t, const CallExpr *);
    Value builtin_gc_enable(const Value *, size_t, const CallExpr *);
    Value builtin_gc_set_threshold(const Value *, size_t, const CallExpr *);
    Value builtin_gc_set_memory_limit(const Value *, size_t, const CallExpr *);
    Value builtin_gc_get_memory_usage(const Value *, size_t, const CallExpr *);

    // utility builtins (shared by AST interpreter and bytecode VM)

    Value builtin_parseInt(const Value *, size_t, const CallExpr *);
    Value builtin_parseFloat(const Value *, size_t, const CallExpr *);
    Value builtin_random(const Value *, size_t, const CallExpr *);
    Value builtin_range(const Value *, size_t, const CallExpr *);
    Value builtin_contains(const Value *, size_t, const CallExpr *);
    Value builtin_json_parse(const Value *, size_t, const CallExpr *);
    Value builtin_json_stringify(const Value *, size_t, const CallExpr *);

    // cryptographic hashes (mbedtls-backed); see src/builtins/hash.cpp
    Value builtin_hash_sha256(const Value *, size_t, const CallExpr *);
    Value builtin_hash_sha256_file(const Value *, size_t, const CallExpr *);

    // archive read/write (libarchive-backed); see src/builtins/archive.cpp
    Value builtin_archive_list(const Value *, size_t, const CallExpr *);
    Value builtin_archive_extract(const Value *, size_t, const CallExpr *);
    Value builtin_archive_create(const Value *, size_t, const CallExpr *);
};

constexpr size_t MAX_CALL_DEPTH = 1000;

// custom exception for runtime errors
class RuntimeError : public std::runtime_error {
  public:
    RuntimeError(const std::string &msg, const std::string &fname = "", int line = 0, int col = 0)
        : std::runtime_error(msg), filename(fname), line_no(line), col_no(col) {
        // Set global flag to signal runtime should halt execution
        Runtime::g_runtime_error_occurred.store(true);
    }

    std::string filename;
    int line_no;
    int col_no;
};

void runtime_fatal(const std::string &msg, const nari::ASTNode *node = nullptr);
inline void bytecode_runtime_fatal(const std::string &msg, const std::string &fname, int line = 0, int col = 0) {
    fprintf(stderr, "Runtime error: %s\n", fname.c_str());
    fprintf(stderr, "  %s\n", msg.c_str());

    throw RuntimeError(msg, fname, line, col);
}
