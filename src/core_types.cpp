#include "core_types.h"
#include "compiler_support.h"
#include "gc.h"

#include <new>
#include <stdlib.h>
#include <vector>

// Heap-object header free-list pools
// A bounded free-list recycles just the fixed-size header block, the C++ members
namespace {
class HeaderPool {
  public:
    explicit HeaderPool(std::size_t cap) : slots(new std::vector<void *>()), cap(cap) {
        this->slots->reserve(cap);
    }

    void *allocate(std::size_t size) {
        if (!this->slots->empty()) {
            void *ptr = this->slots->back();
            this->slots->pop_back();
            return ptr;
        }
        void *ptr = malloc(size);
        if (!ptr) {
            throw std::bad_alloc();
        }
        return ptr;
    }

    void deallocate(void *ptr) noexcept {
        if (!ptr) {
            return;
        }
        if (this->slots->size() < this->cap) {
            this->slots->push_back(ptr);
            return;
        }
        std::free(ptr);
    }

  private:
    std::vector<void *> *slots;
    std::size_t cap;
};

constexpr std::size_t HeaderPoolCap = 8192;

// One pool per pooled type, selected at compile time. The pool is private to each TU and lazily constructed.
template <class Derived> HeaderPool &pool_for() {
    static HeaderPool *p = new HeaderPool(HeaderPoolCap);
    return *p;
}
} // namespace

template <class Derived> void *PooledHeapObject<Derived>::operator new(std::size_t sz) {
    return pool_for<Derived>().allocate(sz);
}
template <class Derived> void PooledHeapObject<Derived>::operator delete(void *ptr) noexcept {
    pool_for<Derived>().deallocate(ptr);
}

template struct PooledHeapObject<StringObj>;
template struct PooledHeapObject<ArrayObj>;
template struct PooledHeapObject<ObjectObj>;
template struct PooledHeapObject<FunctionData>;

Value Value::make_array() {
    Value val;
    auto *arr = new ArrayObj();
    arr->type_tag = ValueTag::Array;
    GarbageCollector::instance().track(arr, GarbageCollector::TrackedType::Array);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(arr);
    return val;
}

Value Value::make_array(std::vector<Value> elements) {
    Value val;
    auto *arr = new ArrayObj();
    arr->type_tag = ValueTag::Array;
    arr->v = std::move(elements);
    GarbageCollector::instance().track(arr, GarbageCollector::TrackedType::Array);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(arr);
    return val;
}

Value Value::make_array(const Value *elements, size_t count) {
    Value val;
    auto *arr = new ArrayObj();
    arr->type_tag = ValueTag::Array;
    arr->v.reserve(count);
    for (size_t i = 0; i < count; i++) {
        arr->v.push_back(elements[i]);
    }
    GarbageCollector::instance().track(arr, GarbageCollector::TrackedType::Array);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(arr);
    return val;
}

Value Value::make_object() {
    Value val;
    auto *obj = new ObjectObj();
    obj->type_tag = ValueTag::Object;
    GarbageCollector::instance().track(obj, GarbageCollector::TrackedType::Object);
    val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(obj);
    return val;
}

Value Value::make_object(ObjectObj *entries) {
    Value val;
    if (entries) {
        // entries is already tracked by the collector; just wrap it.
        val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(entries);
    } else {
        auto *obj = new ObjectObj();
        obj->type_tag = ValueTag::Object;
        GarbageCollector::instance().track(obj, GarbageCollector::TrackedType::Object);
        val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(obj);
    }
    return val;
}

HandlePtr Value::make_handle_ptr() {
    auto *handle = new HandleData();
    handle->type_tag = ValueTag::Handle;
    GarbageCollector::instance().track(handle, GarbageCollector::TrackedType::Handle);
    return handle;
}

Value Value::make_handle(HandlePtr handlePtr) {
    Value val;
    if (handlePtr) {
        val._raw = NB_HEAP_TAG | reinterpret_cast<uint64_t>(handlePtr);
    }
    return val;
}

