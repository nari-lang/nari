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

#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace nari;

struct HandleData;
using HandlePtr = std::shared_ptr<HandleData>;

namespace Runtime {

enum class TraceLevel {
  None = 0,  // no tracing
  Error = 1, // only fatal/runtime errors
  Info = 2,  // important runtime events
  Debug = 3  // verbose tracing (enter/exit/stmt traces)
};

// global flag for graceful shutdown on SIGINT/SIGTERM
inline std::atomic<bool> g_shutdown_requested{false};
// global flag to signal runtime error occurred and execution should halt
inline std::atomic<bool> g_runtime_error_occurred{false};
// reset the runtime error flag (useful for running multiple scripts or REPL
// scenarios)
inline void reset_runtime_error_flag() {
  g_runtime_error_occurred.store(false);
}

void run_program_with_runtime(std::vector<std::unique_ptr<Function>> &funcs);

bool runtime_trace_enabled();
void set_runtime_trace_level(TraceLevel level);
TraceLevel get_runtime_trace_level();
void runtime_log(TraceLevel level, const std::string &msg);

} // namespace Runtime

// conditional builtin lists based on feature availability
#ifndef NO_OPENSSL
#define BUILTIN_HTTP_LIST(X)                                                   \
  X("__http_get", builtin_http_get)                                            \
  X("__http_request", builtin_http_request)
#else
#define BUILTIN_HTTP_LIST(X)
#endif

#ifndef NO_FFI
#define BUILTIN_FFI_LIST(X)                                                    \
  X("__ffi_load_library", builtin_ffi_load_library)                            \
  X("__ffi_get_symbol", builtin_ffi_get_symbol)                                \
  X("__ffi_call", builtin_ffi_call)                                            \
  X("__ffi_membersof", builtin_ffi_membersof)                                  \
  X("__ffi_alloc", builtin_ffi_alloc)                                          \
  X("__ffi_alloc_struct", builtin_ffi_alloc_struct)                            \
  X("__ffi_read_struct", builtin_ffi_read_struct)                              \
  X("__ffi_write_struct", builtin_ffi_write_struct)                            \
  X("__ffi_sizeof", builtin_ffi_sizeof)                                        \
  X("__ffi_utf16", builtin_ffi_utf16)                                          \
  X("__ffi_utf16_read", builtin_ffi_utf16_read)                                \
  X("__ffi_free", builtin_ffi_free)                                            \
  X("__ffi_create_callback", builtin_ffi_create_callback)                      \
  X("__ffi_free_callback", builtin_ffi_free_callback)
#else
#define BUILTIN_FFI_LIST(X)
#endif

#define BUILTIN_FUNCTIONS(X)                                                   \
  X("__system_exec", builtin_system_exec)                                      \
  X("print", builtin_print)                                                    \
  X("setTimeout", builtin_setTimeout)                                          \
  X("__math_sqrt", builtin_math_sqrt)                                          \
  X("__math_rand", builtin_math_rand)                                          \
  X("__fs_readFile", builtin_fs_readFile)                                      \
  X("__fs_writeFile", builtin_fs_writeFile)                                    \
  X("__fs_appendFile", builtin_fs_appendFile)                                  \
  X("__fs_fileExists", builtin_fs_fileExists)                                  \
  X("__fs_isDirectory", builtin_fs_isDirectory)                                \
  X("__fs_deleteFile", builtin_fs_deleteFile)                                  \
  X("__fs_listDir", builtin_fs_listDir)                                        \
  X("__platform_arch", builtin_platform_arch)                                  \
  X("__platform_os", builtin_platform_os)                                      \
  X("__platform_endianness", builtin_platform_endianness)                      \
  X("__platform_hostname", builtin_platform_hostname)                          \
  X("__platform_getenv", builtin_platform_getenv)                              \
  X("setInterval", builtin_setInterval)                                        \
  X("clearInterval", builtin_clearInterval)                                    \
  X("__net_createServer", builtin_net_createServer)                            \
  X("__net_conn_read", builtin_net_conn_read)                                  \
  X("__net_conn_write", builtin_net_conn_write)                                \
  X("__net_conn_close", builtin_net_conn_close)                                \
  BUILTIN_HTTP_LIST(X)                                                         \
  X("__yield", builtin_yield)                                                  \
  X("__shutdown_requested", builtin_shutdown_requested)                        \
  X("typeof", builtin_typeof)                                                  \
  X("toNumber", builtin_toNumber)                                              \
  X("toString", builtin_toString)                                              \
  X("toBool", builtin_toBool)                                                  \
  X("isNumber", builtin_isNumber)                                              \
  X("isString", builtin_isString)                                              \
  X("isBool", builtin_isBool)                                                  \
  X("isArray", builtin_isArray)                                                \
  X("isObject", builtin_isObject)                                              \
  X("isFunction", builtin_isFunction)                                          \
  X("readLine", builtin_readLine)                                              \
  X("readAll", builtin_readAll)                                                \
  X("time", builtin_time)                                                      \
  BUILTIN_FFI_LIST(X)                                                          \
  X("__gc_collect", builtin_gc_collect)                                        \
  X("__gc_stats", builtin_gc_stats)                                            \
  X("__gc_enable", builtin_gc_enable)                                          \
  X("__gc_set_threshold", builtin_gc_set_threshold)

