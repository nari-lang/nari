#pragma once

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast.h"
#include "gc.h"

namespace chrono = std::chrono;
using namespace nari;

struct Value;
struct Task;
struct HeapHeader;
struct StringObj;
struct ArrayObj;
struct ObjectObj;
struct FunctionData;
struct HandleData;
struct ClassInstance;
struct RegexObj;
struct DelegateData;
struct ObjectDict;
struct ObjectShape;
struct ClassLayout;

namespace nari {
struct Function;
namespace bytecode {
struct FunctionMeta;
}
} // namespace nari

using Array = std::vector<Value>;
using Object = ObjectObj *;
using HandlePtr = HandleData *;
using ClassInstancePtr = ClassInstance *;
using CapturesList = std::shared_ptr<std::vector<std::shared_ptr<Value>>>;
using FuncList = std::vector<nari::FunctionPtr>;

enum class ValueTag : uint8_t {
    String = 4,
    Array = 5,
    Object = 6,
    Function = 7,
    Handle = 8,
    ClassInstance = 9,
    Regex = 10,
    Delegate = 11,
};

enum class JitInlineKind : int32_t {
    None = 0,
    IntAdd = 1,
    IntSub = 2,
    IntMul = 3,
    MulConst = 4,
    AddConst = 5,
    SubConst = 6,
    ClosureInc = 7,
    ClosureAddConst = 8,
    Identity = 9,
    Negate = 10,
    LT = 11,
    LE = 12,
    GT = 13,
    GE = 14,
    EQ = 15,
    NE = 16,
};

inline int32_t to_int(JitInlineKind k) {
    return static_cast<int32_t>(k);
}

struct HeapHeader {
    // Objects are reclaimed solely by the mark-sweep GC.
    ValueTag type_tag = ValueTag::String;
    bool gc_marked = false;
    bool gc_tracked = false;
    uint32_t gc_est = 0;
    HeapHeader *gc_next = nullptr;
    HeapHeader *gc_prev = nullptr;
};

// CRTP mixin that routes a heap type's allocations through a bounded header
// free-list pool (see core_types.cpp), replacing the malloc/free round-trip on
// the hot path. A derived type opts in with:
//
//     struct Foo : HeapHeader, PooledHeapObject<Foo> {
//         ...
//     };
//
// This is an empty base, so with HeapHeader listed first the layout is
// unchanged and HeapHeader stays at offset 0 (Value::heap_ptr relies on that).
// The sized operators are defined out-of-line and explicitly instantiated in
// core_types.cpp, which keeps the pool implementation private to that file.
template <class Derived>
struct PooledHeapObject {
    static void *operator new(std::size_t sz);
    static void operator delete(void *ptr) noexcept;
    // Keep placement-new usable (jit_layout.h probes layout with `new (buf) T{}`);
    // declaring the sized operators above would otherwise hide it.
    static void *operator new(std::size_t, void *p) noexcept {
        return p;
    }
    static void operator delete(void *, void *) noexcept {
    }
};

struct Value {
    static constexpr uint64_t NB_HEAP_TAG = 0xFFFB000000000000ULL;
    static constexpr uint64_t NB_INT_TAG = 0xFFFC000000000000ULL;
    static constexpr uint64_t NB_BOOL_TAG = 0xFFFE000000000000ULL;
    static constexpr uint64_t NB_NONE = 0xFFFF000000000000ULL;
    static constexpr uint64_t PTR_MASK = 0x0000FFFFFFFFFFFFULL;
    static constexpr uint16_t TAG_HEAP = 0xFFFB;
    static constexpr uint16_t TAG_INT = 0xFFFC;
    static constexpr uint16_t TAG_BOOL = 0xFFFE;
    static constexpr uint16_t TAG_NONE = 0xFFFF;
    static constexpr int64_t INT48_MIN = -(1LL << 47);
    static constexpr int64_t INT48_MAX = (1LL << 47) - 1;

    uint64_t _raw = NB_NONE;

