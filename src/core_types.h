#pragma once

#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ast.h"

namespace chrono = std::chrono;

using namespace nari;

template <class... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

struct IntervalData {
  int64_t id;
  std::string callback_name;
  int64_t interval_ms;
  chrono::steady_clock::time_point next_fire;
};

struct Value;
struct Task;

using ArrayData = std::shared_ptr<std::vector<Value>>;
using ObjectData = std::shared_ptr<std::map<std::string, Value>>;

struct HandleData;
using HandlePtr = std::shared_ptr<HandleData>;

using Array = std::shared_ptr<std::vector<Value>>;
using Object = std::shared_ptr<std::unordered_map<std::string, Value>>;

struct ValueFunction {
  std::string name;
};

// Class instance with reference to class name and field storage
struct ClassInstance {
  std::string class_name;
  std::shared_ptr<std::unordered_map<std::string, Value>> fields;

  ClassInstance(std::string cn) : class_name(std::move(cn)) {
    fields = std::make_shared<std::unordered_map<std::string, Value>>();
  }
};

using ClassInstancePtr = std::shared_ptr<ClassInstance>;

using ValueData =
    std::variant<std::monostate, std::string, int64_t, double, bool, Array,
                 Object, ValueFunction, HandlePtr, ClassInstancePtr>;

struct Value {
  ValueData data;

  Value() : data(std::monostate{}) {}
  Value(ValueData d) : data(std::move(d)) {}

  static Value none() { return Value(); }
  static Value make_int(int64_t v) {
    Value val;
    val.data = v;
    return val;
  }
  static Value make_float(double v) {
    Value val;
    val.data = v;
    return val;
  }
  static Value make_bool(bool v) {
    Value val;
    val.data = v;
    return val;
  }
  static Value make_string(std::string v) {
    Value val;
    val.data = std::move(v);
    return val;
  }

  static Value make_array();
  static Value make_array(std::vector<Value> elements);
  static Value make_object();
  static Value make_object(Object entries);

  static Value make_function(std::string name) {
    Value val;
    val.data = ValueFunction{std::move(name)};
    return val;
  }
  static Value make_handle(HandlePtr h);
  static HandlePtr make_handle_ptr();

  static Value make_class_instance(std::string class_name) {
    Value val;
    val.data = std::make_shared<ClassInstance>(std::move(class_name));
    return val;
  }

  // type checking helpers
  bool is_none() const { return std::holds_alternative<std::monostate>(data); }
  bool is_string() const { return std::holds_alternative<std::string>(data); }
  bool is_int() const { return std::holds_alternative<int64_t>(data); }
  bool is_float() const { return std::holds_alternative<double>(data); }
  bool is_bool() const { return std::holds_alternative<bool>(data); }
  bool is_array() const { return std::holds_alternative<Array>(data); }
  bool is_object() const { return std::holds_alternative<Object>(data); }
  bool is_function() const {
    return std::holds_alternative<ValueFunction>(data);
  }
  bool is_handle() const { return std::holds_alternative<HandlePtr>(data); }
  bool is_class_instance() const {
    return std::holds_alternative<ClassInstancePtr>(data);
  }

  // check the type before you use these!
  const std::string &get_string() const { return std::get<std::string>(data); }
  std::string &get_string() { return std::get<std::string>(data); }
  int64_t get_int() const { return std::get<int64_t>(data); }
  double get_float() const { return std::get<double>(data); }
  bool get_bool() const { return std::get<bool>(data); }
  const Array &get_array() const { return std::get<Array>(data); }
  Array &get_array() { return std::get<Array>(data); }
  const Object &get_object() const { return std::get<Object>(data); }
  Object &get_object() { return std::get<Object>(data); }
  const ValueFunction &get_function() const {
    return std::get<ValueFunction>(data);
  }
  const HandlePtr &get_handle() const { return std::get<HandlePtr>(data); }
  const ClassInstancePtr &get_class_instance() const {
    return std::get<ClassInstancePtr>(data);
  }
  ClassInstancePtr &get_class_instance() {
    return std::get<ClassInstancePtr>(data);
  }

