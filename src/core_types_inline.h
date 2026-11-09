#pragma once
// Inline method definitions for core_types.h (split out verbatim).
// Included at the bottom of core_types.h; safe to include directly.
#include "core_types.h"

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
    if (base->slot_of(fid) != ObjectShape::NoSlot) {
        return base;
    }

    auto it = base->transitions.find(fid);
    if (it != base->transitions.end()) {
        return it->second;
    }

    auto next = std::make_unique<ObjectShape>();
    next->field_ids = base->field_ids;
    next->field_mask = base->field_mask | (uint64_t{ 1 } << (fid & 63));
    next->field_ids.push_back(fid);
    const ObjectShape *ptr = next.get();
    this->shapes.push_back(std::move(next));
    base->transitions.emplace(fid, ptr);
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

inline uint32_t intern_field_slow(const std::string &name) {
    auto &map = field_intern_map();
    auto it = map.find(name);
    if (it != map.end()) {
        return it->second;
    }
    auto &names = field_intern_names();
    uint32_t id = names.size();
    names.push_back(name);
    map.emplace(names.back(), id);
    return id;
}

inline uint32_t intern_field(const std::string &name) {
    // short names hit a direct-mapped cache keyed by the packed name bytes
    // this skips the string hash and bucket walk of field_intern_map.
    const size_t len = name.size();
    if (len == 0) {
        return intern_field_slow(name);
    }
    if (len - 1 < 8) { // 1..8 bytes (len 0 stays on the slow path)
        struct ShortNameEntry {
            uint64_t key;
            uint32_t len;
            uint32_t fid;
        };
        static ShortNameEntry short_cache[4096] = {};
        uint64_t key = 0;
        std::memcpy(&key, name.data(), len);
        ShortNameEntry &e = short_cache[(key * 0x9E3779B97F4A7C15ull >> 52) & 4095];
        if (e.key == key && e.len == len) {
            return e.fid;
        }
        uint32_t id = intern_field_slow(name);
        e.key = key;
        e.len = len;
        e.fid = id;
        return id;
    }

    // Repeated long property names are common in transpiled JavaScript. Avoid
    // hashing the whole string on a hit; equality keeps collisions exact.
    struct LongNameEntry {
        uint64_t key = 0;
        uint32_t len = 0;
        uint32_t fid = UINT32_MAX;
    };
    static LongNameEntry long_cache[4096];
    uint64_t first;
    uint64_t last;
    std::memcpy(&first, name.data(), sizeof(first));
    std::memcpy(&last, name.data() + len - sizeof(last), sizeof(last));
    uint64_t key = first ^ (last * 0x9E3779B97F4A7C15ull) ^ (len * 0xBF58476D1CE4E5B9ull);
    LongNameEntry &e = long_cache[(key ^ (key >> 32)) & 4095];
    if (e.key == key && e.len == len && e.fid != UINT32_MAX && field_name(e.fid) == name) {
        return e.fid;
    }
    uint32_t id = intern_field_slow(name);
    e.key = key;
    e.len = len;
    e.fid = id;
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

inline HandleData::HandleData() : start_time(chrono::steady_clock::now()) {
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
        str->js_getter_prefix = val == "__js_getter__";
    }
    return new_val;
}
inline Value Value::make_function(std::string name) {
    Value val;
    auto *fn = new FunctionData(std::move(name));
    GarbageCollector::instance().track(fn, GarbageCollector::TrackedType::Function);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(fn);
    return val;
}
inline Value Value::make_function(std::string name, std::shared_ptr<nari::Function> func_ptr) {
    Value val;
    auto *fn = new FunctionData(std::move(name), std::move(func_ptr));
    GarbageCollector::instance().track(fn, GarbageCollector::TrackedType::Function);
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
    return static_cast<uint16_t>((_raw >> 48));
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
    auto *ptr = heap_ptr();
    return ptr && ptr->type_tag == ValueTag::String && !(static_cast<StringObj *>(ptr))->immutable;
}
inline int64_t Value::get_int() const {
    int64_t val = static_cast<int64_t>((_raw & PTR_MASK));
    return (val << 16) >> 16;
}
inline uintptr_t Value::get_ptr_bits() const {
    return (uintptr_t)(_raw & PTR_MASK);
}
inline void *Value::get_ptr() const {
    return reinterpret_cast<void *>((uintptr_t)(_raw & PTR_MASK));
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
    return p && p->type_tag == ValueTag::String ? (static_cast<StringObj *>(p))->s : empty;
}
inline std::string &Value::get_string() {
    static std::string empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::String ? (static_cast<StringObj *>(p))->s : empty;
}
inline const Array &Value::get_array() const {
    static const Array empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Array ? (static_cast<ArrayObj *>(p))->v : empty;
}
inline Array &Value::get_array() {
    static Array empty;
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Array ? (static_cast<ArrayObj *>(p))->v : empty;
}
inline ObjectObj *Value::get_obj_ptr() {
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Object ? (static_cast<ObjectObj *>(p)) : nullptr;
}
inline const ObjectObj *Value::get_obj_ptr() const {
    auto *p = heap_ptr();
    return p && p->type_tag == ValueTag::Object ? (static_cast<const ObjectObj *>(p)) : nullptr;
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
inline void Value::set_int(int64_t v) {
    *this = make_int(v);
}
inline void Value::set_int_checked(int64_t v) {
    *this = make_int_checked(v);
}
inline void Value::set_bool(bool v) {
    *this = make_bool(v);
}
inline void Value::set_float(double v) {
    *this = make_float(v);
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
inline bool is_js_truthy(const Value &v) {
    if (v.is_none()) {
        return false;
    }
    if (v.is_bool()) {
        return v.get_bool();
    }
    if (v.is_int()) {
        return v.get_int() != 0;
    }
    if (v.is_float()) {
        double value = v.get_float();
        return value != 0.0 && !std::isnan(value);
    }
    if (v.is_string()) {
        return !v.get_string().empty();
    }
    return true;
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

// Three-way ordering, shared by <, <=, > and >=. The bytecode interpreter and the
// JIT helpers both reach the operators through this one function.
//
// Returns false when a and b have no order, and then every relational operator
// gives false. Only two values of the same class have an order. This keeps the
// operators coherent with values_equal(): if two values are ==, they are also <=
// and >=, and a value that is < another is never == to it.
//
// Numbers use the same 1e-12 tolerance as values_equal(). Without it, a pair that
// == calls equal could still report < or >.
//
// The branch order matches values_equal(). is_numeric() covers int and float only,
// so a bool never takes the numeric path.
inline bool Value::compare_ordered(const Value &a, const Value &b, int &cmp) {
    if (a.is_none() || b.is_none()) {
        if (!a.is_none() || !b.is_none()) {
            return false;
        }
        cmp = 0;
        return true;
    }
    if (a.is_int() && b.is_int()) {
        const int64_t x = a.get_int();
        const int64_t y = b.get_int();
        cmp = x < y ? -1 : (x > y ? 1 : 0);
        return true;
    }
    if (a.is_numeric() && b.is_numeric()) {
        const double x = a.as_number();
        const double y = b.as_number();
        cmp = std::fabs(x - y) < 1e-12 ? 0 : (x < y ? -1 : 1);
        return true;
    }
    if (a.is_bool() || b.is_bool()) {
        if (!a.is_bool() || !b.is_bool()) {
            return false;
        }
        const int x = a.get_bool() ? 1 : 0;
        const int y = b.get_bool() ? 1 : 0;
        cmp = x < y ? -1 : (x > y ? 1 : 0);
        return true;
    }
    if (a.is_string() || b.is_string()) {
        if (!a.is_string() || !b.is_string()) {
            return false;
        }
        const std::string &x = a.get_string();
        const std::string &y = b.get_string();
        cmp = x < y ? -1 : (x > y ? 1 : 0);
        return true;
    }
    // Two heap values have an order only when they are the same object.
    if (a._raw == b._raw) {
        cmp = 0;
        return true;
    }
    return false;
}

inline bool Value::values_lt(const Value &a, const Value &b) {
    int cmp = 0;
    return compare_ordered(a, b, cmp) && cmp < 0;
}

inline bool Value::values_le(const Value &a, const Value &b) {
    int cmp = 0;
    return compare_ordered(a, b, cmp) && cmp <= 0;
}

inline bool Value::values_gt(const Value &a, const Value &b) {
    int cmp = 0;
    return compare_ordered(a, b, cmp) && cmp > 0;
}

inline bool Value::values_ge(const Value &a, const Value &b) {
    int cmp = 0;
    return compare_ordered(a, b, cmp) && cmp >= 0;
}

inline bool Value::values_strict_equal(const Value &a, const Value &b) {
    const uint16_t a_tag = a.tag_word();
    const uint16_t b_tag = b.tag_word();

    if (a_tag == TAG_HEAP && b_tag == TAG_HEAP) {
        if (a._raw == b._raw) {
            return true;
        }
        HeapHeader *a_heap = reinterpret_cast<HeapHeader *>(a._raw & PTR_MASK);
        HeapHeader *b_heap = reinterpret_cast<HeapHeader *>(b._raw & PTR_MASK);
        return a_heap->type_tag == ValueTag::String && b_heap->type_tag == ValueTag::String &&
               static_cast<StringObj *>(a_heap)->s == static_cast<StringObj *>(b_heap)->s;
    }
    if (a_tag == TAG_INT && b_tag == TAG_INT) {
        return a._raw == b._raw;
    }
    if (a.is_numeric() && b.is_numeric()) {
        return a.as_number() == b.as_number();
    }
    return a_tag == b_tag && a._raw == b._raw;
}
