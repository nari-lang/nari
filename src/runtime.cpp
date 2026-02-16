#include "runtime.h"
#include "ast.h"
#include "parser_api.h"
#include "util.h"

#include <cmath>
#include <stdexcept>

#ifdef _WIN32
#include <cstdio>
#include <cstdlib>
// Windows doesn't have getline, provide a simple implementation
static ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
  if (!lineptr || !n || !stream)
    return -1;

  size_t pos = 0;
  int c;

  if (*lineptr == nullptr || *n == 0) {
    *n = 128;
    *lineptr = (char *)realloc(*lineptr, *n);
    if (!*lineptr)
      return -1;
  }

  while ((c = fgetc(stream)) != EOF) {
    if (pos + 1 >= *n) {
      size_t new_size = *n * 2;
      char *new_ptr = (char *)realloc(*lineptr, new_size);
      if (!new_ptr)
        return -1;
      *lineptr = new_ptr;
      *n = new_size;
    }
    (*lineptr)[pos++] = c;
    if (c == '\n')
      break;
  }

  if (pos == 0 && c == EOF)
    return -1;
  (*lineptr)[pos] = '\0';
  return pos;
}
#endif

std::string nari_embedded_stdlib();

// custom exception for runtime errors
class RuntimeError : public std::runtime_error {
public:
  RuntimeError(const std::string &msg, const std::string &fname = "",
               int line = 0, int col = 0)
      : std::runtime_error(msg), filename(fname), line_no(line), col_no(col) {
    // Set global flag to signal runtime should halt execution
    Runtime::g_runtime_error_occurred.store(true);
  }

  std::string filename;
  int line_no;
  int col_no;
};

// trace state
static bool g_trace_enabled = false;
static int g_trace_level = 0;

namespace Runtime {
bool runtime_trace_enabled() { return g_trace_enabled; }
void set_runtime_trace_level(TraceLevel level) {
  g_trace_level = (int)level;
  g_trace_enabled = (g_trace_level > 0);
}
TraceLevel get_runtime_trace_level() { return (TraceLevel)g_trace_level; };
TraceLevel runtime_trace_level() {
  return static_cast<TraceLevel>(g_trace_level);
}
void runtime_log(TraceLevel level, const std::string &msg) {
  if (g_trace_enabled && static_cast<int>(level) <= g_trace_level) {
    fprintf(stderr, "%s\n", msg.c_str());
  }
}
} // namespace Runtime

// Report fatal runtime error with source location
// throws RuntimeError instead of exiting
static void runtime_fatal(const std::string &msg,
                          const nari::ASTNode *node = nullptr) {
  std::string fname = "<runtime>";
  int l = 0, c = 0;
  if (node) {
    if (!node->filename.empty())
      fname = node->filename;
    l = node->line;
    c = node->col;
  }

  // print error to stderr for logging
  fprintf(stderr, "Runtime error at %s:%d:%d\n", fname.c_str(), l, c);
  fprintf(stderr, "    %s %s:%d:%d\n", msg.c_str(), fname.c_str(), l, c);

  if (!fname.empty() && fname[0] != '<' && l > 0) {
    FILE *fp = fopen(fname.c_str(), "r");
    if (fp) {
      char *line_buffer = nullptr;
      size_t buffer_size = 0;
      std::string line_text;

      for (int i = 1; i <= l; ++i) {
        ssize_t line_length = getline(&line_buffer, &buffer_size, fp);
        if (line_length == -1)
          break;
        if (i == l) {
          line_text = line_buffer;
          // Remove trailing newline if present
          if (!line_text.empty() && line_text.back() == '\n') {
            line_text.pop_back();
          }
        }
      }
      free(line_buffer);
      fclose(fp);

      if (!line_text.empty()) {
        fprintf(stderr, "    %s\n", line_text.c_str());
        int caret_pos = c > 0 ? c - 1 : 0;
        std::string caret_line(caret_pos, ' ');
        caret_line.push_back('^');
        fprintf(stderr, "    %s\n", caret_line.c_str());
      }
    }
  }

  // throw exception instead of exiting
  throw RuntimeError(msg, fname, l, c);
}

using namespace nari;
using namespace Runtime;

// Interpreter
void ScriptRuntime::run_start(bool found_toplevel) {
  auto it = functions.find("start");
  if (it == functions.end()) {
    if (!found_toplevel) {
      fprintf(stderr, "No start() function found and no top level code found. "
                      "Nothing to run!\n");
    }
    return;
  }
  call_user_function(it->second.get(), {});
  if (flags.throw_flag) {
    runtime_fatal("Uncaught throw: " + flags.throw_value.to_string(), nullptr);
  }
}

void ScriptRuntime::run_top_level() {
  bool found_toplevel = false;

  // use preserved insertion order so imports run before the importing module
  const auto &toplevel_funcs = toplevel_order;

  for (const auto &funcname : toplevel_funcs) {
    auto it = functions.find(funcname);
    if (it == functions.end())
      continue;

    found_toplevel = true;
    Function *f = it->second.get();

    // push to module stack
    if (!f->filename.empty()) {
      module_stack.push_back(f->filename);
    }

    // direct execution of top-level function body
    if (f->function_expr && f->function_expr->body) {
      for (const auto &st : f->function_expr->body->stmts) {
        if (!st)
          continue;
        exec_stmt(st.get());
        if (flags.return_flag)
          break;
        if (flags.throw_flag)
          break;
      }
    } else if (f->body) {
      for (const auto &st : f->body->stmts) {
        if (!st)
          continue;
        exec_stmt(st.get());
        if (flags.return_flag)
          break;
        if (flags.throw_flag)
          break;
      }
    }

    // Pop the module from module_stack
    if (!f->filename.empty() && !module_stack.empty()) {
      module_stack.pop_back();
    }

    if (flags.throw_flag) {
      runtime_fatal("Uncaught Throw: " + flags.throw_value.to_string(),
                    nullptr);
      return;
    }
  }

  run_event_loop();
  run_start(found_toplevel);
  run_event_loop();
}

void ScriptRuntime::step_task(HandlePtr handle) {
  if (!handle || !handle->task)
    return;

  Task *task = handle->task.get();
  if (task->state != Task::Running && task->state != Task::Yielded)
    return;

  auto saved_block_scopes = block_scope_stack;
  block_scope_stack = task->block_scopes;
  call_stack.push_back(task->locals);

  Flags saved_flags = flags;
  flags = task->flags;

  if (task->body && task->current_stmt < task->body->stmts.size()) {
    for (; task->current_stmt < task->body->stmts.size();
         ++task->current_stmt) {
      const auto &stmt = task->body->stmts[task->current_stmt];
      if (stmt) {
        exec_stmt(stmt.get());
      }
      if (flags.return_flag || flags.throw_flag) {
        break;
      }
    }
  }

  if (task->current_stmt >= task->body->stmts.size() || flags.return_flag) {
    handle->end_time = std::chrono::steady_clock::now();
    if (flags.throw_flag) {
      handle->state = HandleData::Failed;
      handle->error = flags.throw_value;
      task->state = Task::Failed;
    } else {
      handle->state = HandleData::Completed;
      handle->result = flags.return_flag ? flags.return_value : Value::none();
      task->state = Task::Completed;
    }
  } else {
    task->state = Task::Yielded;
  }

  task->locals = call_stack.back();
  task->block_scopes = block_scope_stack;
  task->flags = flags;

  call_stack.pop_back();
  block_scope_stack = saved_block_scopes;
  flags = saved_flags;
}