  std::string to_string(bool in_container_context = false) const {
    return std::visit(
        overloaded{[](std::monostate) { return std::string("<null>"); },
                   [in_container_context](const std::string &s) {
                     if (!in_container_context) {
                       return s;
                     }

                     std::string result;
                     result.reserve(s.size() + 2);
                     result += '"';
                     result += s;
                     result += '"';
                     return result;
                   },
                   [](int64_t i) { return std::to_string(i); },
                   [](double f) {
                     char buf[64];
                     auto [p, _] = std::to_chars(buf, buf + 64, f);
                     return std::string(buf, p);
                   },
                   [](bool b) {
                     return b ? std::string("true") : std::string("false");
                   },
                   [](const Array &a) {
                     std::string r = "[";
                     if (a) {
                       for (size_t i = 0; i < a->size(); ++i) {
                         if (i)
                           r += ", ";
                         r += (*a)[i].to_string(true);
                       }
                     }
                     r += "]";
                     return r;
                   },
                   [](const Object &o) {
                     std::string r = "{";
                     bool first = true;
                     if (o) {
                       for (auto &[k, v] : *o) {
                         if (!first)
                           r += ", ";
                         first = false;
                         r += k + ": " + v.to_string(true);
                       }
                     }
                     r += "}";
                     return r;
                   },
                   [](const ValueFunction &f) {
                     return std::string("<function " + f.name + ">");
                   },
                   [](const HandlePtr &) { return std::string("<handle>"); },
                   [](const ClassInstancePtr &ci) {
                     return std::string("<" + ci->class_name + " instance>");
                   }},
        data);
  }

  double as_number() const {
    return std::visit(
        overloaded{[](std::monostate) { return 0.0; },
                   [](const std::string &s) {
                     if (s.empty())
                       return 0.0;
                     char *end;
                     double result = std::strtod(s.c_str(), &end);
                     return (*end == '\0') ? result : 0.0;
                   },
                   [](int64_t i) { return static_cast<double>(i); },
                   [](double f) { return f; },
                   [](bool b) { return b ? 1.0 : 0.0; },
                   [](const Array &a) {
                     return a ? static_cast<double>(a->size()) : 0.0;
                   },
                   [](const Object &o) {
                     return o ? static_cast<double>(o->size()) : 0.0;
                   },
                   [](const ValueFunction &) { return 0.0; },
                   [](const HandlePtr &) { return 0.0; },
                   [](const ClassInstancePtr &) { return 0.0; }},
        data);
  }

  bool as_bool() const {
    return std::visit(
        overloaded{[](std::monostate) { return false; },
                   [](const std::string &s) { return !s.empty(); },
                   [](int64_t i) { return i != 0; },
                   [](double f) { return f != 0.0; }, [](bool b) { return b; },
                   [](const Array &a) { return a && !a->empty(); },
                   [](const Object &o) { return o && !o->empty(); },
                   [](const ValueFunction &f) { return !f.name.empty(); },
                   [](const HandlePtr &h) { return h != nullptr; },
                   [](const ClassInstancePtr &ci) { return ci != nullptr; }},
        data);
  }
};

struct Flags {
  bool break_flag = false;
  bool continue_flag = false;
  bool return_flag = false;
  Value return_value;
  bool throw_flag = false;
  Value throw_value;
  bool shutdown_flag = false;

  bool any_flag() {
    return break_flag || continue_flag || return_flag || throw_flag ||
           shutdown_flag;
  };
};

// async work in progress for spawn blocks
struct HandleData {
  enum State { Running, Completed, Failed };
  State state = Running;
  Value result;
  Value error;
  std::unique_ptr<Task> task;

  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point end_time;

  HandleData() : start_time(std::chrono::steady_clock::now()) {}
};

// cooperative multitasking chunk
struct Task {
  enum State { Running, Yielded, Completed, Failed };

  State state = Running;

  const BlockStmt *body = nullptr;
  size_t current_stmt = 0;
  Value result;
  Value error;

  std::unordered_map<std::string, Value> locals;
  std::vector<std::unordered_map<std::string, Value>> block_scopes;

  Flags flags;

  Task(const BlockStmt *b) : body(b) {}
};