struct ObjectDict {
    std::unordered_map<std::string, Value> map;
    // Insertion order for Object.keys(); the map alone cannot preserve it.
    std::vector<std::string> keys;
};

void delete_object_dict(ObjectDict *p) noexcept {
    delete p;
}

ObjectObj::ObjectObj() : shape(object_shape_registry().empty_shape()) {
}

std::vector<std::string> ObjectObj::get_keys() const {
    std::vector<std::string> out;
    if (dict_mode && dict) {
        out = dict->keys;
    } else {
        out.reserve(shape->field_ids.size());
        for (size_t i = 0; i < shape->field_ids.size(); i++) {
            out.push_back(shape->name_at(i));
        }
    }
    return out;
}

void ObjectObj::clear_fields() {
    fields.clear();
    lazy_field_context = nullptr;
    lazy_field_factory = nullptr;
    lazy_field_invoker = nullptr;
    lazy_payload = Value::none();
    lazy_captures.reset();
    lazy_field_mask = 0;
    if (dict) {
        dict->map.clear();
    }
}

Value *ObjectObj::get_field(const std::string &name) noexcept {
    if (dict_mode && dict) {
        auto it = dict->map.find(name);
        return it != dict->map.end() ? &it->second : nullptr;
    }
    const uint32_t slot = shape->slot_of(intern_field(name));
    return slot != ObjectShape::kNoSlot ? materialize_lazy_field(slot) : nullptr;
}

const Value *ObjectObj::get_field(const std::string &name) const noexcept {
    if (dict_mode && dict) {
        auto it = dict->map.find(name);
        return it != dict->map.end() ? &it->second : nullptr;
    }
    const uint32_t slot = shape->slot_of(intern_field(name));
    return slot != ObjectShape::kNoSlot ? const_cast<ObjectObj *>(this)->materialize_lazy_field(slot) : nullptr;
}

const Value *ObjectObj::get_field_by_id(uint32_t fid) const noexcept {
    if (dict_mode && dict) {
        // Rare (only for >32-field objects). Recover the name and use the
        // string path so dict-mode semantics are preserved exactly.
        auto it = dict->map.find(field_name(fid));
        return it != dict->map.end() ? &it->second : nullptr;
    }
    const uint32_t slot = shape->slot_of(fid);
    return slot != ObjectShape::kNoSlot ? const_cast<ObjectObj *>(this)->materialize_lazy_field(slot) : nullptr;
}

Value *ObjectObj::materialize_lazy_field(uint32_t slot) {
    if (slot >= fields.size()) {
        return nullptr;
    }
    const uint64_t bit = slot < 64 ? uint64_t{ 1 } << slot : 0;
    if (bit && (lazy_field_mask & bit) && lazy_field_factory) {
        fields[slot] = lazy_field_factory(lazy_field_context, this, slot);
        lazy_field_mask &= ~bit;
        if (lazy_field_mask == 0) {
            lazy_field_context = nullptr;
            lazy_field_factory = nullptr;
            lazy_field_invoker = nullptr;
            lazy_payload = Value::none();
            lazy_captures.reset();
        }
    }
    return &fields[slot];
}

void ObjectObj::clear_lazy_field(uint32_t slot) noexcept {
    if (slot < 64) {
        lazy_field_mask &= ~(uint64_t{ 1 } << slot);
    }
    if (lazy_field_mask == 0) {
        lazy_field_context = nullptr;
        lazy_field_factory = nullptr;
        lazy_field_invoker = nullptr;
        lazy_payload = Value::none();
        lazy_captures.reset();
    }
}

bool ObjectObj::invoke_lazy_field(uint32_t slot, const Value *args, size_t argc, Value &result) {
    const uint64_t bit = slot < 64 ? uint64_t{ 1 } << slot : 0;
    return bit && (lazy_field_mask & bit) && lazy_field_invoker &&
           lazy_field_invoker(lazy_field_context, this, slot, args, argc, result);
}