    Value() = default;
    Value(const Value &o) = default;
    // Value is a NaN-boxed uint64_t with no ownership (heap lifetime is managed
    // solely by the mark-sweep GC). Moves are therefore plain copies: defaulting
    // them keeps Value trivially copyable, so std::vector growth / frame setup can
    // relocate elements with memcpy instead of an element-wise move loop.
    Value(Value &&o) noexcept = default;
    Value &operator=(const Value &o) = default;
    Value &operator=(Value &&o) noexcept = default;

    static Value from_raw(uint64_t raw);
    uint64_t raw_bits() const;

    static Value none();
    static bool fits_int48(int64_t v);
    static Value make_int(int64_t v);
    static Value make_int_checked(int64_t v);
    static Value make_float(double v);
    static Value make_bool(bool v);
    static Value make_string(std::string v);
    static Value make_const_string(const std::string &v);
    static Value make_array();
    static Value make_array(std::vector<Value> elements);
    static Value make_object();
    static Value make_object(ObjectObj *entries);
    static Value make_function(std::string name);
    static Value make_function(std::string name, std::shared_ptr<nari::Function> func_ptr);
    static HandlePtr make_handle_ptr();
    static Value make_handle(HandlePtr h);
    static Value from_class_instance(ClassInstancePtr p);
    static Value make_regex(std::string pattern, std::string flags);
    static Value make_delegate(Value target, Value handler);

    uint16_t tag_word() const;
    bool is_none() const;
    bool is_int() const;
    bool is_bool() const;
    bool is_float() const;
    bool is_numeric() const;
    bool is_heap() const;
    HeapHeader *heap_ptr() const;
    ValueTag heap_tag() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;
    bool is_function() const;
    bool is_handle() const;
    bool is_class_instance() const;
    bool is_regex() const;
    bool is_delegate() const;
    bool is_sso() const;
    uint8_t sso_len() const;
    char sso_char(uint8_t) const;
    bool is_mutable_heap_string() const;

    int64_t get_int() const;
    double get_float() const;
    bool get_bool() const;
    const std::string &get_string() const;
    std::string &get_string();
    const Array &get_array() const;
    Array &get_array();
    ObjectObj *get_obj_ptr();
    const ObjectObj *get_obj_ptr() const;
    ObjectObj *get_object();
    const ObjectObj *get_object() const;
    FunctionData &get_function();
    const FunctionData &get_function() const;
    HandlePtr get_handle() const;
    ClassInstancePtr get_class_instance() const;
    RegexObj *get_regex();
    const RegexObj *get_regex() const;
    DelegateData *get_delegate() const;

    void inplace_int(int64_t v);
    void inplace_int_checked(int64_t v);
    void set_int(int64_t v);
    void set_bool(bool v);
    void set_float(double v);
    void inplace_float(double v);
    size_t tag() const;

    double as_number() const;
    bool as_bool() const;
    std::string to_string(bool in_container_context = false) const;
    std::string to_string_impl(bool in_container_context, std::vector<const void *> &seen) const;
    static bool values_equal(const Value &a, const Value &b);
};

static_assert(sizeof(Value) == sizeof(uint64_t), "Value must stay NaN-boxed to 8 bytes!");
// Keep Value trivially copyable so vector growth / call-frame setup relocate via
// memcpy. If you add ownership (e.g. an intrusive refcount + destructor), this
// breaks intentionally so the perf-relevant move semantics get re-reviewed.
static_assert(std::is_trivially_copyable<Value>::value, "Value must stay trivially copyable for memcpy relocation");

struct StringObj : HeapHeader, PooledHeapObject<StringObj> {
    std::string s;
    bool immutable = false;
    StringObj() {
        type_tag = ValueTag::String;
    }
    explicit StringObj(std::string v) : s(std::move(v)) {
        type_tag = ValueTag::String;
    }
    // StringObj is by far the most churned heap object (toString / split tokens /
    // concat temps). Every make_string() does one new StringObj and the GC sweep
    // one delete. The PooledHeapObject free-list recycles just the fixed-size
    // header block (the std::string is still constructed/destructed normally, so
    // bytes are unchanged), removing the malloc/free round-trip on the hot path.
};

struct ArrayObj : HeapHeader, PooledHeapObject<ArrayObj> {
    std::vector<Value> v;
    ArrayObj() {
        type_tag = ValueTag::Array;
    }
    // Header free-list pool (see PooledHeapObject in core_types.cpp). Only the
    // fixed-size header block is recycled; the std::vector is
    // constructed/destructed normally, so bytes are unchanged.
};