void ScriptRuntime::run_event_loop() {
  while ((!task_queue.empty() || has_pending_io()) &&
         !Runtime::g_shutdown_requested.load() &&
         !Runtime::g_runtime_error_occurred.load()) {
    process_completed_io();

    // Check if garbage collection should run
    collect_garbage();

    auto now = std::chrono::steady_clock::now();
    for (auto &[id, interval] : active_intervals) {
      if (now >= interval.next_fire) {
        if (interval.callback.is_function()) {
          const auto &fn = interval.callback.get_function();
          // Try direct pointer first (lambdas)  
          if (fn.func_ptr) {
            call_user_function(fn.func_ptr.get(), {});
          } else {
            // Fall back to global map (named functions)
            auto it = functions.find(fn.name);
            if (it != functions.end()) {
              call_user_function(it->second.get(), {});
            }
          }
        }
        interval.next_fire =
            now + std::chrono::milliseconds(interval.interval_ms);
      }
    }

    if (!task_queue.empty()) {
      HandlePtr handle = task_queue.front();
      task_queue.pop();
      step_task(handle);
      if (handle->state == HandleData::Running) {
        task_queue.push(handle);
      }
    } else if (has_pending_io()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  // Final GC pass after event loop completes
  collect_garbage();
}

// pattern matching, this is for match, not regex :)
bool ScriptRuntime::match_pattern(const Pattern *pattern, const Value &value,
                                  Value &bindings) {
  if (!pattern)
    return false;

  // if it's not an object, make it one
  if (!bindings.is_object()) {
    bindings = Value::make_object();
  }

  // dispatch via type tag
  switch (pattern->pattern_kind) {

  case PatternKind::Wildcard:
    return true;

  case PatternKind::Binding: {
    const auto *bp = (const BindingPattern *)pattern;
    (*bindings.get_object())[bp->name] = value;
    return true;
  }

  case PatternKind::Literal: {
    const auto *lp = (const LiteralPattern *)pattern;
    Value pattern_value = eval_expr(lp->value.get());

    if (value.is_int() && pattern_value.is_int()) {
      return value.get_int() == pattern_value.get_int();
    }
    if ((value.is_int() || value.is_float()) &&
        (pattern_value.is_int() || pattern_value.is_float())) {
      return std::fabs(value.as_number() - pattern_value.as_number()) < 1e-12;
    }
    return value.to_string() == pattern_value.to_string();
  }

  case PatternKind::Variant: {
    const auto *vp = (const VariantPattern *)pattern;
    // for now, check if value is an object with __variant field
    if (!value.is_object())
      return false;

    auto obj = value.get_object();
    if (!obj)
      return false;

    auto variant_it = obj->find("__variant");
    if (variant_it == obj->end())
      return false;

    // name matches?
    std::string variant_name = variant_it->second.to_string();
    if (variant_name != vp->variant_name)
      return false;

    // if pattern has fields, match them
    if (!vp->fields.empty()) {
      auto data_it = obj->find("__data");
      if (data_it == obj->end())
        return false;

      const Value &data = data_it->second;

      if (data.is_array()) {
        // tuple variant
        auto data_arr = data.get_array();
        if (!data_arr || vp->fields.size() != data_arr->size())
          return false;

        for (size_t i = 0; i < vp->fields.size(); i++) {
          if (!match_pattern(vp->fields[i].get(), (*data_arr)[i], bindings)) {
            return false;
          }
        }
      } else {
        // single value variant
        if (vp->fields.size() == 1) {
          if (!match_pattern(vp->fields[0].get(), data, bindings)) {
            return false;
          }
        } else {
          return false;
        }
      }
    }

    return true;
  }

  default:
    break;
  }

  return false;
}

// garbage collection implementation
std::vector<const Value *> ScriptRuntime::collect_gc_roots() const {
  std::vector<const Value *> roots;

  // collect from globals
  for (const auto &[key, val] : globals) {
    roots.push_back(&val);
  }

  // collect from call stack
  for (const auto &frame : call_stack) {
    for (const auto &[key, val] : frame) {
      roots.push_back(&val);
    }
  }

  // collect from block scopes
  for (const auto &scope : block_scope_stack) {
    for (const auto &[key, val] : scope) {
      roots.push_back(&val);
    }
  }

  // collect from module-local vars
  for (const auto &[mod, vars] : module_local_vars) {
    for (const auto &[key, val] : vars) {
      roots.push_back(&val);
    }
  }

  // collect from closure scope
  if (current_scope_closure) {
    for (const auto &[key, val] : *current_scope_closure) {
      roots.push_back(&val);
    }
  }

  // collect from task queue
  std::queue<HandlePtr> queue_copy = task_queue;
  while (!queue_copy.empty()) {
    const HandlePtr &handle = queue_copy.front();
    if (handle) {
      // the handle itself is tracked,
      // but this ensures that we mark the Value wrapper as well
      static thread_local Value temp_handle;
      temp_handle = Value::make_handle(handle);
      roots.push_back(&temp_handle);
    }
    queue_copy.pop();
  }

  // Collect from flags (return/throw values)
  if (!flags.return_value.is_none()) {
    roots.push_back(&flags.return_value);
  }
  if (!flags.throw_value.is_none()) {
    roots.push_back(&flags.throw_value);
  }

  return roots;
}

// lookup variable through scopes (block -> call -> module -> global)
Value ScriptRuntime::lookup_variable(const std::string &name, const std::string &filename, bool &found) {
  found = false;
  
  // check block scopes (innermost to outermost)
  for (int i = block_scope_stack.size() - 1; i >= 0; i--) {
    auto &block_scope = block_scope_stack[i];
    auto it = block_scope.find(name);
    if (it != block_scope.end()) {
      found = true;
      return it->second;
    }
  }
  
  // check function call stack (locals)
  if (!call_stack.empty()) {
    auto &locals = call_stack.back();
    auto it = locals.find(name);
    if (it != locals.end()) {
      found = true;
      return it->second;
    }
  }
  
  // check module-local vars (current module on stack)
  if (!module_stack.empty()) {
    auto it = module_local_vars.find(module_stack.back());
    if (it != module_local_vars.end()) {
      auto mlocal = it->second.find(name);
      if (mlocal != it->second.end()) {
        found = true;
        return mlocal->second;
      }
    }
  }
  
  // check globals
  auto it = globals.find(name);
  if (it != globals.end()) {
    found = true;
    return it->second;
  }
  
  // not found
  return Value::make_int(0);
}

// store variable through scopes (block -> call -> module -> global)
void ScriptRuntime::store_variable(const std::string &name, const std::string &filename, const Value &value) {
  // try block scopes first
  for (int i = block_scope_stack.size() - 1; i >= 0; i--) {
    auto &block_scope = block_scope_stack[i];
    auto it = block_scope.find(name);
    if (it != block_scope.end()) {
      it->second = value;
      return;
    }
  }
  
  // try call stack
  if (!call_stack.empty()) {
    auto &locals = call_stack.back();
    if (locals.find(name) != locals.end()) {
      locals[name] = value;
      return;
    }
  }
  
  // try module-local vars (current module)
  if (!module_stack.empty()) {
    auto it = module_local_vars.find(module_stack.back());
    if (it != module_local_vars.end() &&
        it->second.find(name) != it->second.end()) {
      it->second[name] = value;
      return;
    }
  }
  
  // try module-local vars by filename
  if (!filename.empty()) {
    auto it = module_local_vars.find(filename);
    if (it != module_local_vars.end() &&
        it->second.find(name) != it->second.end()) {
      it->second[name] = value;
      return;
    }
  }
  
  // fall back to globals
  globals[name] = value;
}

void ScriptRuntime::collect_garbage() {
  auto &gc = GarbageCollector::instance();

  if (!gc.is_enabled() || !gc.should_collect()) {
    return;
  }

  // Collect all root values
  auto roots = collect_gc_roots();

  // Run garbage collection
  size_t collected = gc.collect(roots);

  if (Runtime::runtime_trace_enabled()) {
    Runtime::runtime_log(Runtime::TraceLevel::Debug,
                         "GC: Collected " + std::to_string(collected) +
                             " unreachable objects. " + "Tracked objects: " +
                             std::to_string(gc.get_tracked_count()));
  }
}

// recursively collect all fields from a class and its parents
static void collect_all_fields(const nari::ClassDecl *class_decl, 
                                std::vector<const nari::ClassField*> &all_fields) {
  if (!class_decl) return;
  
  // collect parent fields
  if (!class_decl->parent_name.empty()) {
    const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
    if (parent) {
      collect_all_fields(parent, all_fields);
    }
  }
  
  // then add this class's fields
  for (const auto &field : class_decl->fields) {
    all_fields.push_back(&field);
  }
}

// find method in class hierarchy, nullptr if not found.
static const nari::ClassMethod* find_method_in_hierarchy(const nari::ClassDecl *class_decl,
                                                          const std::string &method_name) {
  if (!class_decl) return nullptr;
  
  // check this class
  for (const auto &m : class_decl->methods) {
    if (m.name == method_name) {
      return &m;
    }
  }
  
  // then check our parent
  if (!class_decl->parent_name.empty()) {
    const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
    if (parent) {
      return find_method_in_hierarchy(parent, method_name);
    }
  }
  
  return nullptr;
}

// find field declarations in class hierarchy, again, nullptr if not found.
static const nari::ClassField* find_field_in_hierarchy(const nari::ClassDecl *class_decl,
                                                        const std::string &field_name) {
  if (!class_decl) return nullptr;
  
  // check this class
  for (const auto &field : class_decl->fields) {
    if (field.name == field_name) {
      return &field;
    }
  }
  
  // then check our parent
  if (!class_decl->parent_name.empty()) {
    const nari::ClassDecl *parent = Parser::get_registered_class(class_decl->parent_name);
    if (parent) {
      return find_field_in_hierarchy(parent, field_name);
    }
  }
  
  return nullptr;
}

Value ScriptRuntime::eval_expr(const Expr *e) {
  if (!e)
    return Value::none();

  // dispatch via type tag instead of dynamic_cast
  switch (e->kind) {

  case ExprKind::Ident: {
    const auto *ie = static_cast<const IdentExpr *>(e);
    if (ie->name == "true")
      return Value::make_bool(true);
    if (ie->name == "false")
      return Value::make_bool(false);

    for (int i = block_scope_stack.size() - 1; i >= 0; i--) {
      auto &block_scope = block_scope_stack[i];
      auto it = block_scope.find(ie->name);
      if (it != block_scope.end())
        return it->second;
    }
    // function scope (locals)
    if (!call_stack.empty()) {
      auto &locals = call_stack.back();
      auto it = locals.find(ie->name);
      if (it != locals.end())
        return it->second;
    }
    // module-local lookup, prefer the current module on the module stack
    if (!module_stack.empty()) {
      const std::string &mod = module_stack.back();
      auto it = module_local_vars.find(mod);
      if (it != module_local_vars.end()) {
        auto mlocal = it->second.find(ie->name);
        if (mlocal != it->second.end())
          return mlocal->second;
      }
    }
    if (!ie->filename.empty()) {
      auto it = module_local_vars.find(ie->filename);
      if (it != module_local_vars.end()) {
        auto mlocal = it->second.find(ie->name);
        if (mlocal != it->second.end())
          return mlocal->second;
      }
    }
    // Global lookup
    auto global_it = globals.find(ie->name);
    if (global_it != globals.end())
      return global_it->second;
    // Check if it's a function name (for first-class function support)
    auto func_it = functions.find(ie->name);
    if (func_it != functions.end()) {
      return Value::make_function(ie->name);
    }
    // Only allow global builtins as identifiers, not method-only builtins
    if (is_global_builtin(ie->name)) {
      return Value::make_function(ie->name);
    }

    // Check if it's a registered type name
    if (Parser::get_registered_type(ie->name)) {
      return Value::make_string(ie->name);
    }

    // Undefined identifier: this is a fatal runtime error (do not coerce to
    // string).
    std::string msg = "Undefined identifier '" + ie->name + "'";
    runtime_fatal(msg, ie);
    __builtin_unreachable();
  }

  case ExprKind::String: {
    const auto *se = static_cast<const StringExpr *>(e);
    return Value::make_string(se->value);
  }

  case ExprKind::Number: {
    const auto *ne = static_cast<const NumberExpr *>(e);
    if (ne->is_float)
      return Value::make_float(ne->f);
    return Value::make_int(ne->i);
  }

  case ExprKind::Bool: {
    const auto *be = static_cast<const BoolExpr *>(e);
    return Value::make_bool(be->value);
  }

  case ExprKind::Null:
    return Value::none();

  case ExprKind::Unary: {
    const auto *unaryExpr = static_cast<const UnaryExpr *>(e);
    if (!unaryExpr->operand)
      return Value::none();
    const std::string &op = unaryExpr->op;

    // Prefix ++ and --
    if (op == "++" || op == "--") {
      if (!unaryExpr->operand) {
        runtime_fatal("Increment/decrement operand is null", unaryExpr);
      }
      const IdentExpr *ie =
          unaryExpr->operand->kind == ExprKind::Ident
              ? static_cast<const IdentExpr *>(unaryExpr->operand.get())
              : nullptr;
      if (!ie) {
        if (unaryExpr->operand->kind == ExprKind::Unary) {
          const auto *nested =
              static_cast<const UnaryExpr *>(unaryExpr->operand.get());
          std::string msg =
              "Prefix ++ has nested UnaryExpr (op=" + nested->op + ")";
          runtime_fatal(msg, unaryExpr);
        }
        std::string msg = "Prefix ++ requires a variable";
        runtime_fatal(msg, unaryExpr);
      }

      bool found = false;
      Value current = lookup_variable(ie->name, ie->filename, found);

      Value newval;
      if (current.is_float()) {
        double val = current.get_float() + ((op == "++") ? 1.0 : -1.0);
        newval = Value::make_float(val);
      } else if (current.is_int() || current.is_none()) {
        int64_t val = current.is_int() ? current.get_int() : 0;
        val += (op == "++") ? 1 : -1;
        newval = Value::make_int(val);
      } else {
        runtime_fatal("Increment/decrement requires int or float", unaryExpr);
      }

      store_variable(ie->name, ie->filename, newval);
      return newval;
    }

    // this is postfix ++ and --
    if (op == "post++" || op == "post--") {
      const IdentExpr *ie =
          unaryExpr->operand->kind == ExprKind::Ident
              ? static_cast<const IdentExpr *>(unaryExpr->operand.get())
              : nullptr;
      if (!ie) {
        runtime_fatal("Increment/decrement requires a variable", unaryExpr);
      }
      
      bool found = false;
      Value current = lookup_variable(ie->name, ie->filename, found);
      Value oldval = current;

      Value newval;
      if (current.is_float()) {
        double val = current.get_float() + ((op == "post++") ? 1.0 : -1.0);
        newval = Value::make_float(val);
      } else if (current.is_int() || current.is_none()) {
        int64_t val = current.is_int() ? current.get_int() : 0;
        val += (op == "post++") ? 1 : -1;
        newval = Value::make_int(val);
      } else {
        runtime_fatal("Increment/decrement requires int or float", unaryExpr);
      }

      store_variable(ie->name, ie->filename, newval);
      return oldval;
    }

    Value v = eval_expr(unaryExpr->operand.get());
    if (op == "neg") {
      if (v.is_float())
        return Value::make_float(-v.get_float());
      if (v.is_int())
        return Value::make_int(-v.get_int());
      return Value::make_float(-v.as_number());
    } else if (op == "!") {
      return Value::make_bool(!v.as_bool());
    } else if (op == "~") {
      // Bitwise NOT - only works on integers
      if (v.is_int()) {
        return Value::make_int(~v.get_int());
      }
      // Convert to int if not already
      return Value::make_int(~static_cast<int64_t>(v.as_number()));
    } else {
      return v;
    }
  }

  case ExprKind::Binary: {
    const auto *binaryExpr = static_cast<const BinaryExpr *>(e);
    const std::string &op = binaryExpr->op;
    if (op == "&&") {
      if (!binaryExpr->left || !binaryExpr->right)
        return Value::none();
      Value a = eval_expr(binaryExpr->left.get());
      if (!a.as_bool())
        return Value::make_bool(false);
      Value b = eval_expr(binaryExpr->right.get());
      return Value::make_bool(b.as_bool());
    }
    if (op == "||") {
      if (!binaryExpr->left || !binaryExpr->right)
        return Value::none();
      Value a = eval_expr(binaryExpr->left.get());
      if (a.as_bool())
        return Value::make_bool(true);
      Value b = eval_expr(binaryExpr->right.get());
      return Value::make_bool(b.as_bool());
    }
    if (op == "??") {
      if (!binaryExpr->left || !binaryExpr->right)
        return Value::none();
      Value a = eval_expr(binaryExpr->left.get());
      if (!a.is_none())
        return a;
      return eval_expr(binaryExpr->right.get());
    }

    // Arithmetic and comparison
    if (!binaryExpr->left || !binaryExpr->right)
      return Value::none();
    Value left = eval_expr(binaryExpr->left.get());
    Value right = eval_expr(binaryExpr->right.get());

    if (op == "+") {
      if (left.is_string() || right.is_string()) {
        return Value::make_string(left.to_string() + right.to_string());
      }
      if (left.is_int() && right.is_int()) {
        return Value::make_int(left.get_int() + right.get_int());
      }
      return Value::make_float(left.as_number() + right.as_number());
    }
    if (op == "@") {
      return Value::make_string(left.to_string() + right.to_string());
    }
    if (op == "-") {
      if (left.is_int() && right.is_int()) {
        return Value::make_int(left.get_int() - right.get_int());
      }
      return Value::make_float(left.as_number() - right.as_number());
    }
    if (op == "*") {
      if (left.is_int() && right.is_int()) {
        return Value::make_int(left.get_int() * right.get_int());
      }
      return Value::make_float(left.as_number() * right.as_number());
    }
    if (op == "/") {
      double rn = right.as_number();
      if (rn == 0.0)
        return Value::make_float(0.0);
      return Value::make_float(left.as_number() / rn);
    }
    if (op == "%") {
      if (left.is_int() && right.is_int()) {
        if (right.get_int() == 0)
          return Value::make_int(0);
        return Value::make_int(left.get_int() % right.get_int());
      }
      double rn = right.as_number();
      if (rn == 0.0)
        return Value::make_float(0.0);
      return Value::make_float(std::fmod(left.as_number(), rn));
    }
    if (op == "**") {
      if (left.is_int() && right.is_int() && right.get_int() >= 0) {
        if (right.get_int() > std::numeric_limits<uint64_t>::max()) {
          return Value::make_float(
              std::pow(left.as_number(), right.as_number()));
        }
        uint64_t exp = static_cast<uint64_t>(right.get_int());
        int64_t base = left.get_int();
        int64_t result = 1;
        while (exp > 0) {
          if (exp & 1ULL) {
            __int128 v =
                static_cast<__int128>(result) * static_cast<__int128>(base);
            if (v < std::numeric_limits<int64_t>::min() ||
                v > std::numeric_limits<int64_t>::max()) {
              return Value::make_float(
                  std::pow(left.as_number(), right.as_number()));
            }
            result = static_cast<int64_t>(v);
          }
          if (exp > 1) {
            __int128 v =
                static_cast<__int128>(base) * static_cast<__int128>(base);
            if (v < std::numeric_limits<int64_t>::min() ||
                v > std::numeric_limits<int64_t>::max()) {
              return Value::make_float(
                  std::pow(left.as_number(), right.as_number()));
            }
            base = static_cast<int64_t>(v);
          }
          exp >>= 1ULL;
        }
        return Value::make_int(result);
      }
      return Value::make_float(std::pow(left.as_number(), right.as_number()));
    }

    if (op == "==") {
      if (left.is_int() && right.is_int()) {
        return Value::make_bool(left.get_int() == right.get_int());
      }
      if ((left.is_int() || left.is_float()) &&
          (right.is_int() || right.is_float())) {
        return Value::make_bool(
            std::fabs(left.as_number() - right.as_number()) < 1e-12);
      }
      return Value::make_bool(left.to_string() == right.to_string());
    }
    if (op == "!=") {
      if (left.is_int() && right.is_int()) {
        return Value::make_bool(left.get_int() != right.get_int());
      }
      if ((left.is_int() || left.is_float()) &&
          (right.is_int() || right.is_float())) {
        return Value::make_bool(
            !(std::fabs(left.as_number() - right.as_number()) < 1e-12));
      }
      return Value::make_bool(left.to_string() != right.to_string());
    }
    if (op == "<") {
      if (left.is_int() && right.is_int()) {
        return Value::make_bool(left.get_int() < right.get_int());
      }
      return Value::make_bool(left.as_number() < right.as_number());
    }
    if (op == ">") {
      if (left.is_int() && right.is_int()) {
        return Value::make_bool(left.get_int() > right.get_int());
      }
      return Value::make_bool(left.as_number() > right.as_number());
    }
    if (op == "<=") {
      if (left.is_int() && right.is_int()) {
        return Value::make_bool(left.get_int() <= right.get_int());
      }
      return Value::make_bool(left.as_number() <= right.as_number());
    }
    if (op == ">=") {
      if (left.is_int() && right.is_int()) {
        return Value::make_bool(left.get_int() >= right.get_int());
      }
      return Value::make_bool(left.as_number() >= right.as_number());
    }

    // Bitwise operations - only work on integers
    if (op == "&") {
      int64_t l = left.is_int() ? left.get_int()
                                : static_cast<int64_t>(left.as_number());
      int64_t r = right.is_int() ? right.get_int()
                                 : static_cast<int64_t>(right.as_number());
      return Value::make_int(l & r);
    }
    if (op == "|") {
      int64_t l = left.is_int() ? left.get_int()
                                : static_cast<int64_t>(left.as_number());
      int64_t r = right.is_int() ? right.get_int()
                                 : static_cast<int64_t>(right.as_number());
      return Value::make_int(l | r);
    }
    if (op == "^") {
      int64_t l = left.is_int() ? left.get_int()
                                : static_cast<int64_t>(left.as_number());
      int64_t r = right.is_int() ? right.get_int()
                                 : static_cast<int64_t>(right.as_number());
      return Value::make_int(l ^ r);
    }
    if (op == "<<") {
      int64_t l = left.is_int() ? left.get_int()
                                : static_cast<int64_t>(left.as_number());
      int64_t r = right.is_int() ? right.get_int()
                                 : static_cast<int64_t>(right.as_number());
      // clamp shift amount to reasonable range to avoid UB
      if (r < 0)
        r = 0;
      if (r >= 64)
        r = 63;
      return Value::make_int(l << r);
    }
    if (op == ">>") {
      int64_t l = left.is_int() ? left.get_int()
                                : static_cast<int64_t>(left.as_number());
      int64_t r = right.is_int() ? right.get_int()
                                 : static_cast<int64_t>(right.as_number());

      // clamp shift amount to reasonable range to avoid UB
      if (r < 0)
        r = 0;
      if (r >= 64)
        r = 63;
      return Value::make_int(l >> r);
    }

    return Value::none();
  }

  case ExprKind::Call: {
    const auto *callExpr = static_cast<const CallExpr *>(e);
    // check if this is a method call (callee is MemberExpr like obj.method())
    if (callExpr->callee->kind == ExprKind::Member) {
      const auto *me = static_cast<const MemberExpr *>(callExpr->callee.get());
      Value obj = eval_expr(me->object.get());
      std::string method_name = me->member;

      // handle class instance method calls
      if (obj.is_class_instance()) {
        const auto &instance = obj.get_class_instance();
        const nari::ClassDecl *class_decl =
            Parser::get_registered_class(instance->class_name);

        if (!class_decl) {
          runtime_fatal("Unknown class: " + instance->class_name, callExpr);
        }

        // find method in class hierarchy
        const nari::ClassMethod *method = find_method_in_hierarchy(class_decl, method_name);

        if (!method) {
          runtime_fatal("Class " + instance->class_name + " has no method '" +
                            method_name + "'",
                        callExpr);
        }

        // Check visibility
        if (method->visibility == nari::Visibility::Private) {
          if (current_class_name != instance->class_name) {
            runtime_fatal("Cannot call private method '" + method_name +
                              "' of class " + instance->class_name,
                          callExpr);
          }
        }

        // Evaluate arguments
        std::vector<Value> arg_values;
        for (const auto &arg_expr : callExpr->args) {
          arg_values.push_back(eval_expr(arg_expr.get()));
        }

        // Check argument count
        if (arg_values.size() != method->params.size()) {
          runtime_fatal("Method '" + method_name + "' expects " +
                            std::to_string(method->params.size()) +
                            " arguments but got " +
                            std::to_string(arg_values.size()),
                        callExpr);
        }

        // Set up 'this' context and execute method
        ClassInstancePtr saved_instance = current_instance;
        std::string saved_class = current_class_name;
        current_instance = instance;
        current_class_name = instance->class_name;

        // Create new scope for method
        call_stack.emplace_back();

        // Bind parameters
        for (size_t i = 0; i < method->params.size(); ++i) {
          call_stack.back()[method->params[i].name] = arg_values[i];
        }

        // Execute method body
        Value return_value = Value::none();
        if (method->body) {
          for (const auto &stmt : method->body->stmts) {
            exec_stmt(stmt.get());
            if (flags.return_flag) {
              return_value = flags.return_value;
              flags.return_flag = false;
              break;
            }
            if (flags.break_flag || flags.continue_flag || flags.throw_flag) {
              break;
            }
          }
        }

        // Restore context
        call_stack.pop_back();
        current_instance = saved_instance;
        current_class_name = saved_class;

        return return_value;
      }

      // First check if the object has this member as a property (for
      // obj.method() on objects)
      if (obj.is_object()) {
        const auto &objMap = obj.get_object();
        if (objMap) {
          auto it = objMap->find(method_name);
          if (it != objMap->end()) {
            // Object has this property - check if it's a function
            if (it->second.is_function()) {
              // This is a method/function property on an object - call it
              std::vector<Value> argvals;
              for (const auto &a : callExpr->args) {
                argvals.push_back(eval_expr(a.get()));
              }

              const auto &func_val = it->second.get_function();

              // Check if it's a builtin function
              if (is_builtin_name(func_val.name)) {
                return call_builtin(func_val.name, argvals, callExpr);
              }

              // Try direct pointer first (for lambdas)
              if (func_val.func_ptr) {
                return call_user_function(func_val.func_ptr.get(), argvals);
              }

              // Otherwise look it up as a named user function
              auto func_it = functions.find(func_val.name);
              if (func_it != functions.end()) {
                return call_user_function(func_it->second.get(), argvals);
              }
            }
            // Member exists but is not a function - fall through to error
          }
        }
      }

      // If object doesn't have this as a property, check if it's a builtin
      // method
      if (is_builtin_name(method_name)) {
        // Check if the method is valid for this type
        if (!is_method_valid_for_type(method_name, obj)) {
          std::string type_str = obj.is_string()   ? "string"
                                 : obj.is_array()  ? "array"
                                 : obj.is_object() ? "object"
                                 : obj.is_int()    ? "number"
                                 : obj.is_float()  ? "number"
                                 : obj.is_bool()   ? "boolean"
                                 : obj.is_none()   ? "null"
                                                   : "value";
          runtime_fatal("Method '" + method_name +
                            "' is not available on type '" + type_str + "'",
                        callExpr);
        }

        // Prepare arguments: insert object as first argument
        std::vector<Value> method_args;
        method_args.reserve(callExpr->args.size() + 1);
        method_args.push_back(obj); // Object becomes first argument
        for (const auto &a : callExpr->args) {
          method_args.push_back(eval_expr(a.get()));
        }

        // Call the builtin with object as first arg
        return call_builtin(method_name, method_args, callExpr);
      }

      std::string type_str = obj.is_string()   ? "string"
                             : obj.is_array()  ? "array"
                             : obj.is_object() ? "object"
                             : obj.is_int()    ? "number"
                             : obj.is_float()  ? "number"
                             : obj.is_bool()   ? "boolean"
                                               : "value";
      runtime_fatal("Unknown method '" + method_name + "' on " + type_str,
                    callExpr);
    }

    // Regular function call (not a method)
    std::vector<Value> argvals;
    argvals.reserve(callExpr->args.size());
    for (const auto &a : callExpr->args)
      argvals.push_back(eval_expr(a.get()));

    std::string op;
    if (callExpr->callee->kind == ExprKind::Ident) {
      op = static_cast<const IdentExpr *>(callExpr->callee.get())->name;
    }

    Value calleeVal;
    bool haveCalleeVal = false;
    if (op.empty()) {
      calleeVal = eval_expr(callExpr->callee.get());
      haveCalleeVal = true;
      if (calleeVal.is_function()) {
        op = calleeVal.get_function().name;
      }
    }

    // Only allow calling global builtins directly, not method-only builtins
    if (is_global_builtin(op)) {
      return call_builtin(op, argvals, callExpr);
    }

    // try to evaluate the callee as an expression
    if (!haveCalleeVal) {
      calleeVal = eval_expr(callExpr->callee.get());
      haveCalleeVal = true;
    }
    if (calleeVal.is_function()) {
      const auto &func_val = calleeVal.get_function();
      
      // First, check if we have a direct pointer to the function (for lambdas)
      if (func_val.func_ptr) {
        return call_user_function(func_val.func_ptr.get(), argvals);
      }
      
      // Otherwise, look it up in the global functions map (for named functions)
      auto it = functions.find(func_val.name);
      if (it != functions.end()) {
        return call_user_function(it->second.get(), argvals);
      }
    }

    // Unknown call: print and return none

    printf("[call] %s(", (op.empty() ? "<expr>" : op.c_str()));
    for (size_t i = 0; i < argvals.size(); ++i) {
      if (i)
        printf(", ");
      printf("%s", argvals[i].to_string().c_str());
    }
    printf(")\n");
    return Value::none();
  }

  // ternary conditional expression
  case ExprKind::Ternary: {
    const auto *te = static_cast<const TernaryExpr *>(e);
    Value cond = eval_expr(te->condition.get());
    if (cond.as_bool()) {
      return eval_expr(te->true_expr.get());
    } else {
      return eval_expr(te->false_expr.get());
    }
  }

  // match expression - pattern matching
  case ExprKind::Match: {
    const auto *match_expr = static_cast<const MatchExpr *>(e);
    Value scrutinee = eval_expr(match_expr->scrutinee.get());

    // Try each arm in order
    for (const auto &arm : match_expr->arms) {
      Value bindings; // Will hold pattern bindings
      if (match_pattern(arm.pattern.get(), scrutinee, bindings)) {
        // Pattern matched! Apply bindings and evaluate body

        // Push a new block scope for pattern bindings
        block_scope_stack.push_back({});
        auto &scope = block_scope_stack.back();

        // Add bindings to scope
        if (bindings.is_object()) {
          for (const auto &[name, value] : *bindings.get_object()) {
            scope[name] = value;
          }
        }

        // Evaluate arm body
        Value result = eval_expr(arm.body.get());

        // Pop scope
        block_scope_stack.pop_back();

        return result;
      }
    }

    runtime_fatal("No pattern matched in match expression", match_expr);
  }

  // spawn expression - creates a handle for cooperative async execution
  case ExprKind::Spawn: {
    const auto *se = static_cast<const SpawnExpr *>(e);
    if (!se->body) {
      return Value::make_handle(nullptr);
    }
    auto handle = Value::make_handle_ptr();
    auto task = std::make_unique<Task>(se->body.get());

    // capture local variables and block scope
    if (!call_stack.empty()) {
      task->locals = call_stack.back();
    }
    task->block_scopes = block_scope_stack;

    handle->task = std::move(task);
    handle->state = HandleData::Running;

    task_queue.push(handle);
    return Value::make_handle(handle);
  }

  // string interpolation
  case ExprKind::StringInterpolation: {
    const auto *sie = static_cast<const StringInterpolationExpr *>(e);
    std::string result;

    // Iterate through parts and expression sources
    for (size_t i = 0; i < sie->parts.size(); ++i) {
      result += sie->parts[i];
      if (i < sie->expr_sources.size()) {
        // Temporarily set filename for better error messages
        std::string saved_filename =
            !sie->filename.empty() ? sie->filename : "<interpolation>";
        Parser::set_source_filename(saved_filename);

        auto expr_funcs =
            Parser::parse_program_from_source(sie->expr_sources[i]);
        Parser::set_source_filename(saved_filename);

        // parse_program_from_source returns at least 2 functions:
        // [0] = __top_level__ aggregator
        // [1] = __top_level__@filename with actual statements

        if (expr_funcs.size() < 2 || !expr_funcs[1] || !expr_funcs[1]->body ||
            expr_funcs[1]->body->stmts.empty()) {
          runtime_fatal("Failed to parse interpolated expression: " +
                            sie->expr_sources[i],
                        sie);
        }

        // Extract and evaluate the expression from the per-module function
        auto *first_stmt = expr_funcs[1]->body->stmts[0].get();
        auto *expr_stmt = first_stmt->stmt_kind == StmtKind::Expr
                              ? (nari::ExprStmt *)first_stmt
                              : nullptr;
        if (!expr_stmt || !expr_stmt->expr) {
          runtime_fatal("String interpolation must contain expressions: " +
                            sie->expr_sources[i],
                        sie);
        }

        Value expr_val = eval_expr(expr_stmt->expr.get());
        result += expr_val.to_string();
      }
    }

    return Value::make_string(result);
  }

  case ExprKind::ArrayLiteral: {
    const auto *ale = static_cast<const ArrayLiteralExpr *>(e);
    std::vector<Value> elements;
    for (const auto &elem : ale->elements) {
      elements.push_back(eval_expr(elem.get()));
    }
    return Value::make_array(std::move(elements));
  }

  case ExprKind::ObjectLiteral: {
    const auto *ole = static_cast<const ObjectLiteralExpr *>(e);
    auto entries = std::make_shared<std::unordered_map<std::string, Value>>();
    for (const auto &[key, val] : ole->entries) {
      (*entries)[key] = eval_expr(val.get());
    }
    return Value::make_object(std::move(entries));
  }

  // func(params) { ... }
  case ExprKind::Function: {
    const auto *fe = static_cast<const FunctionExpr *>(e);
    // Generate a unique name for this lambda function (for debugging/tracing)
    static size_t lambda_counter = 0;
    std::string func_name = "<lambda_" + std::to_string(lambda_counter++) + ">";

    // Create a Function object from the FunctionExpr
    // Use shared_ptr for lambdas so they can be cleaned up automatically
    auto func = std::make_shared<nari::Function>();
    func->name = func_name;
    func->line = fe->line;
    func->col = fe->col;
    func->filename = fe->filename;
    func->function_expr = fe; // original FunctionExpr

    // reuse current scope's closure if it exists, or create a new one
    if (!call_stack.empty()) {
      if (!current_scope_closure) {
        // create new shared closure environment for this scope
        current_scope_closure =
            std::make_shared<std::unordered_map<std::string, Value>>(
                call_stack.back());
      }
      func->closure_env_ptr =
          new std::shared_ptr<std::unordered_map<std::string, Value>>(
              current_scope_closure);
      // set up deleter for proper cleanup
      func->closure_deleter = [](void *ptr) {
        delete static_cast<
            std::shared_ptr<std::unordered_map<std::string, Value>> *>(ptr);
      };
    }

    // Copy parameters from FunctionExpr to Function
    for (const auto &param : fe->params) {
      func->params.emplace_back(
          param.name,
          nullptr, // We'll handle defaults during function calls
          param.is_rest);
    }
    // we can use the original body directly
    func->body = std::make_unique<BlockStmt>();
    
    // Don't add lambdas to global functions map - store them directly in the Value
    // This allows them to be garbage collected when no longer referenced
    return Value::make_function(func_name, func);
  }

  // index access: arr[index] or obj[key]
  case ExprKind::Index: {
    const auto *ie = static_cast<const IndexExpr *>(e);
    Value obj = eval_expr(ie->object.get());
    Value index = eval_expr(ie->index.get());

    if (obj.is_array()) {
      const auto &arr = obj.get_array();
      if (!index.is_int()) {
        runtime_fatal("Array index must be int", ie);
      }
      int64_t idx = index.get_int();
      if (!arr || idx < 0 || idx >= static_cast<int64_t>(arr->size())) {
        std::string error_msg =
            "Array index out of bounds: " + std::to_string(idx) +
            " (size: " + std::to_string(arr ? arr->size() : 0) + ")";
        runtime_fatal(error_msg, ie);
      }
      return (*arr)[idx];
    } else if (obj.is_object()) {
      const auto &objMap = obj.get_object();
      if (!objMap) {
        return Value::none();
      }
      std::string key = index.to_string();
      auto it = objMap->find(key);
      if (it == objMap->end()) {
        // no key? return null
        return Value::none();
      }
      return it->second;
    } else if (obj.is_string()) {
      if (!index.is_int()) {
        runtime_fatal("String index must be int", ie);
      }
      int64_t idx = index.get_int();
      const auto &str = obj.get_string();
      if (idx < 0 || idx >= static_cast<int64_t>(str.size())) {
        return Value::none();
      }
      return Value::make_string(std::string(1, str[idx]));
    } else {
      runtime_fatal("Index access requires array, object, or string", ie);
    }
  }

  // member access: obj.member
  case ExprKind::Member: {
    const auto *me = static_cast<const MemberExpr *>(e);
    Value obj = eval_expr(me->object.get());

    // Class instance field or method access
    if (obj.is_class_instance()) {
      const auto &instance = obj.get_class_instance();
      const nari::ClassDecl *class_decl =
          Parser::get_registered_class(instance->class_name);

      if (!class_decl) {
        runtime_fatal("Unknown class: " + instance->class_name, me);
      }

      // check if it's a field in the class hierarchy
      const nari::ClassField *field = find_field_in_hierarchy(class_decl, me->member);
      if (field) {
        // check visibility
        if (field->visibility == nari::Visibility::Private) {
          // we can't access private fields.
          if (current_class_name != instance->class_name) {
            runtime_fatal(
              "Cannot access private field '" + me->member + "' of class " + instance->class_name, 
              me
            );
          }
        }

        auto it = instance->fields->find(me->member);
        if (it != instance->fields->end()) {
          return it->second;
        }
        return Value::none();
      }

      return Value::none();
    }

    if (obj.is_object()) {
      const auto &objMap = obj.get_object();
      if (!objMap) {
        return Value::none();
      }
      auto it = objMap->find(me->member);
      if (it == objMap->end()) {
        return Value::none(); // return null for missing members
      }
      return it->second;
    } else if (obj.is_handle()) {
      const auto &handle = obj.get_handle();
      if (!handle) {
        runtime_fatal("Member access on null handle", me);
      }

      // Handle special members
      if (me->member == "value") {
        // Process tasks and IO cooperatively until this handle completes
        while (handle->state == HandleData::Running) {
          bool did_work = false;

          // Process completed IO operations
          process_completed_io();

          // Process one task if available
          if (!task_queue.empty()) {
            HandlePtr next_task = task_queue.front();
            task_queue.pop();
            step_task(next_task);
            // Re-queue if still running
            if (next_task->state == HandleData::Running) {
              task_queue.push(next_task);
            }
            did_work = true;
          }

          // Process IO events (for HTTP handles and other IO operations)
          if (has_pending_io()) {
            did_work = true;
          }

          // If no work was done and handle is still running, yield briefly
          if (!did_work && handle->state == HandleData::Running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }

        if (handle->state == HandleData::Failed) {
          // Propagate the error
          flags.throw_flag = true;
          flags.throw_value = handle->error;
          return Value::none();
        }

        return handle->result;
      } else if (me->member == "ready") {
        // Check if task is complete without blocking
        // Process any completed IO operations first
        process_completed_io();

        // If still not ready, give IO threads a tiny bit of time
        if (handle->state == HandleData::Running && has_pending_io()) {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
          process_completed_io();
        }

        return Value::make_bool(handle->state != HandleData::Running);
      } else if (me->member == "failed") {
        // Check if task failed
        return Value::make_bool(handle->state == HandleData::Failed);
      } else if (me->member == "error") {
        // Get error value if task failed
        return handle->error;
      } else if (me->member == "duration") {
        // Get duration in milliseconds
        if (handle->state == HandleData::Running) {
          // Still running - return time elapsed so far
          auto now = std::chrono::steady_clock::now();
          auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              now - handle->start_time);
          return Value::make_int(elapsed.count());
        } else {
          // Completed or failed - return total time
          auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              handle->end_time - handle->start_time);
          return Value::make_int(elapsed.count());
        }
      }
    }

    return Value::none();
  }

  // this expression - return current instance context
  case ExprKind::This: {
    if (!current_instance) {
      const auto *te = static_cast<const ThisExpr *>(e);
      runtime_fatal("'this' can only be used inside class methods", te);
    }
    Value v;
    v.data = current_instance;
    return v;
  }

  // new ClassName(args) - create class instance
  case ExprKind::New: {
    const auto *ne = static_cast<const NewExpr *>(e);
    const nari::ClassDecl *class_decl =
        Parser::get_registered_class(ne->class_name);

    if (!class_decl) {
      runtime_fatal("Unknown class: " + ne->class_name, ne);
    }

    // Create new instance
    auto instance = std::make_shared<ClassInstance>(ne->class_name);

    // collect all fields from this class and its parents
    std::vector<const nari::ClassField*> all_fields;
    collect_all_fields(class_decl, all_fields);

    // initialize fields with default values (parent fields first, then child)
    for (const auto *field : all_fields) {
      if (field->default_value) {
        (*instance->fields)[field->name] = eval_expr(field->default_value.get());
      } else {
        (*instance->fields)[field->name] = Value::none();
      }
    }

    // find and call constructor if it exists
    const nari::ClassMethod *constructor = find_method_in_hierarchy(class_decl, "init");
    if (constructor && constructor->is_constructor) {
      // evaluate constructor arguments
      std::vector<Value> arg_values;
      for (const auto &arg_expr : ne->args) {
        arg_values.push_back(eval_expr(arg_expr.get()));
      }

      // check arg count
      if (arg_values.size() != constructor->params.size()) {
        runtime_fatal(
          "Constructor expects " +
          std::to_string(constructor->params.size()) +
          " arguments but got " +
          std::to_string(arg_values.size()),
          ne
        );
      }

      // configure 'this' context and run ctor
      ClassInstancePtr saved_instance = current_instance;
      std::string saved_class = current_class_name;
      current_instance = instance;
      current_class_name = ne->class_name;

      // create new scope for ctor
      call_stack.emplace_back();

      // bind parameters
      for (size_t i = 0; i < constructor->params.size(); ++i) {
        call_stack.back()[constructor->params[i].name] = arg_values[i];
      }

      // run ctor body
      if (constructor->body) {
        for (const auto &stmt : constructor->body->stmts) {
          exec_stmt(stmt.get());
          if (flags.any_flag()) {
            break;
          }
        }
      }

      // restore ctx
      call_stack.pop_back();
      current_instance = saved_instance;
      current_class_name = saved_class;

      // clear flags except throw
      if (flags.return_flag)
        flags.return_flag = false;
    } else if (!ne->args.empty()) {
      runtime_fatal(
        "Class " +
        ne->class_name +
        " has no constructor but arguments were provided",
        ne
      );
    }

    Value result;
    result.data = instance;
    return result;
  }

  default:
    break;
  } // end switch

  // fallback
  return Value::none();
}

// execute a statement AST node
void ScriptRuntime::exec_stmt(const Stmt *s) {
  if (!s) {
    fprintf(stderr, "Runtime trace: exec_stmt received null statement\n");
    return;
  }

  // basic trace info for debugging: file:line:col and a small type hint.
  std::string fn = s->filename.empty() ? std::string("<unknown>") : s->filename;
  if (Runtime::runtime_trace_enabled()) {
    std::string trace_msg = "exec_stmt: " + fn + ":" + std::to_string(s->line) +
                            ":" + std::to_string(s->col);
    Runtime::runtime_log(Runtime::TraceLevel::Debug, trace_msg);
  }

  // dispatch via type tag instead of dynamic_cast
  switch (s->stmt_kind) {

  // variable declaration: `let name = expr` or `global name = expr`
  // or destructuring: `let [a, b] = expr` or `let {x, y} = expr`
  case StmtKind::VarDecl: {
    const auto *varDecl = static_cast<const VarDeclStmt *>(s);
    
    // handle destructuring
    if (varDecl->destructure_kind == nari::DestructureKind::Array) {
      // array destructuring: let [a, b, c] = value
      if (!varDecl->initializerExpr) {
        runtime_fatal("Array destructuring requires initialization", varDecl);
      }
      
      Value val = eval_expr(varDecl->initializerExpr.get());
      if (!val.is_array()) {
        runtime_fatal("Array destructuring requires an array value", varDecl);
      }
      
      const auto &arr = val.get_array();
      for (size_t i = 0; i < varDecl->array_names.size(); i++) {
        Value element = Value::none();
        if (arr && i < arr->size()) {
          element = (*arr)[i];
        }
        
        // declare the variable
        const std::string &name = varDecl->array_names[i];
        if (varDecl->is_global) {
          globals[name] = element;
        } else if (!block_scope_stack.empty()) {
          auto &block_scope = block_scope_stack.back();
          if (block_scope.find(name) != block_scope.end()) {
            runtime_fatal("Variable already declared in current block: '" + name + "'", varDecl);
          }
          block_scope[name] = element;
        } else if (!call_stack.empty()) {
          auto &locals = call_stack.back();
          if (locals.find(name) != locals.end()) {
            runtime_fatal("Variable already declared: '" + name + "'", varDecl);
          }
          locals[name] = element;
        } else {
          if (!module_stack.empty()) {
            const std::string &modfn = module_stack.back();
            auto &module = module_local_vars[modfn];
            if (module.find(name) != module.end()) {
              runtime_fatal("Module-local already declared: '" + name + "'", varDecl);
            }
            module[name] = element;
          } else if (!varDecl->filename.empty()) {
            auto &module = module_local_vars[varDecl->filename];
            if (module.find(name) != module.end()) {
              runtime_fatal("Module-local already declared: '" + name + "'", varDecl);
            }
            module[name] = element;
          } else {
            if (globals.find(name) != globals.end()) {
              runtime_fatal("Global already declared: '" + name + "'", varDecl);
            }
            globals[name] = element;
          }
        }
      }
      return;
    }
    
    if (varDecl->destructure_kind == nari::DestructureKind::Object) {
      // object destructuring: let {a, b: c} = value
      if (!varDecl->initializerExpr) {
        runtime_fatal("Object destructuring requires initialization", varDecl);
      }
      
      Value val = eval_expr(varDecl->initializerExpr.get());
      if (!val.is_object()) {
        runtime_fatal("Object destructuring requires an object value", varDecl);
      }
      
      const auto &obj = val.get_object();
      for (const auto &binding : varDecl->object_bindings) {
        const std::string &key = binding.first;
        const std::string &name = binding.second;
        
        Value element = Value::none();
        if (obj) {
          auto it = obj->find(key);
          if (it != obj->end()) {
            element = it->second;
          }
        }
        
        // declare the variable
        if (varDecl->is_global) {
          globals[name] = element;
        } else if (!block_scope_stack.empty()) {
          auto &block_scope = block_scope_stack.back();
          if (block_scope.find(name) != block_scope.end()) {
            runtime_fatal("Variable already declared in current block: '" + name + "'", varDecl);
          }
          block_scope[name] = element;
        } else if (!call_stack.empty()) {
          auto &locals = call_stack.back();
          if (locals.find(name) != locals.end()) {
            runtime_fatal("Variable already declared: '" + name + "'", varDecl);
          }
          locals[name] = element;
        } else {
          if (!module_stack.empty()) {
            const std::string &modfn = module_stack.back();
            auto &module = module_local_vars[modfn];
            if (module.find(name) != module.end()) {
              runtime_fatal("Module-local already declared: '" + name + "'", varDecl);
            }
            module[name] = element;
          } else if (!varDecl->filename.empty()) {
            auto &module = module_local_vars[varDecl->filename];
            if (module.find(name) != module.end()) {
              runtime_fatal("Module-local already declared: '" + name + "'", varDecl);
            }
            module[name] = element;
          } else {
            if (globals.find(name) != globals.end()) {
              runtime_fatal("Global already declared: '" + name + "'", varDecl);
            }
            globals[name] = element;
          }
        }
      }
      return;
    }
    
    // simple variable declaration
    Value val = Value::none();
    if (varDecl->initializerExpr)
      val = eval_expr(varDecl->initializerExpr.get());

    if (varDecl->is_global) {
      // declare or overwrite a global explicitly requested by `global`
      globals[varDecl->name] = val;
    } else {
      // normal `let` semantics: create a block-local if in a block,
      // otherwise a function-local if inside a function,
      // otherwise create a module-local for top-level `let`.
      if (!block_scope_stack.empty()) {
        // Declare in current (innermost) block scope
        auto &block_scope = block_scope_stack.back();
        // Disallow redeclaration in the same block scope
        if (block_scope.find(varDecl->name) != block_scope.end()) {
          std::string error_msg =
              "Variable already declared in current block: '" + varDecl->name +
              "'";
          runtime_fatal(error_msg, varDecl);
        }
        block_scope[varDecl->name] = val;
      } else if (!call_stack.empty()) {
        auto &locals = call_stack.back();
        // disallow redeclaration of an existing local name
        if (locals.find(varDecl->name) != locals.end()) {
          std::string error_msg =
              "Variable already declared: '" + varDecl->name + "'";
          runtime_fatal(error_msg, varDecl);
        }
        locals[varDecl->name] = val;
      } else {
        // top-level let: store as module-local variables keyed by the
        // current module context (module_stack).
        // If not available, fall back to the declaration's filename
        // and if that's not available, fall back to globals.
        if (!module_stack.empty()) {
          const std::string &modfn = module_stack.back();
          auto &module = module_local_vars[modfn];
          if (module.find(varDecl->name) != module.end()) {
            std::string error_msg =
                "Module-local already declared: '" + varDecl->name + "'";
            runtime_fatal(error_msg, varDecl);
          }
          module[varDecl->name] = val;
        } else if (!varDecl->filename.empty()) {
          auto &module = module_local_vars[varDecl->filename];
          if (module.find(varDecl->name) != module.end()) {
            std::string error_msg =
                "Module-local already declared: '" + varDecl->name + "'";
            runtime_fatal(error_msg, varDecl);
          }
          module[varDecl->name] = val;
        } else {
          // fallback to global if no module context or filename is available.
          if (globals.find(varDecl->name) != globals.end()) {
            std::string error_msg =
                "Global already declared: '" + varDecl->name + "'";
            runtime_fatal(error_msg, varDecl);
          }
          globals[varDecl->name] = val;
        }
      }
    }
    return;
  }

  case StmtKind::Expr: {
    const auto *es = static_cast<const ExprStmt *>(s);
    eval_expr(es->expr.get());
    return;
  }
  case StmtKind::Assign: {
    const auto *as = static_cast<const AssignStmt *>(s);
    if (!as->value) {
      std::string error_msg =
          "Attempt to assign from null expression for target '" + as->target +
          "'";
      runtime_fatal(error_msg, as);
    }
    Value v = eval_expr(as->value.get());
    // check if variable exists in block scopes
    for (int i = block_scope_stack.size() - 1; i >= 0; i--) {
      auto &block_scope = block_scope_stack[i];
      auto blkTarget = block_scope.find(as->target);
      if (blkTarget != block_scope.end()) {
        blkTarget->second = std::move(v);
        return;
      }
    }
    // store in current function locals if exists, else module-local or globals
    if (!call_stack.empty()) {
      call_stack.back()[as->target] = std::move(v);
    } else {
      if (!module_stack.empty()) {
        auto &mmap = module_local_vars[module_stack.back()];
        mmap[as->target] = std::move(v);
      } else if (!as->filename.empty()) {
        auto &mmap = module_local_vars[as->filename];
        mmap[as->target] = std::move(v);
      } else {
        globals[as->target] = std::move(v);
      }
    }
    return;
  }

  // indexed assignment: arr[i] = val or obj.key = val
  case StmtKind::IndexAssign: {
    const auto *indexAssignStmt = (const IndexAssignStmt *)s;
    if (!indexAssignStmt->target || !indexAssignStmt->value) {
      runtime_fatal("Indexed assignment has null target or value",
                    indexAssignStmt);
    }

    Value val = eval_expr(indexAssignStmt->value.get());

    // handle IndexExpr target: arr[index] = val or obj[key] = val
    if (indexAssignStmt->target->kind == ExprKind::Index) {
      const auto *ie =
          static_cast<const IndexExpr *>(indexAssignStmt->target.get());
      Value obj = eval_expr(ie->object.get());
      Value index = eval_expr(ie->index.get());

      if (obj.is_array()) {
        auto &arr = obj.get_array();
        if (!index.is_int()) {
          runtime_fatal("Array index must be int", indexAssignStmt);
        }
        int64_t idx = index.get_int();
        if (!arr || idx < 0 || idx >= static_cast<int64_t>(arr->size())) {
          std::string error_msg =
              "Array index out of bounds: " + std::to_string(idx) +
              " (size: " + std::to_string(arr ? arr->size() : 0) + ")";
          runtime_fatal(error_msg, indexAssignStmt);
        }
        (*arr)[idx] = val;
      } else if (obj.is_object()) {
        auto &objMap = obj.get_object();
        if (!objMap) {
          runtime_fatal("Cannot assign to null object", indexAssignStmt);
        }
        std::string key = index.to_string();
        (*objMap)[key] = val;
      } else {
        runtime_fatal("Indexed assignment requires array or object",
                      indexAssignStmt);
      }
    }
    // handle MemberExpr target: obj.member = val
    else if (indexAssignStmt->target->kind == ExprKind::Member) {
      const auto *me =
          static_cast<const MemberExpr *>(indexAssignStmt->target.get());
      Value obj = eval_expr(me->object.get());

      if (obj.is_class_instance()) {
        const auto &instance = obj.get_class_instance();
        const nari::ClassDecl *class_decl =
            Parser::get_registered_class(instance->class_name);

        if (!class_decl) {
          runtime_fatal("Unknown class: " + instance->class_name,
                        indexAssignStmt);
        }

        // find the field in class hierarchy
        const nari::ClassField *field = find_field_in_hierarchy(class_decl, me->member);
        
        if (!field) {
          runtime_fatal(
            "Class " +
            instance->class_name +
            " has no field '" +
            me->member + "'",
            indexAssignStmt
          );
        }

        // check visibility
        if (field->visibility == nari::Visibility::Private) {
          if (current_class_name != instance->class_name) {
            runtime_fatal(
              "Cannot assign to private field '" + me->member +
              "' of class " + instance->class_name,
              indexAssignStmt
            );
          }
        }

        (*instance->fields)[me->member] = val;
      } else if (obj.is_object()) {
        auto &objMap = obj.get_object();
        if (!objMap) {
          runtime_fatal("Cannot assign to null object", indexAssignStmt);
        }
        (*objMap)[me->member] = val;
      } else {
        runtime_fatal("Member assignment requires object or class instance",
                      indexAssignStmt);
      }
    } else {
      runtime_fatal("Indexed assignment target must be IndexExpr or MemberExpr",
                    indexAssignStmt);
    }
    return;
  }

  case StmtKind::Block: {
    const auto *blockStmt = (const BlockStmt *)s;
    // push new block scope
    push_block_scope();

    for (const auto &stmt : blockStmt->stmts) {
      exec_stmt(stmt.get());

      if (flags.any_flag())
        break;
    }

    // pop block scope
    pop_block_scope();
    return;
  }
  case StmtKind::If: {
    const auto *ifStmt = static_cast<const IfStmt *>(s);
    // evaluate condition and execute appropriate branch
    bool take = false;
    if (ifStmt->cond)
      take = eval_expr(ifStmt->cond.get()).as_bool();
    if (take) {
      if (ifStmt->then_branch)
        exec_stmt(ifStmt->then_branch.get());
    } else {
      if (ifStmt->else_branch)
        exec_stmt(ifStmt->else_branch.get());
    }
    return;
  }
  case StmtKind::While: {
    const auto *whileStmt = static_cast<const WhileStmt *>(s);

    while (true) {
      bool ok = true;
      if (whileStmt->cond)
        ok = eval_expr(whileStmt->cond.get()).as_bool();
      if (!ok)
        break;
      // clear continue flag at loop top
      flags.continue_flag = false;
      exec_stmt(whileStmt->body.get());
      if (flags.return_flag)
        return;
      if (flags.throw_flag)
        return;
      if (flags.shutdown_flag)
        return;
      // check for shutdown and break
      if (Runtime::g_shutdown_requested.load())
        break;
      if (Runtime::g_runtime_error_occurred.load())
        break;
      if (flags.break_flag) {
        flags.break_flag = false;
        break;
      }
      if (flags.continue_flag) {
        flags.continue_flag = false;
        continue;
      }
    }
    return;
  }
  case StmtKind::ForEach: {
    const auto *forEachStmt = static_cast<const ForEachStmt *>(s);
    Value iterable = eval_expr(forEachStmt->iterable.get());
    if (!iterable.is_array()) {
      runtime_fatal("for-each requires an array", forEachStmt);
    }

    auto assign_var = [&](const std::string &name, Value val) {
      if (!call_stack.empty()) {
        call_stack.back()[name] = std::move(val);
      } else if (!module_stack.empty()) {
        module_local_vars[module_stack.back()][name] = std::move(val);
      } else if (!forEachStmt->filename.empty()) {
        module_local_vars[forEachStmt->filename][name] = std::move(val);
      } else {
        globals[name] = std::move(val);
      }
    };

    const auto &arr_ptr = iterable.get_array();
    if (!arr_ptr) {
      runtime_fatal("for-each on null array", forEachStmt);
    }
    for (const auto &item : *arr_ptr) {
      assign_var(forEachStmt->var, item);
      // clear continue flag at loop top
      flags.continue_flag = false;
      exec_stmt(forEachStmt->body.get());
      if (flags.return_flag)
        return;
      if (flags.throw_flag)
        return;
      if (flags.shutdown_flag)
        return;
      // check for shutdown and break
      if (Runtime::g_shutdown_requested.load())
        break;
      if (Runtime::g_runtime_error_occurred.load())
        break;
      if (flags.break_flag) {
        flags.break_flag = false;
        break;
      }
      if (flags.continue_flag) {
        flags.continue_flag = false;
        continue;
      }
    }
    return;
  }
  case StmtKind::Switch: {
    const auto *switchStmt = (const SwitchStmt *)s;
    Value target = eval_expr(switchStmt->value.get());

    auto values_equal = [&](const Value &a, const Value &b) {
      if (a.is_int() && b.is_int()) {
        return a.get_int() == b.get_int();
      }
      if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
        return std::fabs(a.as_number() - b.as_number()) < 1e-12;
      }
      return a.to_string() == b.to_string();
    };

    auto is_empty_body = [](const BlockPtr &body) -> bool {
      return !body ||
             (dynamic_cast<const BlockStmt *>(body.get()) &&
              dynamic_cast<const BlockStmt *>(body.get())->stmts.empty());
    };

    // Find the first matching case
    for (size_t i = 0; i < switchStmt->cases.size(); i++) {
      Value match = eval_expr(switchStmt->cases[i].match.get());
      if (values_equal(target, match)) {
        // Found a match - now find the first non-empty body from this point
        for (size_t j = i; j < switchStmt->cases.size(); j++) {
          if (!is_empty_body(switchStmt->cases[j].body)) {
            exec_stmt(switchStmt->cases[j].body.get());
            if (flags.break_flag)
              flags.break_flag = false;
            return;
          }
        }
        // If we reach here, all remaining cases are empty, try default
        if (switchStmt->default_body &&
            !is_empty_body(switchStmt->default_body)) {
          exec_stmt(switchStmt->default_body.get());
          if (flags.break_flag)
            flags.break_flag = false;
        }
        return;
      }
    }

    // No match found, execute default if it exists
    if (switchStmt->default_body) {
      exec_stmt(switchStmt->default_body.get());
      if (flags.break_flag)
        flags.break_flag = false;
    }
    return;
  }
  case StmtKind::For: {
    const auto *forStmt = (const ForStmt *)s;
    push_block_scope();

    if (forStmt->init)
      exec_stmt(forStmt->init.get());

    while (true) {
      bool ok = true;
      if (forStmt->cond)
        ok = eval_expr(forStmt->cond.get()).as_bool();
      if (!ok)
        break;
      // clear continue flag at loop top
      flags.continue_flag = false;
      exec_stmt(forStmt->body.get());
      if (flags.return_flag)
        return;
      if (flags.throw_flag)
        break;
      // check for runtime error and stop execution
      if (Runtime::g_runtime_error_occurred.load())
        break;
      if (flags.break_flag) {
        flags.break_flag = false;
        break;
      }
      if (flags.continue_flag) {
        flags.continue_flag = false;

        if (forStmt->post)
          exec_stmt(forStmt->post.get());
        continue;
      }

      if (forStmt->post)
        exec_stmt(forStmt->post.get());
    }
    pop_block_scope();
    return;
  }

  case StmtKind::Break:
    flags.break_flag = true;
    return;

  case StmtKind::Continue:
    flags.continue_flag = true;
    return;

  case StmtKind::Return: {
    const auto *returnStmt = (const nari::ReturnStmt *)s;
    flags.return_flag = true;
    if (returnStmt->value) {
      flags.return_value = eval_expr(returnStmt->value.get());
    } else {
      flags.return_value = Value::none();
    }
    return;
  }

  case StmtKind::Throw: {
    const auto *throwStmt = (const nari::ThrowStmt *)s;
    if (throwStmt->value) {
      flags.throw_value = eval_expr(throwStmt->value.get());
    } else {
      flags.throw_value = Value::none();
    }
    flags.throw_flag = true;
    return;
  }

  case StmtKind::Try: {
    const auto *tryStmt = (const nari::TryStmt *)s;
    auto assign_var = [&](const std::string &name, Value val) {
      if (!call_stack.empty()) {
        call_stack.back()[name] = std::move(val);
      } else if (!module_stack.empty()) {
        module_local_vars[module_stack.back()][name] = std::move(val);
      } else if (!tryStmt->filename.empty()) {
        module_local_vars[tryStmt->filename][name] = std::move(val);
      } else {
        globals[name] = std::move(val);
      }
    };

    exec_stmt(tryStmt->try_block.get());

    bool pending_throw = flags.throw_flag;
    Value pending_value = flags.throw_value;
    if (pending_throw) {
      flags.throw_flag = false;
      flags.throw_value = Value::none();
    }

    if (pending_throw && tryStmt->catch_block) {
      if (!tryStmt->catch_var.empty()) {
        assign_var(tryStmt->catch_var, pending_value);
      }
      exec_stmt(tryStmt->catch_block.get());
      pending_throw = false;
    }

    if (tryStmt->finally_block) {
      exec_stmt(tryStmt->finally_block.get());
    }

    if (pending_throw && !flags.throw_flag) {
      flags.throw_flag = true;
      flags.throw_value = pending_value;
    }
    return;
  }

  default:
    break;
  } // end switch

  fprintf(stderr, "Unhandled statement type in exec_stmt %s! This is a bug!\n",
          demangle(typeid(*s).name()).c_str());
}