#if COMPILER_IS_REAL_MSVC
inline int ctzll(uint64_t x) {
    unsigned long index;
    _BitScanForward64(&index, x);
    return (int)index;
}
#else
inline int ctzll(uint64_t x) {
    return __builtin_ctzll(x);
}
#endif

void ObjectObj::promote_to_dict_mode() {
    if (dict_mode) {
        return;
    }
    // Dict storage has no slot-level lazy hook, so realize pending fields first.
    while (lazy_field_mask) {
        const uint32_t slot = (uint32_t)ctzll(lazy_field_mask);
        materialize_lazy_field(slot);
        clear_lazy_field(slot);
    }
    // move existing shape-mode fields into the hash map, preserving values.
    dict.reset(new ObjectDict());
    const size_t nfields = shape->field_ids.size();
    dict->map.reserve(nfields * 2);
    dict->keys.reserve(nfields);
    for (size_t i = 0; i < nfields && i < fields.size(); i++) {
        const std::string &nm = shape->name_at(i);
        dict->map.emplace(nm, std::move(fields[i]));
        dict->keys.push_back(nm);
    }
    fields.clear();
    fields.shrink_to_fit();
    // reset to the empty shape so any code that still reads `shape` sees a sane layout
    shape = object_shape_registry().empty_shape();
    dict_mode = true;
    shape_version++;
}

void ObjectObj::set_field(const std::string &name, Value val) {
    if (frozen) {
        return;
    }
    // Dict mode is the only path that genuinely needs the name; everything else is
    // keyed by field id, so hand over to the by-id path rather than duplicating the
    // shape/promote logic here.
    if (dict_mode) {
        if (!dict) {
            dict.reset(new ObjectDict());
        }
        auto it = dict->map.find(name);
        if (it != dict->map.end()) {
            it->second = std::move(val);
        } else {
            dict->map.emplace(name, std::move(val));
            dict->keys.push_back(name);
            shape_version++;
        }
        return;
    }
    const uint32_t fid = intern_field(name);
    if (const uint32_t slot = shape->slot_of(fid); slot != ObjectShape::kNoSlot) {
        clear_lazy_field(slot);
        fields[slot] = std::move(val);
        return;
    }
    // adding a field: promote to dict mode first if this shape is already too wide
    if (shape->field_ids.size() >= kDictModeThreshold) {
        promote_to_dict_mode();
        dict->map.emplace(name, std::move(val));
        dict->keys.push_back(name);
        shape_version++;
        return;
    }
    shape = object_shape_registry().extend(shape, fid);
    fields.push_back(std::move(val));
    shape_version++;
}

bool ObjectObj::has_field(const std::string &name) const noexcept {
    if (dict_mode && dict) {
        return dict->map.count(name) != 0;
    }
    return has_field_by_id(intern_field(name));
}

Value *ObjectObj::get_field_by_id(uint32_t fid) noexcept {
    if (dict_mode && dict) {
        // Rare (only past kDictModeThreshold fields)
        // Recover the name and use the string path so dict-mode semantics are preserved exactly
        auto it = dict->map.find(field_name(fid));
        return it != dict->map.end() ? &it->second : nullptr;
    }
    const uint32_t slot = shape->slot_of(fid);
    return slot != ObjectShape::kNoSlot ? materialize_lazy_field(slot) : nullptr;
}

void ObjectObj::set_field_by_id(uint32_t fid, Value val) {
    // Same trust boundary as the string-keyed path: a frozen object rejects every write,
    // including the by-id fast path the JIT's inline caches use.
    if (frozen) {
        return;
    }
    if (dict_mode) {
        set_field(field_name(fid), std::move(val));
        return;
    }
    if (const uint32_t slot = shape->slot_of(fid); slot != ObjectShape::kNoSlot) {
        clear_lazy_field(slot);
        fields[slot] = std::move(val);
        return;
    }
    if (shape->field_ids.size() >= kDictModeThreshold) {
        promote_to_dict_mode();
        const std::string &name = field_name(fid);
        dict->map.emplace(name, std::move(val));
        dict->keys.push_back(name);
        shape_version++;
        return;
    }
    shape = object_shape_registry().extend(shape, fid);
    fields.push_back(std::move(val));
    shape_version++;
}