// methods that are only accessible via type.method() syntax
#define string_methods(X)                                                      \
  X("substr", builtin_substr)                                                  \
  X("indexOf", builtin_indexOf)                                                \
  X("lastIndexOf", builtin_lastIndexOf)                                        \
  X("split", builtin_split)                                                    \
  X("replace", builtin_replace)                                                \
  X("replaceAll", builtin_replaceAll)                                          \
  X("trim", builtin_trim)                                                      \
  X("toUpper", builtin_toUpper)                                                \
  X("toLower", builtin_toLower)                                                \
  X("startsWith", builtin_startsWith)                                          \
  X("endsWith", builtin_endsWith)                                              \
  X("charAt", builtin_charAt)

#define array_methods(X)                                                       \
  X("push", builtin_push)                                                      \
  X("pop", builtin_pop)                                                        \
  X("slice", builtin_slice)                                                    \
  X("concat", builtin_concat)                                                  \
  X("join", builtin_join)

#define object_methods(X)                                                      \
  X("keys", builtin_keys)                                                      \
  X("values", builtin_values)                                                  \
  X("hasKey", builtin_hasKey)

// methods that work on multiple types
#define universal_methods(X) X("length", builtin_length)

// combined method table
#define METHOD_ONLY_BUILTINS(X)                                                \
  string_methods(X) array_methods(X) object_methods(X) universal_methods(X)