Value ScriptRuntime::call_user_function(Function *func,
                                        const std::vector<Value> &args) {
  if (!func) {
    fprintf(stderr,
            "Runtime trace: call_user_function called with null Function*\n");
    return Value::none();
  }

  if (Runtime::runtime_trace_enabled()) {
    std::string trace_msg = "enter function: " + func->name + " @ " +
                            func->filename + ":" + std::to_string(func->line) +
                            ":" + std::to_string(func->col);
    Runtime::runtime_log(Runtime::TraceLevel::Debug, trace_msg);
  }

  auto saved_scope_closure = current_scope_closure;
  current_scope_closure = nullptr;

  // every function gets a clean block scope stack
  auto saved_block_scopes = block_scope_stack;
  block_scope_stack.clear();

  // push module context
  module_stack.push_back(func->filename.empty() ? std::string("<unknown>")
                                                : func->filename);
  func_stack.push_back(func->name);

  // create locals frame
  call_stack.emplace_back();
  auto &locals = call_stack.back();

  // store ref to closure environment for proper variable updates
  std::shared_ptr<std::unordered_map<std::string, Value>> closure_env_ref;

  // copy captured environment to local scope
  if (func->closure_env_ptr) {
    closure_env_ref =
        *static_cast<std::shared_ptr<std::unordered_map<std::string, Value>> *>(
            func->closure_env_ptr);
    for (const auto &[var_name, var_value] : *closure_env_ref) {
      locals[var_name] = var_value;
    }
  }

  // bind parameters by position (parameters are local variables and are
  // therefore considered declared in the local frame)
  size_t arg_index = 0;
  for (size_t i = 0; i < func->params.size(); ++i) {
    const auto &param = func->params[i];
    if (param.is_rest) {
      std::vector<Value> rest_values;
      for (size_t j = arg_index; j < args.size(); ++j) {
        rest_values.push_back(args[j]);
      }
      locals[param.name] = Value::make_array(std::move(rest_values));
      arg_index = args.size();
    } else if (arg_index < args.size()) {
      locals[param.name] = args[arg_index++];
    } else if (param.default_value) {
      locals[param.name] = eval_expr(param.default_value.get());
    } else {
      locals[param.name] = Value::none();
    }
  }

  // clear return flag and value before executing function body
  flags.return_flag = false;
  flags.return_value = Value::none();

  if (func->function_expr && func->function_expr->body) {
    // lambda function created from FunctionExpr
    for (const auto &st : func->function_expr->body->stmts) {
      if (!st) {
        fprintf(stderr, "Runtime trace: skipping null statement in lambda %s\n",
                func->name.c_str());
        continue;
      }
      exec_stmt(st.get());

      if (flags.return_flag)
        break;
      if (flags.throw_flag)
        break;

      // small precaution just in case we somehow leak a break/continue flag
      if (flags.break_flag)
        flags.break_flag = false;
      if (flags.continue_flag)
        flags.continue_flag = false;
    }
  } else if (func->body) {
    // regular function with copied/cloned body
    for (const auto &st : func->body->stmts) {
      if (!st) {
        fprintf(stderr,
                "Runtime trace: skipping null statement in function %s\n",
                func->name.c_str());
        continue;
      }
      exec_stmt(st.get());
      // stop executing on return
      if (flags.return_flag)
        break;
      if (flags.throw_flag)
        break;

      // small precaution just in case we somehow leak a break/continue flag
      if (flags.break_flag)
        flags.break_flag = false;
      if (flags.continue_flag)
        flags.continue_flag = false;
    }
  } else {
    fprintf(stderr, "Runtime trace: function %s has no body\n",
            func->name.c_str());
  }

  // capture return value before popping the frame
  Value result = flags.return_value;
  flags.return_flag = false;
  flags.return_value = Value::none();

  if (closure_env_ref) {
    // do not move this line anywhere else!
    // i moved it up like 2 lines in a refactor once
    // and spent 4 hours debugging why the callstack would get corrupted and
    // explode
    auto &locals_final = call_stack.back();

    for (const auto &[var_name, var_value] : locals_final) {
      if (closure_env_ref->find(var_name) != closure_env_ref->end()) {
        (*closure_env_ref)[var_name] = var_value;
      }
    }
  }

  // pop frame and module context
  call_stack.pop_back();
  if (!func_stack.empty())
    func_stack.pop_back();
  if (!module_stack.empty())
    module_stack.pop_back();

  // Restore previous block scope stack and scope closure
  block_scope_stack = saved_block_scopes;
  current_scope_closure = saved_scope_closure;

  if (Runtime::runtime_trace_enabled()) {
    std::string trace_msg = "exit function: " + func->name;
    Runtime::runtime_log(Runtime::TraceLevel::Debug, trace_msg);
  }

  return result;
}

namespace Runtime {

void run_program_with_runtime(
    std::vector<std::unique_ptr<nari::Function>> &funcs) {
  Parser::set_source_filename("<embedded_stdlib>");
  std::string embedded = nari_embedded_stdlib();
  auto stdlib_funcs = Parser::parse_program_from_source(embedded, false);

  // combine stdlib functions first, then user functions
  // this means user code can override stdlib names, but i will be sad if you do
  // that.
  std::vector<std::unique_ptr<nari::Function>> combined;
  combined.reserve(stdlib_funcs.size() + funcs.size());
  for (auto &f : stdlib_funcs)
    combined.push_back(std::move(f));
  for (auto &f : funcs)
    combined.push_back(std::move(f));

  try {
    ScriptRuntime rt(combined);
    rt.run_top_level();
  } catch (const RuntimeError &err) {
    // runtime error occurred, at this point error details have already been
    // printed to stderr just rethrow to let caller decide what to do
    throw;
  }
}

} // namespace Runtime