uint32_t intern_field(const std::string &name);
const std::string &field_name(uint32_t id);

struct ObjectShape {
    std::vector<std::string> names;
    std::unordered_map<uint32_t, uint32_t> index;
};

class ObjectShapeRegistry {
  public:
    ObjectShapeRegistry();
    const ObjectShape *empty_shape() const;
    const ObjectShape *extend(const ObjectShape *base, uint32_t fid);

  private:
    const ObjectShape *empty = nullptr;
    std::vector<std::unique_ptr<ObjectShape>> shapes;
    std::unordered_map<std::string, const ObjectShape *> cache;
};

std::unordered_map<std::string, uint32_t> &field_intern_map();
std::vector<std::string> &field_intern_names();
ObjectShapeRegistry &object_shape_registry();
void delete_object_dict(ObjectDict *p) noexcept;

struct ObjectObj : HeapHeader, PooledHeapObject<ObjectObj> {
    static constexpr size_t kDictModeThreshold = 32;

    const ObjectShape *shape = nullptr;
    std::vector<Value> fields;
    std::unique_ptr<ObjectDict, void (*)(ObjectDict *)> dict = { nullptr, delete_object_dict };
    uint32_t shape_version = 0;
    bool frozen = false;
    bool dict_mode = false;

    ObjectObj();
    // Header free-list pool (see PooledHeapObject in core_types.cpp). Only the
    // fixed-size header block is recycled
    std::vector<std::string> get_keys() const;
    void clear_fields();
    Value *get_field(const std::string &name) noexcept;
    const Value *get_field(const std::string &name) const noexcept;
    // Lookup by a pre-interned field id, skipping the per-call intern_field() string hash.
    const Value *get_field_by_id(uint32_t fid) const noexcept;
    void promote_to_dict_mode();
    void set_field(const std::string &name, Value val);
    bool has_field(const std::string &name) const noexcept;
    bool is_empty() const noexcept;
    size_t field_count() const noexcept;
};

struct FunctionData : HeapHeader {
    std::string name;
    std::shared_ptr<nari::Function> func_ptr;
    CapturesList captures;
    int32_t jit_func_idx = -1;
    uint32_t jit_locals_count = 0;
    nari::bytecode::FunctionMeta *jit_meta = nullptr;
    JitInlineKind jit_inline_kind = JitInlineKind::None;
    int32_t jit_native_kind = 0;
    int64_t jit_inline_imm = 0;
    Value *jit_capture0_raw = nullptr;
    // Pre-resolved runtime builtin member-function pointer for global builtins (toString, toNumber, etc.). 
    // Filled at registration time by VM::register_builtin so that the JIT call helper can dispatch in one indirect call
    void *jit_builtin_fn[2] = { nullptr, nullptr };
    bool jit_builtin_fn_valid = false;

    FunctionData() {
        type_tag = ValueTag::Function;
    }
    explicit FunctionData(std::string n) : name(std::move(n)) {
        type_tag = ValueTag::Function;
    }
    FunctionData(std::string n, std::shared_ptr<nari::Function> ptr)
        : name(std::move(n)), func_ptr(std::move(ptr)) {
        type_tag = ValueTag::Function;
    }
};

struct ClassLayout {
    std::vector<std::string> names;
    std::unordered_map<std::string, uint32_t> index;
    std::unordered_set<std::string> private_fields;
};

typedef std::unordered_map<std::string, ClassLayout> LayoutRegistry;

LayoutRegistry &class_layout_registry();

struct ClassInstance : HeapHeader {
    std::string class_name;
    const ClassLayout *layout = nullptr;
    std::vector<Value> field_values;

    explicit ClassInstance(std::string cn);
    Value *get_field(const std::string &name) noexcept;
    const Value *get_field(const std::string &name) const noexcept;
};

struct RegexObj : HeapHeader {
    std::string pattern;
    std::string flags;
    RegexObj(std::string p, std::string f) : pattern(std::move(p)), flags(std::move(f)) {
        type_tag = ValueTag::Regex;
    }
};