bool ObjectObj::has_field_by_id(uint32_t fid) const noexcept {
    if (dict_mode && dict) {
        return dict->map.count(field_name(fid)) != 0;
    }
    return shape->slot_of(fid) != ObjectShape::kNoSlot;
}

static const std::unordered_map<std::string, Value> g_empty_dict_fields;

const std::unordered_map<std::string, Value> &ObjectObj::dict_fields() const noexcept {
    return (dict_mode && dict) ? dict->map : g_empty_dict_fields;
}

bool ObjectObj::is_empty() const noexcept {
    if (dict_mode) {
        return !dict || dict->map.empty();
    }
    return fields.empty();
}

size_t ObjectObj::field_count() const noexcept {
    if (dict_mode) {
        return dict ? dict->map.size() : 0;
    }
    return fields.size();
}

// ClassInstance
ClassInstance::ClassInstance(std::string cn) : class_name(std::move(cn)) {
    type_tag = ValueTag::ClassInstance;
}

Value *ClassInstance::get_field(const std::string &name) noexcept {
    if (!layout) {
        return nullptr;
    }
    auto fit = layout->index.find(name);
    if (fit == layout->index.end()) {
        return nullptr;
    }
    uint32_t idx = fit->second;
    if (idx >= (uint32_t)field_values.size()) {
        return nullptr;
    }
    return &field_values[idx];
}

const Value *ClassInstance::get_field(const std::string &name) const noexcept {
    if (!layout) {
        return nullptr;
    }
    auto fit = layout->index.find(name);
    if (fit == layout->index.end()) {
        return nullptr;
    }
    uint32_t idx = fit->second;
    if (idx >= (uint32_t)field_values.size()) {
        return nullptr;
    }
    return &field_values[idx];
}

// ArrayObj
ArrayObj::ArrayObj() {
    type_tag = ValueTag::Array;
}

ArrayObj::~ArrayObj() = default;

const Value *ArrayObj::get_property(const std::string &name) const noexcept {
    return properties ? properties->get_field(name) : nullptr;
}

bool ArrayObj::has_property(const std::string &name) const noexcept {
    return properties && properties->has_field(name);
}

void ArrayObj::set_property(const std::string &name, Value value) {
    if (!properties) {
        properties = std::make_unique<ObjectObj>();
    }
    properties->set_field(name, std::move(value));
}

// FunctionData property bag
static uint32_t length_field_id() noexcept {
    static const uint32_t id = intern_field("length");
    return id;
}

ObjectObj *FunctionData::ensure_properties() {
    if (!properties) {
        properties = std::make_unique<ObjectObj>();
    }
    return properties.get();
}

const Value *FunctionData::get_property(const std::string &name) const noexcept {
    if (!int_length.is_none() && is_length_name(name)) {
        return &int_length;
    }
    return properties ? properties->get_field(name) : nullptr;
}

void FunctionData::set_property(const std::string &name, Value val) {
    if (val.is_int() && is_length_name(name)) {
        int_length = val;
        return;
    }
    ensure_properties()->set_field(name, std::move(val));
}

const Value *FunctionData::get_property_by_id(uint32_t fid) const noexcept {
    if (fid == length_field_id() && !int_length.is_none()) {
        return &int_length;
    }
    return properties ? properties->get_field_by_id(fid) : nullptr;
}

void FunctionData::set_property_by_id(uint32_t fid, Value val) {
    if (fid == length_field_id() && val.is_int()) {
        int_length = val;
        return;
    }
    ensure_properties()->set_field_by_id(fid, std::move(val));
}