class ScriptRuntime {
public:
  ScriptRuntime(std::vector<std::unique_ptr<Function>> &funcs) {
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
      close(fd);
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

  void push_block_scope() { block_scope_stack.emplace_back(); }

  void pop_block_scope() {
    if (!block_scope_stack.empty()) {
      block_scope_stack.pop_back();
    }
  }

  Value eval_expr(const Expr *e);
  void exec_stmt(const Stmt *s);
  Value call_user_function(Function *f, const std::vector<Value> &args);

  // Call a function Value (for FFI callbacks)
  Value call_function_value(const Value &func_val,
                            const std::vector<Value> &args) {
    if (!func_val.is_function()) {
      return Value::none();
    }
    std::string func_name = func_val.get_function().name;
    auto it = functions.find(func_name);
    if (it != functions.end()) {
      return call_user_function(it->second.get(), args);
    }
    return Value::none();
  }

  // Pattern matching helper
  bool match_pattern(const Pattern *pattern, const Value &value,
                     Value &bindings);

  // Garbage collection
  void collect_garbage();
  std::vector<const Value *> collect_gc_roots() const;

  std::unique_ptr<IOThreadPool> get_io_pool() { return std::move(io_pool); }

private:
#define BUILTIN_NARI_ENTRY(bname, method) {bname, &ScriptRuntime::method},

  // builtin dispatch via hash lookup instead of linear string comparison
  using BuiltinFn = Value (ScriptRuntime::*)(const std::vector<Value> &,
                                             const CallExpr *);

  static const std::unordered_map<std::string, BuiltinFn> &
  get_global_builtin_table() {
    static const std::unordered_map<std::string, BuiltinFn> table = {
        BUILTIN_FUNCTIONS(BUILTIN_NARI_ENTRY)};
    return table;
  }

  static const std::unordered_map<std::string, BuiltinFn> &
  get_method_builtin_table() {
    static const std::unordered_map<std::string, BuiltinFn> table = {
        METHOD_ONLY_BUILTINS(BUILTIN_NARI_ENTRY)};
    return table;
  }

  bool is_builtin_name(const std::string &name) const {
    return get_global_builtin_table().count(name) ||
           get_method_builtin_table().count(name);
  }

  bool is_global_builtin(const std::string &name) const {
    return get_global_builtin_table().count(name) > 0;
  }

#define NARI_ENTRY(bname, method) bname,
// typename##_methods expands into something like string_methods(NARI_ENTRY)
// which is an x-macro set of method names for that type, like {"substr",
// &ScriptRuntime::builtin_substr}, etc.
#define GET_TYPE_METHODS(typename)                                             \
  static const std::unordered_set<std::string> &get_##typename##_methods() {   \
    static const std::unordered_set<std::string> names = {                     \
        typename##_methods(NARI_ENTRY)};                                       \
    return names;                                                              \
  }

  // Type-specific method tables for validation
  GET_TYPE_METHODS(string);
  GET_TYPE_METHODS(array);
  GET_TYPE_METHODS(object);
  GET_TYPE_METHODS(universal);

  // Check if a method is valid for a given value type
  bool is_method_valid_for_type(const std::string &method_name,
                                const Value &obj) const {
    // universal methods work on strings, arrays, and objects
    if (get_universal_methods().count(method_name)) {
      return obj.is_string() || obj.is_array() || obj.is_object();
    }

    // string methods
    if (get_string_methods().count(method_name)) {
      return obj.is_string();
    }

    // array methods
    if (get_array_methods().count(method_name)) {
      return obj.is_array();
    }

    // object methods
    if (get_object_methods().count(method_name)) {
      return obj.is_object();
    }

    return false;
  }

  Value call_builtin(const std::string &name, const std::vector<Value> &argvals,
                     const CallExpr *ce) {
    auto &globals_tbl = get_global_builtin_table();
    auto it = globals_tbl.find(name);
    if (it != globals_tbl.end())
      return (this->*(it->second))(argvals, ce);

    auto &methods_tbl = get_method_builtin_table();
    auto it2 = methods_tbl.find(name);
    if (it2 != methods_tbl.end())
      return (this->*(it2->second))(argvals, ce);

    return Value::none();
  }

  std::unordered_map<std::string, std::unique_ptr<Function>> functions;
  std::vector<std::string> toplevel_order; // preserves import execution order
  std::vector<std::unordered_map<std::string, Value>> call_stack;
  std::unordered_map<std::string, Value> globals;
  std::unordered_map<std::string, std::unordered_map<std::string, Value>>
      module_local_vars;
  std::vector<std::string> module_stack;
  std::vector<std::string> func_stack;
  std::vector<std::unordered_map<std::string, Value>> block_scope_stack;
  Flags flags;

  std::shared_ptr<std::unordered_map<std::string, Value>> current_scope_closure;
  ClassInstancePtr current_instance; // current 'this' context for method calls
  std::string current_class_name;    // class name context for access control
  std::queue<HandlePtr> task_queue;
  std::unique_ptr<IOThreadPool> io_pool;
  std::unordered_map<int64_t, IntervalData> active_intervals;
  int64_t next_interval_id = 1;

#ifndef NO_THREADS
  // server socket tracking
  std::vector<int> server_sockets;
  std::mutex server_sockets_mutex;

  // accept loop threads
  std::vector<std::thread> accept_threads;
  std::mutex accept_threads_mutex;
#endif

  bool has_pending_io() {
    if (!io_pool)
      return !active_intervals.empty();
    return io_pool->has_pending() || !active_intervals.empty();
  }

  void process_completed_io() {
    if (!io_pool)
      return;

    while (io_pool->has_completed()) {
      IOOperationPtr op = io_pool->pop_completed();
      if (op && op->callback) {
        // run cb on main thread
        op->callback();
      }
    }
  }

  Value builtin_print(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_setTimeout(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_setInterval(const std::vector<Value> &argvals,
                            const CallExpr *);
  Value builtin_clearInterval(const std::vector<Value> &argvals,
                              const CallExpr *);
  Value builtin_math_sqrt(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_math_rand(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_fs_readFile(const std::vector<Value> &argvals,
                            const CallExpr *);
  Value builtin_fs_writeFile(const std::vector<Value> &argvals,
                             const CallExpr *);
  Value builtin_fs_appendFile(const std::vector<Value> &argvals,
                              const CallExpr *);
  Value builtin_fs_fileExists(const std::vector<Value> &argvals,
                              const CallExpr *);
  Value builtin_fs_isDirectory(const std::vector<Value> &argvals,
                               const CallExpr *);
  Value builtin_fs_deleteFile(const std::vector<Value> &argvals,
                              const CallExpr *);
  Value builtin_fs_listDir(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_net_createServer(const std::vector<Value> &argvals,
                                 const CallExpr *);
  Value builtin_net_conn_read(const std::vector<Value> &argvals,
                              const CallExpr *);
  Value builtin_net_conn_write(const std::vector<Value> &argvals,
                               const CallExpr *);
  Value builtin_net_conn_close(const std::vector<Value> &argvals,
                               const CallExpr *);
#ifndef NO_OPENSSL
  Value builtin_http_get(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_http_request(const std::vector<Value> &argvals,
                             const CallExpr *);
#endif
  Value builtin_push(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_pop(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_length(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_slice(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_concat(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_keys(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_values(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_hasKey(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_yield(const std::vector<Value> &, const CallExpr *);
  Value builtin_shutdown_requested(const std::vector<Value> &,
                                   const CallExpr *);
  Value builtin_substr(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_indexOf(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_lastIndexOf(const std::vector<Value> &argvals,
                            const CallExpr *);
  Value builtin_split(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_replace(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_replaceAll(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_trim(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_toUpper(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_toLower(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_startsWith(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_endsWith(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_charAt(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_join(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_typeof(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_toNumber(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_toString(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_toBool(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_isNumber(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_isString(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_isBool(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_isArray(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_isObject(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_isFunction(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_readLine(const std::vector<Value> &, const CallExpr *);
  Value builtin_readAll(const std::vector<Value> &, const CallExpr *);
  Value builtin_time(const std::vector<Value> &, const CallExpr *);

#ifndef NO_FFI
  Value builtin_ffi_load_library(const std::vector<Value> &argvals,
                                 const CallExpr *);
  Value builtin_ffi_get_symbol(const std::vector<Value> &argvals,
                               const CallExpr *);
  Value builtin_ffi_call(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_ffi_membersof(const std::vector<Value> &argvals,
                              const CallExpr *);
  Value builtin_ffi_alloc(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_ffi_alloc_struct(const std::vector<Value> &argvals,
                                 const CallExpr *);
  Value builtin_ffi_read_struct(const std::vector<Value> &argvals,
                                const CallExpr *);
  Value builtin_ffi_write_struct(const std::vector<Value> &argvals,
                                 const CallExpr *);
  Value builtin_ffi_sizeof(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_ffi_utf16(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_ffi_utf16_read(const std::vector<Value> &argvals,
                               const CallExpr *);
  Value builtin_ffi_free(const std::vector<Value> &argvals, const CallExpr *);
  Value builtin_ffi_create_callback(const std::vector<Value> &argvals,
                                    const CallExpr *);
  Value builtin_ffi_free_callback(const std::vector<Value> &argvals,
                                  const CallExpr *);
#endif

  Value builtin_platform_arch(const std::vector<Value> &, const CallExpr *);
  Value builtin_platform_os(const std::vector<Value> &, const CallExpr *);
  Value builtin_platform_endianness(const std::vector<Value> &,
                                    const CallExpr *);
  Value builtin_platform_hostname(const std::vector<Value> &, const CallExpr *);
  Value builtin_platform_getenv(const std::vector<Value> &, const CallExpr *);

  Value builtin_system_exec(const std::vector<Value> &, const CallExpr *);

  Value builtin_gc_collect(const std::vector<Value> &, const CallExpr *);
  Value builtin_gc_stats(const std::vector<Value> &, const CallExpr *);
  Value builtin_gc_enable(const std::vector<Value> &, const CallExpr *);
  Value builtin_gc_set_threshold(const std::vector<Value> &, const CallExpr *);
};