// proxy-like interposer: wraps a target value with a handler object whose
// optional get/set/has/call fields intercept the corresponding operations.
struct DelegateData : HeapHeader {
    Value target;
    Value handler;
    DelegateData(Value t, Value h) : target(t), handler(h) {
        type_tag = ValueTag::Delegate;
    }
};

struct HandleData : HeapHeader {
    enum State {
        Running,
        Completed,
        Failed
    };
    State state = Running;
    Value result;
    Value error;
    std::unique_ptr<Task> task;
    chrono::steady_clock::time_point start_time;
    chrono::steady_clock::time_point end_time;

    HandleData();
};

struct Flags {
    bool break_flag = false;
    bool continue_flag = false;
    bool return_flag = false;
    Value return_value;
    bool tailcall_flag = false;
    std::vector<Value> tailcall_args;
    bool throw_flag = false;
    Value throw_value;
    bool shutdown_flag = false;

    bool any_flag() const {
        return break_flag || continue_flag || return_flag || throw_flag || shutdown_flag;
    }
};

struct IntervalData {
    int64_t id = 0;
    Value callback;
    int64_t interval_ms = 0;
    chrono::steady_clock::time_point next_fire;
};

struct Task {
    enum State {
        Running,
        Yielded,
        Completed,
        Failed
    };
    State state = Running;
    const nari::BlockStmt *body = nullptr;
    size_t current_stmt = 0;
    Value result;
    Value error;
    std::map<std::string, Value> locals;
    std::vector<std::map<std::string, Value>> block_scopes;
    Flags flags;

    explicit Task(const nari::BlockStmt *b) : body(b) {
    }
};

inline ObjectShapeRegistry::ObjectShapeRegistry() {
    auto empty = std::make_unique<ObjectShape>();
    this->empty = empty.get();
    shapes.push_back(std::move(empty));
}

inline const ObjectShape *ObjectShapeRegistry::empty_shape() const {
    return this->empty;
}

inline const ObjectShape *ObjectShapeRegistry::extend(const ObjectShape *base, uint32_t fid) {
    if (!base) {
        base = this->empty;
    }
    if (base->index.count(fid) != 0) {
        return base;
    }

    std::string key;
    for (const auto &name : base->names) {
        key += std::to_string(intern_field(name));
        key += ',';
    }
    key += std::to_string(fid);
    auto it = this->cache.find(key);
    if (it != this->cache.end()) {
        return it->second;
    }

    auto next = std::make_unique<ObjectShape>();
    next->names = base->names;
    next->index = base->index;
    next->index[fid] = static_cast<uint32_t>(next->names.size());
    next->names.push_back(field_name(fid));
    const ObjectShape *ptr = next.get();
    this->shapes.push_back(std::move(next));
    this->cache[std::move(key)] = ptr;
    return ptr;
}

inline std::unordered_map<std::string, uint32_t> &field_intern_map() {
    static std::unordered_map<std::string, uint32_t> map;
    return map;
}

inline std::vector<std::string> &field_intern_names() {
    static std::vector<std::string> names;
    return names;
}

inline uint32_t intern_field(const std::string &name) {
    auto &map = field_intern_map();
    auto it = map.find(name);
    if (it != map.end()) {
        return it->second;
    }
    auto &names = field_intern_names();
    uint32_t id = static_cast<uint32_t>(names.size());
    names.push_back(name);
    map.emplace(names.back(), id);
    return id;
}

inline const std::string &field_name(uint32_t id) {
    static const std::string empty;
    auto &names = field_intern_names();
    return id < names.size() ? names[id] : empty;
}

inline ObjectShapeRegistry &object_shape_registry() {
    static ObjectShapeRegistry reg;
    return reg;
}

inline LayoutRegistry &class_layout_registry() {
    static LayoutRegistry reg;
    return reg;
}

inline HandleData::HandleData() : start_time(std::chrono::steady_clock::now()) {
    this->type_tag = ValueTag::Handle;
}

inline Value Value::from_raw(uint64_t raw) {
    Value val;
    val._raw = raw;
    return val;
}
inline uint64_t Value::raw_bits() const {
    return this->_raw;
}
inline Value Value::none() {
    return Value();
}
inline bool Value::fits_int48(int64_t val) {
    return val >= INT48_MIN && val <= INT48_MAX;
}
inline Value Value::make_int(int64_t val) {
    Value new_val;
    new_val._raw = NB_INT_TAG | (static_cast<uint64_t>(val) & PTR_MASK);
    return new_val;
}
inline Value Value::make_int_checked(int64_t val) {
    return fits_int48(val) ? make_int(val) : make_float(static_cast<double>(val));
}
inline Value Value::make_float(double val) {
    Value new_val;
    std::memcpy(&new_val._raw, &val, sizeof(double));
    return new_val;
}
inline Value Value::make_bool(bool val) {
    Value new_val;
    new_val._raw = NB_BOOL_TAG | (val ? 1ULL : 0ULL);
    return new_val;
}
inline Value Value::make_string(std::string val) {
    Value new_val;
    auto *str = new StringObj(std::move(val));
    GarbageCollector::instance().track(str, GarbageCollector::TrackedType::String);
    new_val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(str);
    return new_val;
}
inline Value Value::make_const_string(const std::string &val) {
    Value new_val = make_string(val);
    if (auto *str = static_cast<StringObj *>(new_val.heap_ptr())) {
        str->immutable = true;
    }
    return new_val;
}
inline Value Value::make_function(std::string name) {
    Value val;
    auto *fn = new FunctionData(std::move(name));
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(fn);
    return val;
}
inline Value Value::make_function(std::string name, std::shared_ptr<nari::Function> func_ptr) {
    Value val;
    auto *fn = new FunctionData(std::move(name), std::move(func_ptr));
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(fn);
    return val;
}
inline Value Value::from_class_instance(ClassInstancePtr ptr) {
    Value val;
    if (ptr) {
        val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(ptr);
    }
    return val;
}
inline Value Value::make_regex(std::string pattern, std::string flags) {
    Value val;
    auto *regex = new RegexObj(std::move(pattern), std::move(flags));
    GarbageCollector::instance().track(regex, GarbageCollector::TrackedType::Unknown);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(regex);
    return val;
}
inline Value Value::make_delegate(Value target, Value handler) {
    Value val;
    auto *delegate = new DelegateData(target, handler);
    GarbageCollector::instance().track(delegate, GarbageCollector::TrackedType::Unknown);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(delegate);
    return val;
}
inline uint16_t Value::tag_word() const {
    return static_cast<uint16_t>(_raw >> 48);
}
inline bool Value::is_none() const {
    return _raw == NB_NONE;
}
inline bool Value::is_int() const {
    return tag_word() == TAG_INT;
}
inline bool Value::is_bool() const {
    return tag_word() == TAG_BOOL;
}
inline bool Value::is_float() const {
    uint16_t t = tag_word();
    return t != TAG_HEAP && t != TAG_INT && t != TAG_BOOL && t != TAG_NONE;
}
inline bool Value::is_numeric() const {
    return is_int() || is_float();
}
inline bool Value::is_heap() const {
    return tag_word() == TAG_HEAP;
}
inline HeapHeader *Value::heap_ptr() const {
    return is_heap() ? reinterpret_cast<HeapHeader *>(_raw & PTR_MASK) : nullptr;
}
inline ValueTag Value::heap_tag() const {
    HeapHeader *p = heap_ptr();
    return p ? p->type_tag : ValueTag::String;
}
inline bool Value::is_string() const {
    return is_heap() && heap_tag() == ValueTag::String;
}
inline bool Value::is_array() const {
    return is_heap() && heap_tag() == ValueTag::Array;
}
inline bool Value::is_object() const {
    return is_heap() && heap_tag() == ValueTag::Object;
}
inline bool Value::is_function() const {
    return is_heap() && heap_tag() == ValueTag::Function;
}
inline bool Value::is_handle() const {
    return is_heap() && heap_tag() == ValueTag::Handle;
}
inline bool Value::is_class_instance() const {
    return is_heap() && heap_tag() == ValueTag::ClassInstance;
}
inline bool Value::is_regex() const {
    return is_heap() && heap_tag() == ValueTag::Regex;
}
inline bool Value::is_delegate() const {
    return is_heap() && heap_tag() == ValueTag::Delegate;
}
inline bool Value::is_sso() const {
    return false;
}
inline uint8_t Value::sso_len() const {
    return 0;
}
inline char Value::sso_char(uint8_t) const {
    return '\0';
}
inline bool Value::is_mutable_heap_string() const {
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::String && !static_cast<StringObj *>(p)->immutable;
}
inline int64_t Value::get_int() const {
    int64_t v = static_cast<int64_t>(_raw & PTR_MASK);
    return (v << 16) >> 16;
}
inline double Value::get_float() const {
    double d;
    uint64_t raw = _raw;
    std::memcpy(&d, &raw, sizeof(double));
    return d;
}
inline bool Value::get_bool() const {
    return (this->_raw & 1ULL) != 0;
}
inline const std::string &Value::get_string() const {
    static const std::string empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::String ? static_cast<StringObj *>(p)->s : empty;
}
inline std::string &Value::get_string() {
    static std::string empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::String ? static_cast<StringObj *>(p)->s : empty;
}
inline const Array &Value::get_array() const {
    static const Array empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Array ? static_cast<ArrayObj *>(p)->v : empty;
}
inline Array &Value::get_array() {
    static Array empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Array ? static_cast<ArrayObj *>(p)->v : empty;
}
inline ObjectObj *Value::get_obj_ptr() {
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Object ? static_cast<ObjectObj *>(p) : nullptr;
}
inline const ObjectObj *Value::get_obj_ptr() const {
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Object ? static_cast<const ObjectObj *>(p) : nullptr;
}
inline ObjectObj *Value::get_object() {
    return get_obj_ptr();
}
inline const ObjectObj *Value::get_object() const {
    return get_obj_ptr();
}
inline FunctionData &Value::get_function() {
    return *static_cast<FunctionData *>(heap_ptr());
}
inline const FunctionData &Value::get_function() const {
    return *static_cast<const FunctionData *>(heap_ptr());
}
inline HandlePtr Value::get_handle() const {
    return is_handle() ? static_cast<HandleData *>(heap_ptr()) : nullptr;
}
inline ClassInstancePtr Value::get_class_instance() const {
    return is_class_instance() ? static_cast<ClassInstance *>(heap_ptr()) : nullptr;
}
inline RegexObj *Value::get_regex() {
    return is_regex() ? static_cast<RegexObj *>(heap_ptr()) : nullptr;
}
inline const RegexObj *Value::get_regex() const {
    return is_regex() ? static_cast<const RegexObj *>(heap_ptr()) : nullptr;
}
inline DelegateData *Value::get_delegate() const {
    return is_delegate() ? static_cast<DelegateData *>(heap_ptr()) : nullptr;
}
inline void Value::inplace_int(int64_t v) {
    *this = make_int(v);
}
inline void Value::inplace_int_checked(int64_t v) {
    *this = make_int_checked(v);
}
inline void Value::set_int(int64_t v) {
    inplace_int(v);
}
inline void Value::set_bool(bool v) {
    *this = make_bool(v);
}
inline void Value::set_float(double v) {
    *this = make_float(v);
}
inline void Value::inplace_float(double v) {
    set_float(v);
}
inline size_t Value::tag() const {
    if (is_none()) {
        return 0;
    }
    if (is_int()) {
        return 1;
    }
    if (is_float()) {
        return 2;
    }
    if (is_bool()) {
        return 3;
    }
    return static_cast<size_t>(heap_tag());
}
inline double Value::as_number() const {
    if (is_int()) {
        return static_cast<double>(get_int());
    }
    if (is_float()) {
        return get_float();
    }
    if (is_bool()) {
        return get_bool() ? 1.0 : 0.0;
    }
    if (is_string()) {
        const auto &s = get_string();
        if (s.empty()) {
            return 0.0;
        }
        char *end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        return end && *end == '\0' ? d : 0.0;
    }
    if (is_array()) {
        return static_cast<double>(get_array().size());
    }
    if (is_object()) {
        const auto *o = get_obj_ptr();
        return o ? static_cast<double>(o->field_count()) : 0.0;
    }
    return 0.0;
}
inline bool Value::as_bool() const {
    if (is_none()) {
        return false;
    }
    if (is_bool()) {
        return get_bool();
    }
    if (is_int()) {
        return get_int() != 0;
    }
    if (is_float()) {
        return get_float() != 0.0;
    }
    if (is_string()) {
        return !get_string().empty();
    }
    if (is_array()) {
        return !get_array().empty();
    }
    if (is_object()) {
        const auto *o = get_obj_ptr();
        return o && !o->is_empty();
    }
    return heap_ptr() != nullptr;
}
inline bool is_truthy(const Value &v) {
    return v.as_bool();
}
inline std::string Value::to_string(bool in_container_context) const {
    // `seen` tracks the ancestor container chain to prevent recursing until the C stack overflows.
    std::vector<const void *> seen;
    return to_string_impl(in_container_context, seen);
}
inline std::string Value::to_string_impl(bool in_container_context, std::vector<const void *> &seen) const {
    constexpr size_t kMaxDepth = 128; // also caps non-cyclic nesting
    if (is_none()) {
        return "<null>";
    }
    if (is_int()) {
        return std::to_string(get_int());
    }
    if (is_float()) {
        char buf[64];
        auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), get_float());
        return ec == std::errc() ? std::string(buf, p) : std::to_string(get_float());
    }
    if (is_bool()) {
        return get_bool() ? "true" : "false";
    }
    if (is_string()) {
        const auto &s = get_string();
        return in_container_context ? (std::string("\"") + s + "\"") : s;
    }
    if (is_array()) {
        const void *p = heap_ptr();
        for (const void *q : seen) {
            if (q == p) {
                return "[Circular]";
            }
        }
        if (seen.size() >= kMaxDepth) {
            return "...";
        }
        seen.push_back(p);
        std::string r = "[";
        const auto &a = get_array();
        for (size_t i = 0; i < a.size(); i++) {
            if (i) {
                r += ", ";
            }
            r += a[i].to_string_impl(true, seen);
        }
        r += "]";
        seen.pop_back();
        return r;
    }
    if (is_object()) {
        const void *p = heap_ptr();
        for (const void *q : seen) {
            if (q == p) {
                return "[Circular]";
            }
        }
        if (seen.size() >= kMaxDepth) {
            return "...";
        }
        seen.push_back(p);
        std::string r = "{";
        const auto *o = get_obj_ptr();
        bool first = true;
        if (o) {
            for (const auto &k : o->get_keys()) {
                const Value *v = o->get_field(k);
                if (!first) {
                    r += ", ";
                }
                first = false;
                r += k + ": " + (v ? v->to_string_impl(true, seen) : Value::none().to_string_impl(true, seen));
            }
        }
        r += "}";
        seen.pop_back();
        return r;
    }
    if (is_function()) {
        return "<function " + get_function().name + ">";
    }
    if (is_handle()) {
        return "<handle>";
    }
    if (is_class_instance()) {
        auto *ci = get_class_instance();
        return ci ? ("<" + ci->class_name + " instance>") : "<instance>";
    }
    if (is_regex()) {
        const auto *re = get_regex();
        return re ? ("/" + re->pattern + "/" + re->flags) : "/(?:)/";
    }
    if (is_delegate()) {
        const void *p = heap_ptr();
        for (const void *q : seen) {
            if (q == p) {
                return "[Circular]";
            }
        }
        if (seen.size() >= kMaxDepth) {
            return "...";
        }
        auto *d = get_delegate();
        if (!d) {
            return "<delegate>";
        }
        seen.push_back(p);
        std::string r = "<delegate " + d->target.to_string_impl(true, seen) + ">";
        seen.pop_back();
        return r;
    }
    return "<unknown>";
}
inline bool Value::values_equal(const Value &a, const Value &b) {
    if (a.is_none() || b.is_none()) {
        return a.is_none() && b.is_none();
    }
    if (a.is_numeric() && b.is_numeric()) {
        return std::fabs(a.as_number() - b.as_number()) < 1e-12;
    }
    if (a.is_bool() || b.is_bool()) {
        return a.is_bool() && b.is_bool() && a.get_bool() == b.get_bool();
    }
    if (a.is_string() || b.is_string()) {
        return a.is_string() && b.is_string() && a.get_string() == b.get_string();
    }
    return a._raw == b._raw;
}
